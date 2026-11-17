#include "runtime_log.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SignatureScanner.h"
#include "wmport_reader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr std::size_t chunk_size = 64 * 1024;
constexpr std::uint32_t max_image_size = 512 * 1024 * 1024;
constexpr std::size_t max_pattern_size = 4096;
constexpr std::size_t max_hit_limit = 65536;

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::vector<int>> parse_pattern(std::string_view text) {
    std::vector<int> result;
    while (!text.empty()) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        if (text.empty()) break;
        std::size_t length = 0;
        while (length < text.size() && !std::isspace(static_cast<unsigned char>(text[length]))) ++length;
        const auto token = text.substr(0, length);
        if (token == "?" || token == "??") {
            result.push_back(-1);
        } else if (token.size() == 2 && hex_digit(token[0]) >= 0 && hex_digit(token[1]) >= 0) {
            result.push_back(hex_digit(token[0]) * 16 + hex_digit(token[1]));
        } else {
            return std::nullopt;
        }
        if (result.size() > max_pattern_size) return std::nullopt;
        text.remove_prefix(length);
    }
    if (result.empty()) return std::nullopt;
    return result;
}
} // namespace

SignatureScanner::SignatureScanner(WmPortReader& reader, std::uint32_t pid, Address module_base)
    : reader_(reader), pid_(pid), base_(module_base) {
    static_assert(sizeof(void*) == 8, "SignatureScanner requires x64");
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    min_ = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    max_ = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    page_size_ = info.dwPageSize;
}

void SignatureScanner::log(const char* stage, Address address, std::uint32_t error) const {
    const DWORD saved = GetLastError();
    scarlex::diagnostic_printf("[SignatureScanner pid=%lu] %s addr=0x%llX err=%lu\n",
                static_cast<unsigned long>(pid_), stage, static_cast<unsigned long long>(address),
                static_cast<unsigned long>(error));
    SetLastError(saved);
}

bool SignatureScanner::pointer(Address address) const noexcept {
    return address >= min_ && address <= max_ && address != 0 && address % 8 == 0;
}

bool SignatureScanner::read(Address address, void* out, std::size_t size) const {
    if (pid_ == 0 || !out || size == 0 || address < min_ || address > max_ || size - 1 > max_ - address) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    if (!reader_.read(pid_, address, out, size)) {
        if (GetLastError() == ERROR_SUCCESS) SetLastError(ERROR_READ_FAULT);
        return false;
    }
    return true;
}

bool SignatureScanner::initialize() {
    sections_.clear();
    image_size_ = 0;
    timestamp_ = 0;
    const auto fail = [&](const char* stage, DWORD error) {
        log(stage, base_, error);
        sections_.clear();
        image_size_ = 0;
        timestamp_ = 0;
        SetLastError(error);
        return false;
    };
    if (pid_ == 0 || !pointer(base_) || page_size_ == 0) return fail("Invalid arguments", ERROR_INVALID_PARAMETER);
    try {
        IMAGE_DOS_HEADER dos{};
        if (!read(base_, &dos, sizeof(dos))) return fail("DOS header read", GetLastError());
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
            dos.e_lfanew > 0x100000 || static_cast<Address>(dos.e_lfanew) > max_ - base_) {
            return fail("Invalid DOS header", ERROR_BAD_EXE_FORMAT);
        }
        const Address nt = base_ + static_cast<Address>(dos.e_lfanew);
        DWORD signature{};
        IMAGE_FILE_HEADER file{};
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (!read(nt, &signature, sizeof(signature)) ||
            !read(nt + sizeof(signature), &file, sizeof(file))) return fail("PE header read", GetLastError());
        if (signature != IMAGE_NT_SIGNATURE || file.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            file.NumberOfSections == 0 || file.NumberOfSections > 96 ||
            file.SizeOfOptionalHeader < sizeof(optional) || file.SizeOfOptionalHeader > 0x1000) {
            return fail("Invalid x64 PE header", ERROR_BAD_EXE_FORMAT);
        }
        const Address optional_address = nt + sizeof(signature) + sizeof(file);
        if (!read(optional_address, &optional, sizeof(optional))) return fail("Optional header read", GetLastError());
        const Address table = optional_address + file.SizeOfOptionalHeader;
        const auto table_bytes = static_cast<std::size_t>(file.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional.SizeOfImage == 0 ||
            optional.SizeOfImage > max_image_size || optional.SizeOfImage - 1 > max_ - base_ ||
            optional.SizeOfHeaders == 0 || optional.SizeOfHeaders > optional.SizeOfImage ||
            table < base_ || table - base_ > optional.SizeOfHeaders ||
            table_bytes > optional.SizeOfHeaders - (table - base_)) {
            return fail("Invalid image/section bounds", ERROR_BAD_EXE_FORMAT);
        }
        std::vector<IMAGE_SECTION_HEADER> headers(file.NumberOfSections);
        if (!read(table, headers.data(), table_bytes)) return fail("Section table read", GetLastError());
        for (const auto& header : headers) {
            const std::uint32_t size = header.Misc.VirtualSize ? header.Misc.VirtualSize : header.SizeOfRawData;
            if (size == 0) continue;
            if (header.VirtualAddress < optional.SizeOfHeaders || header.VirtualAddress >= optional.SizeOfImage ||
                size > optional.SizeOfImage - header.VirtualAddress) {
                return fail("Invalid mapped section", ERROR_BAD_EXE_FORMAT);
            }
            const auto* name = reinterpret_cast<const char*>(header.Name);
            std::size_t length = 0;
            while (length < IMAGE_SIZEOF_SHORT_NAME && name[length] != '\0') ++length;
            sections_.push_back({std::string(name, length), base_ + header.VirtualAddress, size, header.Characteristics});
        }
        std::sort(sections_.begin(), sections_.end(), [](const Section& a, const Section& b) { return a.address < b.address; });
        for (std::size_t i = 1; i < sections_.size(); ++i) {
            if (sections_[i - 1].address + sections_[i - 1].size > sections_[i].address) {
                return fail("Overlapping mapped sections", ERROR_BAD_EXE_FORMAT);
            }
        }
        if (sections_.empty()) return fail("Empty section table", ERROR_BAD_EXE_FORMAT);
        image_size_ = optional.SizeOfImage;
        timestamp_ = file.TimeDateStamp;
        log("PE32+ initialized", base_);
        scarlex::diagnostic_printf("[SignatureScanner] SizeOfImage=0x%lX TimeDateStamp=0x%lX\n",
                    static_cast<unsigned long>(image_size_), static_cast<unsigned long>(timestamp_));
        for (const auto& section : sections_) {
            scarlex::diagnostic_printf("[SignatureScanner] section=%s rva=0x%llX size=0x%lX flags=0x%lX\n", section.name.c_str(),
                        static_cast<unsigned long long>(section.address - base_), static_cast<unsigned long>(section.size),
                        static_cast<unsigned long>(section.characteristics));
        }
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (const std::bad_alloc&) {
        return fail("Allocation", ERROR_OUTOFMEMORY);
    }
}

bool SignatureScanner::visit(Region region, const Visitor& visitor, Stats& stats) const {
    if (image_size_ == 0) { stats.error = ERROR_INVALID_STATE; return false; }
    std::vector<std::pair<Address, std::size_t>> ranges;
    if (region == Region::Image) {
        ranges.emplace_back(base_, image_size_);
    } else {
        for (const auto& section : sections_) {
            const bool selected = region == Region::Data ? section.name == ".data" :
                region == Region::Text ? section.name == ".text" && (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 :
                region == Region::Executable ? (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 :
                (section.characteristics & IMAGE_SCN_MEM_WRITE) != 0 && (section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0;
            if (selected) ranges.emplace_back(section.address, section.size);
        }
    }
    if (ranges.empty()) { stats.error = ERROR_NOT_FOUND; return false; }
    std::vector<std::uint8_t> buffer(chunk_size);
    for (const auto& [start, size] : ranges) {
        for (std::size_t offset = 0; offset < size;) {
            const std::size_t count = (std::min)(chunk_size, size - offset);
            const Address address = start + offset;
            if (read(address, buffer.data(), count)) {
                stats.readable += count;
                if (!visitor(address, {buffer.data(), count})) return false;
            } else {
                // Discard the partially filled buffer. Retry page-sized reads so
                // one inaccessible page does not hide the rest of a 64 KiB block.
                for (std::size_t done = 0; done < count;) {
                    const Address page_address = address + done;
                    const std::size_t part = (std::min)(count - done,
                        static_cast<std::size_t>(page_size_ - page_address % page_size_));
                    if (read(page_address, buffer.data(), part)) {
                        stats.readable += part;
                        if (!visitor(page_address, {buffer.data(), part})) return false;
                    } else {
                        stats.unreadable += part;
                        // Notify the matcher of a gap: never match across it.
                        if (!visitor(page_address, {})) return false;
                    }
                    done += part;
                }
            }
            offset += count;
        }
    }
    return true;
}

SignatureScanner::ScanResult SignatureScanner::scan_bytes(const std::vector<int>& pattern, Region region,
                                                         std::size_t max_hits, std::size_t alignment) const {
    ScanResult result;
    if (pattern.empty() || pattern.size() > max_pattern_size || max_hits == 0 || max_hits > max_hit_limit) {
        result.error = ERROR_INVALID_PARAMETER;
        SetLastError(result.error);
        return result;
    }
    Stats stats;
    try {
        std::vector<std::uint8_t> tail, joined;
        Address expected_next{};
        joined.reserve(chunk_size + pattern.size());
        (void)visit(region, [&](Address address, std::span<const std::uint8_t> bytes) {
            if (bytes.empty()) { tail.clear(); expected_next = 0; return true; }
            if (address != expected_next) tail.clear();
            const Address start = address - tail.size();
            joined.assign(tail.begin(), tail.end());
            joined.insert(joined.end(), bytes.begin(), bytes.end());
            if (joined.size() >= pattern.size()) {
                for (std::size_t i = 0; i <= joined.size() - pattern.size(); ++i) {
                    if ((start + i) % alignment != 0) continue;
                    bool match = true;
                    for (std::size_t j = 0; j < pattern.size(); ++j) {
                        if (pattern[j] >= 0 && joined[i + j] != pattern[j]) { match = false; break; }
                    }
                    if (match) {
                        result.hits.push_back(start + i);
                        if (result.hits.size() == max_hits) { result.truncated = true; return false; }
                    }
                }
            }
            const auto keep = (std::min)(pattern.size() - 1, joined.size());
            tail.assign(joined.end() - static_cast<std::ptrdiff_t>(keep), joined.end());
            expected_next = address + bytes.size();
            return true;
        }, stats);
        result.error = stats.error;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_OUTOFMEMORY;
        result.hits.clear();
    }
    result.readable_bytes = stats.readable;
    result.unreadable_bytes = stats.unreadable;
    scarlex::diagnostic_printf("[SignatureScanner] scan hits=%zu readable=%llu unreadable=%llu truncated=%d err=%lu\n",
                result.hits.size(), static_cast<unsigned long long>(result.readable_bytes),
                static_cast<unsigned long long>(result.unreadable_bytes), static_cast<int>(result.truncated),
                static_cast<unsigned long>(result.error));
    SetLastError(result.error);
    return result;
}

SignatureScanner::ScanResult SignatureScanner::scan_pattern(std::string_view pattern, Region region,
                                                           std::size_t max_hits) const {
    try {
        const auto parsed = parse_pattern(pattern);
        if (parsed) return scan_bytes(*parsed, region, max_hits, 1);
        SetLastError(ERROR_INVALID_PARAMETER);
        ScanResult result; result.error = ERROR_INVALID_PARAMETER; return result;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_OUTOFMEMORY);
        ScanResult result; result.error = ERROR_OUTOFMEMORY; return result;
    }
}

SignatureScanner::ScanResult SignatureScanner::scan_string(std::string_view text, Region region,
                                                          std::size_t max_hits, bool include_nul) const {
    try {
        if (text.empty() || text.size() > max_pattern_size - (include_nul ? 1 : 0)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            ScanResult result; result.error = ERROR_INVALID_PARAMETER; return result;
        }
        std::vector<int> pattern;
        pattern.reserve(text.size() + 1);
        for (const unsigned char byte : text) pattern.push_back(byte);
        if (include_nul) pattern.push_back(0);
        return scan_bytes(pattern, region, max_hits, 1);
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_OUTOFMEMORY);
        ScanResult result; result.error = ERROR_OUTOFMEMORY; return result;
    }
}

SignatureScanner::ScanResult SignatureScanner::scan_pointer(Address target, Region region,
                                                           std::size_t max_hits) const {
    try {
        std::vector<int> pattern(sizeof(Address));
        for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<int>((target >> (8 * i)) & 0xFF);
        return scan_bytes(pattern, region, max_hits, sizeof(Address));
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_OUTOFMEMORY);
        ScanResult result; result.error = ERROR_OUTOFMEMORY; return result;
    }
}

std::optional<SignatureScanner::Address> SignatureScanner::resolve_rip_relative(
    Address instruction, std::size_t displacement_offset, std::size_t instruction_size) const {
    if (image_size_ == 0 || instruction < base_ || instruction - base_ >= image_size_ ||
        instruction_size < sizeof(std::int32_t) || instruction_size > 15 ||
        displacement_offset > instruction_size - sizeof(std::int32_t) ||
        instruction_size > image_size_ - (instruction - base_)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return std::nullopt;
    }
    std::int32_t displacement{};
    if (!read(instruction + displacement_offset, &displacement, sizeof(displacement))) return std::nullopt;
    const Address next = instruction + instruction_size;
    Address target{};
    if (displacement >= 0) {
        const auto delta = static_cast<Address>(displacement);
        if (next > max_ || delta > max_ - next) { SetLastError(ERROR_INVALID_ADDRESS); return std::nullopt; }
        target = next + delta;
    } else {
        const auto delta = static_cast<Address>(-static_cast<std::int64_t>(displacement));
        if (delta > next || next - delta < min_) { SetLastError(ERROR_INVALID_ADDRESS); return std::nullopt; }
        target = next - delta;
    }
    SetLastError(ERROR_SUCCESS);
    return target;
}

bool SignatureScanner::contains(Region region, Address address, std::size_t size) const {
    if (size == 0 || image_size_ == 0 || address < base_ || address - base_ >= image_size_ ||
        size > image_size_ - (address - base_)) return false;
    if (region == Region::Image) return true;
    for (const auto& section : sections_) {
        const bool selected = region == Region::Data ? section.name == ".data" :
            region == Region::Text ? section.name == ".text" && (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 :
            region == Region::Executable ? (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 :
            (section.characteristics & IMAGE_SCN_MEM_WRITE) != 0 && (section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0;
        if (selected && address >= section.address && address - section.address < section.size &&
            size <= section.size - (address - section.address)) return true;
    }
    return false;
}

std::optional<std::string> SignatureScanner::read_name(Address address, std::size_t max_bytes) const {
    // Native NUL-terminated name, NOT Il2CppString. Read only up to the terminator;
    // never require a whole fixed-size buffer past the string to be accessible.
    if (address < min_ || address > max_) return std::nullopt;
    std::string name;
    for (std::size_t i = 0; i < max_bytes; ++i) {
        unsigned char byte{};
        if (i > max_ - address || !read(address + i, &byte, sizeof(byte))) return std::nullopt;
        if (byte == 0) return name.empty() ? std::nullopt : std::optional<std::string>(std::move(name));
        // Reject control bytes to avoid logging binary garbage or terminal escapes.
        if (byte < 0x20 || byte == 0x7F) return std::nullopt;
        name.push_back(static_cast<char>(byte));
    }
    return std::nullopt; // Truncated names must not count as exact matches.
}

bool SignatureScanner::valid_gom_options(const GomOptions& options) const {
    if (options.max_candidates == 0 || options.max_candidates > max_hit_limit ||
        options.max_nodes_per_list == 0 || options.max_manager_probes == 0 || options.max_total_nodes == 0 ||
        options.max_name_bytes < 2 || options.max_name_bytes > 4096 || options.dump_name_limit > 1000 ||
        options.list_first_offsets.empty() || options.list_first_offsets.size() > 64 ||
        options.game_object_name_offset > 0x1000 || options.max_name_bytes <= sizeof("UnityEntry") - 1 ||
        options.max_components_per_object == 0 || options.max_components_per_object > 4096 ||
        options.max_component_slots == 0) return false;
    for (const auto offset : options.list_first_offsets) {
        if (offset < 8 || offset > 0x1000 || offset % 8 != 0) return false;
    }
    return true;
}

bool SignatureScanner::entry_component_class(Address object, std::uint32_t index,
                                             Address component, const char* source,
                                             const GomOptions& options, GomResult& result) const {
    if (!pointer(component)) return false;
    Address klass{}, name_address{};
    std::optional<std::string> name;
    if (read(component, &klass, sizeof(klass)) && pointer(klass) && klass <= max_ - 0x10 &&
        read(klass + 0x10, &name_address, sizeof(name_address))) {
        name = read_name(name_address, options.max_name_bytes);
    }
    if (name) ++result.readable_component_classes;
    else ++result.component_class_failures;
    if (options.log_component_classes) {
        const DWORD error = GetLastError();
        scarlex::diagnostic_printf("[SignatureScanner] CLASS object=0x%llX index=%lu source=%s component=0x%llX klass=0x%llX class=\"%s\" expected=\"UnityEntry\" match=%d\n",
                    static_cast<unsigned long long>(object), static_cast<unsigned long>(index), source,
                    static_cast<unsigned long long>(component), static_cast<unsigned long long>(klass),
                    name ? name->c_str() : "<unreadable/invalid/truncated>", static_cast<int>(name && *name == "UnityEntry"));
        SetLastError(error);
    }
    return name && *name == "UnityEntry"; // No prefix, aliases, or case folding.
}

std::optional<SignatureScanner::Address> SignatureScanner::find_entry_component(
    Address object, const GomOptions& options, GomResult& result) const {
    if (!pointer(object) || object > max_ - 0x30) return std::nullopt;
    Address components{};
    std::uint32_t count{};
    if (!read(object + 0x30, &count, sizeof(count)) || count > options.max_components_per_object ||
        count == 0 || !read(object + 0x20, &components, sizeof(components)) || !pointer(components)) return std::nullopt;
    if (options.log_component_classes) {
        scarlex::diagnostic_printf("[SignatureScanner] COMPONENTS object=0x%llX array=0x%llX count=%lu\n",
                    static_cast<unsigned long long>(object), static_cast<unsigned long long>(components),
                    static_cast<unsigned long>(count));
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (result.component_slots >= options.max_component_slots) { result.limited = true; return std::nullopt; }
        ++result.component_slots;
        const Address offset = 0x10ULL * i + 8;
        Address component{};
        if (offset > max_ - components || !read(components + offset, &component, sizeof(component)) ||
            !pointer(component)) continue;
        // Managed array-slot references are supported explicitly. Native Unity
        // components contain a vtable at +0, not klass, and must be unwrapped.
        if (entry_component_class(object, i, component, "array-slot", options, result)) return component;
        Address tagged{}, candidate{};
        if (component <= max_ - 0x18 && read(component + 0x18, &tagged, sizeof(tagged))) {
            const Address reference = tagged & ~Address{7};
            if (pointer(reference) && read(reference, &candidate, sizeof(candidate)) &&
                entry_component_class(object, i, candidate, "native+0x18 indirect", options, result)) return candidate;
        }
        candidate = 0;
        if (component <= max_ - 0x28 && read(component + 0x28, &candidate, sizeof(candidate)) &&
            entry_component_class(object, i, candidate, "native+0x28 direct", options, result)) return candidate;
    }
    return std::nullopt;
}

std::optional<SignatureScanner::GomCandidate> SignatureScanner::probe_gom(
    Address manager, const GomOptions& options, GomResult& result) const {
    // x64 native node: previous +0, next +8, GameObject +0x10.
    // No GameObject name filter: scan each object's full component array.
    for (const auto first_offset : options.list_first_offsets) {
        if (first_offset > max_ - manager) continue;
        std::array<Address, 2> endpoints{}; // {last, first}
        if (!read(manager + first_offset - 8, endpoints.data(), sizeof(endpoints))) continue;
        Address node = endpoints[1];
        const Address last = endpoints[0];
        if (!pointer(node) || (last != 0 && !pointer(last))) continue;
        std::unordered_set<Address> seen;
        while (node != 0) {
            if (seen.size() >= options.max_nodes_per_list || result.visited_nodes >= options.max_total_nodes) {
                result.limited = true;
                break;
            }
            if (!seen.insert(node).second) break;
            ++result.visited_nodes;
            std::array<Address, 3> links{};
            if (!read(node, links.data(), sizeof(links))) break;
            const auto component = pointer(links[2]) ? find_entry_component(links[2], options, result) : std::nullopt;
            if (component) {
                GomCandidate candidate;
                candidate.manager = manager;
                candidate.entry_object = links[2];
                candidate.entry_component = *component;
                candidate.first_offset = first_offset;
                candidate.visited_nodes = seen.size();
                candidate.entry_name = "UnityEntry";
                return candidate;
            }
            // Name samples are optional diagnostics only; even an invalid name
            // pointer never prevents trying this object's components or next node.
            Address name_address{};
            std::optional<std::string> name;
            if (pointer(links[2]) && links[2] <= max_ - options.game_object_name_offset &&
                read(links[2] + options.game_object_name_offset, &name_address, sizeof(name_address))) {
                name = read_name(name_address, options.max_name_bytes);
            }
            if (name) {
                ++result.readable_names;
                if (result.sampled_names.size() < options.dump_name_limit &&
                    std::none_of(result.sampled_names.begin(), result.sampled_names.end(),
                                 [&](const ObjectName& sample) { return sample.object == links[2]; })) {
                    result.sampled_names.push_back({manager, node, links[2], name_address, first_offset, *name});
                }
            } else {
                ++result.name_read_failures;
            }
            if (result.component_slots >= options.max_component_slots) { result.limited = true; return std::nullopt; }
            if (node == last) break; // Last node is live, not an excluded sentinel.
            if (links[1] != 0 && !pointer(links[1])) break;
            node = links[1];
        }
    }
    return std::nullopt;
}

bool SignatureScanner::record_candidate(Address slot, const GomCandidate& source,
                                       const GomOptions& options, GomResult& result) const {
    GomCandidate candidate = source;
    candidate.slot = slot;
    candidate.rva = slot - base_;
    Address current{};
    if (!read(slot, &current, sizeof(current)) || current != candidate.manager) return true;
    result.candidates.push_back(candidate);
    scarlex::diagnostic_printf("[SignatureScanner] GOM CANDIDATE slot=0x%llX rva=0x%llX manager=0x%llX object=0x%llX component=0x%llX class=\"%s\" first_offset=0x%llX nodes=%zu\n",
                static_cast<unsigned long long>(slot), static_cast<unsigned long long>(candidate.rva),
                static_cast<unsigned long long>(candidate.manager), static_cast<unsigned long long>(candidate.entry_object),
                static_cast<unsigned long long>(candidate.entry_component), candidate.entry_name.c_str(),
                static_cast<unsigned long long>(candidate.first_offset), candidate.visited_nodes);
    if (result.candidates.size() == options.max_candidates) { result.limited = true; return false; }
    return true;
}

void SignatureScanner::finish_gom_result(const GomResult& result, const char* source) const {
    scarlex::diagnostic_printf("[SignatureScanner] GOM scan source=%s candidates=%zu manager_probes=%zu nodes=%zu component_slots=%zu readable_classes=%zu class_failures=%zu readable_names=%zu name_failures=%zu unreadable=%llu limited=%d err=%lu\n",
                source, result.candidates.size(), result.manager_probes, result.visited_nodes,
                result.component_slots, result.readable_component_classes, result.component_class_failures,
                result.readable_names, result.name_read_failures, static_cast<unsigned long long>(result.unreadable_bytes),
                static_cast<int>(result.limited), static_cast<unsigned long>(result.error));
    if (result.candidates.empty()) {
        scarlex::diagnostic_printf("[SignatureScanner] NAME DUMP samples=%zu (objects from candidate native lists; not confirmed GOM)\n",
                    result.sampled_names.size());
        for (std::size_t i = 0; i < result.sampled_names.size(); ++i) {
            const auto& sample = result.sampled_names[i];
            scarlex::diagnostic_printf("[SignatureScanner] NAME[%zu] manager=0x%llX first_offset=0x%llX node=0x%llX object=0x%llX name_ptr=0x%llX name=\"%s\"\n",
                        i, static_cast<unsigned long long>(sample.manager), static_cast<unsigned long long>(sample.first_offset),
                        static_cast<unsigned long long>(sample.node), static_cast<unsigned long long>(sample.object),
                        static_cast<unsigned long long>(sample.name_address), sample.name.c_str());
        }
        if (result.sampled_names.empty()) {
            scarlex::diagnostic_printf("[SignatureScanner] No readable names: inspect native list/node/name layouts or scene readiness.\n");
        }
    }
    SetLastError(result.error);
}

SignatureScanner::GomResult SignatureScanner::find_gom_candidates(const GomOptions& options) const {
    GomResult result;
    if (!valid_gom_options(options)) {
        result.error = ERROR_INVALID_PARAMETER;
        SetLastError(result.error);
        return result;
    }
    Stats stats;
    try {
        std::unordered_map<Address, std::optional<GomCandidate>> tested;
        (void)visit(options.region, [&](Address address, std::span<const std::uint8_t> bytes) {
            const auto alignment = static_cast<std::size_t>((8 - address % 8) % 8);
            for (std::size_t offset = alignment; offset + sizeof(Address) <= bytes.size(); offset += sizeof(Address)) {
                Address manager{};
                std::memcpy(&manager, bytes.data() + offset, sizeof(manager));
                if (!pointer(manager)) continue;
                auto it = tested.find(manager);
                if (it == tested.end()) {
                    if (result.manager_probes >= options.max_manager_probes || result.visited_nodes >= options.max_total_nodes ||
                        result.component_slots >= options.max_component_slots) {
                        result.limited = true;
                        return false;
                    }
                    ++result.manager_probes;
                    const auto candidate = probe_gom(manager, options, result);
                    it = tested.emplace(manager, candidate).first;
                }
                if (!it->second) continue;
                if (!record_candidate(address + offset, *it->second, options, result)) return false;
            }
            return true;
        }, stats);
        result.error = stats.error;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_OUTOFMEMORY;
        result.candidates.clear();
    }
    result.readable_bytes = stats.readable;
    result.unreadable_bytes = stats.unreadable;
    finish_gom_result(result, "data");
    return result;
}

SignatureScanner::RipLoadResult SignatureScanner::scan_rip_loads(Region target_region,
                                                               std::size_t max_loads, std::size_t log_limit) const {
    RipLoadResult result;
    if ((target_region != Region::Data && target_region != Region::WritableData) ||
        max_loads == 0 || max_loads > max_hit_limit || log_limit > 1000) {
        result.error = ERROR_INVALID_PARAMETER;
        SetLastError(result.error);
        return result;
    }
    if (image_size_ == 0) {
        result.error = ERROR_INVALID_STATE;
        SetLastError(result.error);
        return result;
    }
    const bool have_target = std::any_of(sections_.begin(), sections_.end(), [&](const Section& section) {
        return contains(target_region, section.address, sizeof(Address));
    });
    if (!have_target) { result.error = ERROR_NOT_FOUND; SetLastError(result.error); return result; }
    Stats stats;
    try {
        std::vector<std::uint8_t> tail, joined;
        joined.reserve(chunk_size + 6);
        Address expected_next{};
        (void)visit(Region::Text, [&](Address address, std::span<const std::uint8_t> bytes) {
            if (bytes.empty()) { tail.clear(); expected_next = 0; return true; }
            if (address != expected_next) tail.clear();
            const Address start = address - tail.size();
            joined.assign(tail.begin(), tail.end());
            joined.insert(joined.end(), bytes.begin(), bytes.end());
            for (std::size_t i = 0; i + 7 <= joined.size(); ++i) {
                if (joined[i] != 0x48 || joined[i + 1] != 0x8B ||
                    (joined[i + 2] != 0x05 && joined[i + 2] != 0x0D)) continue;
                const Address instruction = start + i;
                if (!contains(Region::Text, instruction, 7)) continue;
                ++result.byte_matches;
                std::int32_t displacement{};
                std::memcpy(&displacement, joined.data() + i + 3, sizeof(displacement));
                const auto signed_target = static_cast<std::int64_t>(instruction + 7) + displacement;
                if (signed_target < 0) continue;
                const auto slot = static_cast<Address>(signed_target);
                if (!contains(target_region, slot, sizeof(Address))) continue;
                RipLoad load;
                load.instruction = instruction;
                load.slot = slot;
                load.rva = slot - base_;
                load.loads_rcx = joined[i + 2] == 0x0D;
                load.value_readable = read(slot, &load.value, sizeof(load.value));
                if (!load.value_readable) {
                    load.value = 0; // Discard any partial buffer, even for diagnostic display.
                    load.read_error = GetLastError();
                    ++result.slot_read_failures;
                }
                result.loads.push_back(load);
                if (result.loads.size() <= log_limit) {
                    scarlex::diagnostic_printf("[SignatureScanner] RIP LOAD instruction=0x%llX reg=%s slot=0x%llX rva=0x%llX value=0x%llX readable=%d err=%lu\n",
                                static_cast<unsigned long long>(instruction), load.loads_rcx ? "rcx" : "rax",
                                static_cast<unsigned long long>(slot), static_cast<unsigned long long>(load.rva),
                                static_cast<unsigned long long>(load.value), static_cast<int>(load.value_readable),
                                static_cast<unsigned long>(load.read_error));
                }
                if (result.loads.size() == max_loads) { result.limited = true; return false; }
            }
            const auto keep = (std::min)(std::size_t{6}, joined.size());
            tail.assign(joined.end() - static_cast<std::ptrdiff_t>(keep), joined.end());
            expected_next = address + bytes.size();
            return true;
        }, stats);
        result.error = stats.error;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_OUTOFMEMORY;
        result.loads.clear();
    }
    result.readable_bytes = stats.readable;
    result.unreadable_bytes = stats.unreadable;
    scarlex::diagnostic_printf("[SignatureScanner] RIP scan byte_matches=%zu data_loads=%zu slot_read_failures=%zu unreadable_text=%llu limited=%d err=%lu\n",
                result.byte_matches, result.loads.size(), result.slot_read_failures,
                static_cast<unsigned long long>(result.unreadable_bytes), static_cast<int>(result.limited),
                static_cast<unsigned long>(result.error));
    SetLastError(result.error);
    return result;
}

SignatureScanner::GomResult SignatureScanner::find_gom_candidates_from_text(const GomOptions& options,
                                                                          std::size_t max_loads) const {
    GomResult result;
    if (!valid_gom_options(options)) {
        result.error = ERROR_INVALID_PARAMETER;
        SetLastError(result.error);
        return result;
    }
    const auto rip = scan_rip_loads(options.region, max_loads);
    result.error = rip.error;
    result.limited = rip.limited;
    result.readable_bytes = rip.readable_bytes;
    result.unreadable_bytes = rip.unreadable_bytes;
    result.rip_loads = rip.loads.size();
    result.slot_read_failures = rip.slot_read_failures;
    if (result.error == 0) {
        try {
            std::unordered_set<Address> slots;
            std::unordered_map<Address, std::optional<GomCandidate>> tested;
            for (const auto& load : rip.loads) {
                if (!load.value_readable || !slots.insert(load.slot).second) continue;
                // Reread immediately before structural probing; cached scan values can change.
                Address manager{};
                if (!read(load.slot, &manager, sizeof(manager))) { ++result.slot_read_failures; continue; }
                if (!pointer(manager)) continue;
                auto it = tested.find(manager);
                if (it == tested.end()) {
                    if (result.manager_probes >= options.max_manager_probes || result.visited_nodes >= options.max_total_nodes ||
                        result.component_slots >= options.max_component_slots) {
                        result.limited = true;
                        break;
                    }
                    ++result.manager_probes;
                    it = tested.emplace(manager, probe_gom(manager, options, result)).first;
                }
                if (it->second && !record_candidate(load.slot, *it->second, options, result)) break;
            }
        } catch (const std::bad_alloc&) {
            result.error = ERROR_OUTOFMEMORY;
            result.candidates.clear();
        }
    }
    finish_gom_result(result, "text-rip");
    return result;
}

