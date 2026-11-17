#include "runtime_log.h"
#include "ClassScanner.h"
#include "app/offsets.h"

#include "SignatureScanner.h"
#include "wmport_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Address = ClassScanner::Address;
constexpr Address unity_entry_game_offset = 0x28;
constexpr std::size_t read_chunk_size = 64 * 1024;

// il2cpp_image_get_class -> class-by-type-definition-index. The MOV loads
// the cache table base, and the following LEA selects an entry with index * 8.
constexpr auto class_table_aob =
    "48 8B 05 ?? ?? ?? ?? 4C 8D 34 F0 49 8B 1E 90 48 85 DB 0F 85 ?? ?? ?? ??";

struct ProcessCloser {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};

struct AddressRange {
    Address min;
    Address max;

    [[nodiscard]] bool contains(Address address, std::size_t size) const noexcept {
        return size != 0 && address >= min && address <= max && size - 1 <= max - address;
    }

    [[nodiscard]] bool pointer(Address address) const noexcept {
        return address % sizeof(Address) == 0 && contains(address, sizeof(Address));
    }
};

struct EntryHeader {
    Address klass;
    Address monitor;
    Address cached_native_object;
    std::array<Address, 2> intervening_fields;
    Address game;
};

static_assert(sizeof(void*) == sizeof(Address), "ClassScanner requires x64 Windows");
static_assert(offsetof(EntryHeader, cached_native_object) == 0x10);
static_assert(offsetof(EntryHeader, game) == unity_entry_game_offset);
static_assert(sizeof(EntryHeader) == 0x30);

std::optional<ClassScanner::Match> fail(const char* stage, DWORD error) {
    scarlex::diagnostic_fprintf(stderr, "[ClassScanner] %s (Win32=%lu)\n", stage, static_cast<unsigned long>(error));
    SetLastError(error);
    return std::nullopt;
}

bool is_writable_private(const MEMORY_BASIC_INFORMATION& info) noexcept {
    return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
           (info.Protect & PAGE_GUARD) == 0 && (info.Protect & 0xff) == PAGE_READWRITE;
}

bool query(HANDLE process, Address address, MEMORY_BASIC_INFORMATION& info) {
    return VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) ==
           sizeof(info);
}

bool readable_object(HANDLE process, Address address, std::size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!query(process, address, info) || !is_writable_private(info)) {
        return false;
    }
    const auto base = reinterpret_cast<Address>(info.BaseAddress);
    return address >= base && address - base <= info.RegionSize &&
           size <= info.RegionSize - (address - base);
}

bool class_string(const WmPortReader& reader, DWORD pid, const AddressRange& range,
                  Address field, std::string_view expected) {
    Address text{};
    std::array<char, 32> bytes{};
    const std::size_t length = expected.size() + 1;
    return length <= bytes.size() && reader.read(pid, field, &text, sizeof(text)) &&
           range.contains(text, length) && reader.read(pid, text, bytes.data(), length) &&
           bytes[expected.size()] == '\0' &&
           std::memcmp(bytes.data(), expected.data(), expected.size()) == 0;
}

bool is_named_class(const WmPortReader& reader, DWORD pid, const AddressRange& range,
                    Address klass, std::string_view name, std::string_view namespc) {
    // il2cpp_class_get_name / il2cpp_class_get_namespace in the profiled build.
    return range.pointer(klass) && range.contains(klass, 0x20) &&
           class_string(reader, pid, range, klass + 0x10, name) &&
           class_string(reader, pid, range, klass + 0x18, namespc);
}

std::optional<Address> inspect_chunk(const WmPortReader& reader, DWORD pid, HANDLE process,
                                     const AddressRange& range, Address start, Address region_end,
                                     std::span<const Address> words, Address entry_class,
                                     Address game_class, Address& game) {
    auto found = std::find(words.begin(), words.end(), entry_class);
    while (found != words.end()) {
        const auto index = static_cast<Address>(found - words.begin());
        const Address candidate = start + index * sizeof(Address);
        EntryHeader entry{};
        Address current_game_class{}, current_game{}, current_entry_class{};
        // m_CachedPtr can be zero on a destroyed UnityEngine.Object.
        if (candidate <= region_end && sizeof(entry) <= region_end - candidate &&
            reader.read(pid, candidate, &entry, sizeof(entry)) && entry.klass == entry_class &&
            range.pointer(entry.cached_native_object) && range.pointer(entry.game) &&
            readable_object(process, entry.game, sizeof(Address)) &&
            reader.read(pid, entry.game, &current_game_class, sizeof(current_game_class)) &&
            current_game_class == game_class &&
            reader.read(pid, candidate, &current_entry_class, sizeof(current_entry_class)) &&
            current_entry_class == entry_class &&
            reader.read(pid, candidate + unity_entry_game_offset, &current_game, sizeof(current_game)) &&
            current_game == entry.game) {
            game = entry.game;
            return candidate;
        }
        found = std::find(found + 1, words.end(), entry_class);
    }
    return std::nullopt;
}

}  // namespace

std::optional<ClassScanner::Match> ClassScanner::find_unity_entry() const {
    if (pid_ == 0) {
        return fail("invalid PID", ERROR_INVALID_PARAMETER);
    }

    try {
        std::unique_ptr<void, ProcessCloser> process(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid_));
        if (!process) {
            return fail("OpenProcess(PROCESS_QUERY_INFORMATION)", GetLastError());
        }
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const AddressRange range{reinterpret_cast<Address>(system.lpMinimumApplicationAddress),
                                 reinterpret_cast<Address>(system.lpMaximumApplicationAddress)};
        const std::size_t page_size = system.dwPageSize;
        if (page_size == 0 || page_size > read_chunk_size || page_size % sizeof(Address) != 0) {
            return fail("unexpected system page size", ERROR_NOT_SUPPORTED);
        }

        const auto module = reader_.module_base(pid_, "GameAssembly.dll");
        if (!module) {
            return fail("locating GameAssembly.dll", GetLastError());
        }
        SignatureScanner scanner(reader_, pid_, *module);
        if (!scanner.initialize()) {
            return fail("reading GameAssembly.dll PE headers", GetLastError());
        }
        const auto& profiles = rob_run_offsets::assembly_profiles;
        const auto profile = std::find_if(std::begin(profiles), std::end(profiles), [&](const auto& candidate) {
            return scanner.timestamp() == candidate.timestamp && scanner.image_size() == candidate.image_size;
        });
        if (profile == std::end(profiles)) {
            scarlex::diagnostic_fprintf(stderr, "[ClassScanner] PE timestamp=0x%08lX image_size=0x%08lX\n",
                         static_cast<unsigned long>(scanner.timestamp()),
                         static_cast<unsigned long>(scanner.image_size()));
            return fail("GameAssembly build has no matching metadata-index profile", ERROR_REVISION_MISMATCH);
        }
        const Address unity_entry_index = profile->unity_entry_index;
        const Address game_index = profile->game_index;

        const auto matches = scanner.scan_pattern(class_table_aob, SignatureScanner::Region::Text, 2);
        if (matches.error != ERROR_SUCCESS) {
            return fail("scanning for class cache accessor", matches.error);
        }
        if (matches.truncated || matches.hits.size() > 1) {
            return fail("ambiguous class cache accessor signature", ERROR_INVALID_DATA);
        }
        if (matches.unreadable_bytes != 0) {
            return fail("incomplete .text scan for class cache accessor", ERROR_PARTIAL_COPY);
        }
        if (matches.hits.empty()) {
            return fail("class cache accessor signature missing", ERROR_NOT_FOUND);
        }
        const auto global = scanner.resolve_rip_relative(matches.hits.front(), 3, 7);
        if (!global) {
            return fail("resolving class table global RVA", GetLastError());
        }
        const bool in_data = std::any_of(scanner.sections().begin(), scanner.sections().end(),
            [slot = *global](const SignatureScanner::Section& section) {
                return section.name == ".data" &&
                       (section.characteristics & IMAGE_SCN_MEM_WRITE) != 0 &&
                       slot >= section.address && slot - section.address <= section.size &&
                       sizeof(Address) <= section.size - (slot - section.address);
            });
        if (!in_data || !range.pointer(*global)) {
            return fail("class table global lies outside writable GameAssembly .data", ERROR_INVALID_DATA);
        }

        Address table{}, entry_class{}, game_class{};
        const Address global_rva = *global - *module;
        scarlex::diagnostic_printf("[ClassScanner] GameAssembly=0x%llX class_table_global_rva=0x%llX\n",
                    static_cast<unsigned long long>(*module), static_cast<unsigned long long>(global_rva));
        if (!reader_.read(pid_, *global, &table, sizeof(table))) {
            return fail("reading class table pointer", GetLastError());
        }
        const auto last_index = (std::max)(unity_entry_index, game_index);
        if (!range.pointer(table) || !range.contains(table, (last_index + 1) * sizeof(Address))) {
            return fail("class table not initialized or invalid", ERROR_NOT_READY);
        }
        if (!reader_.read(pid_, table + unity_entry_index * sizeof(Address), &entry_class, sizeof(entry_class)) ||
            !reader_.read(pid_, table + game_index * sizeof(Address), &game_class, sizeof(game_class))) {
            return fail("reading IL2CPP class cache entries", GetLastError());
        }
        if (entry_class == 0 || game_class == 0) {
            return fail("UnityEntry or Game class cache entry is not initialized yet", ERROR_NOT_READY);
        }
        if (!is_named_class(reader_, pid_, range, entry_class, "UnityEntry", "Start") ||
            !is_named_class(reader_, pid_, range, game_class, "Game", "Game")) {
            return fail("IL2CPP class cache entry failed name and namespace validation", ERROR_INVALID_DATA);
        }
        scarlex::diagnostic_printf("[ClassScanner] Start.UnityEntry klass=0x%llX Game.Game klass=0x%llX\n",
                    static_cast<unsigned long long>(entry_class), static_cast<unsigned long long>(game_class));

        std::vector<Address> words(read_chunk_size / sizeof(Address));
        std::uint64_t read_bytes{}, skipped_bytes{};
        Address cursor = range.min;
        Match result{0, 0, entry_class, global_rva};
        for (; cursor <= range.max && result.entry == 0;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!query(process.get(), cursor, info)) {
                return fail("VirtualQueryEx while enumerating private pages", GetLastError());
            }
            const auto base = reinterpret_cast<Address>(info.BaseAddress);
            if (base > cursor || info.RegionSize == 0 || info.RegionSize > range.max + 1 - base ||
                base + info.RegionSize <= cursor) {
                return fail("invalid virtual memory region", ERROR_INVALID_DATA);
            }
            const Address end = base + info.RegionSize;
            if (is_writable_private(info)) {
                // VirtualQueryEx may describe a larger region than the current cursor.
                for (Address address = cursor; address < end && result.entry == 0;) {
                    const auto count = static_cast<std::size_t>((std::min)(end - address,
                                                                           Address{read_chunk_size}));
                    const auto inspect = [&](Address start, std::size_t bytes) {
                        read_bytes += bytes;
                        const auto candidate = inspect_chunk(reader_, pid_, process.get(), range, start, end,
                            std::span<const Address>(words.data(), bytes / sizeof(Address)),
                            entry_class, game_class, result.game);
                        if (candidate) {
                            result.entry = *candidate;
                        }
                    };
                    if (reader_.read(pid_, address, words.data(), count)) {
                        inspect(address, count);
                    } else {
                        const DWORD error = GetLastError();
                        if (error == ERROR_INVALID_HANDLE || error == ERROR_INVALID_PARAMETER) {
                            return fail("reading private memory", error);
                        }
                        // A failed driver read may leave a partial prefix. Discard it,
                        // re-query pages, and inspect only individually successful reads.
                        for (std::size_t offset = 0; offset < count && result.entry == 0;) {
                            const Address page = address + offset;
                            const auto part = (std::min)(count - offset,
                                page_size - static_cast<std::size_t>(page % page_size));
                            if (readable_object(process.get(), page, part) &&
                                reader_.read(pid_, page, words.data(), part)) {
                                inspect(page, part);
                            } else {
                                skipped_bytes += part;
                            }
                            offset += part;
                        }
                    }
                    address += count;
                }
            }
            cursor = end;
        }

        scarlex::diagnostic_printf("[ClassScanner] heap_bytes_read=%llu heap_bytes_skipped=%llu\n",
                    static_cast<unsigned long long>(read_bytes), static_cast<unsigned long long>(skipped_bytes));
        if (result.entry == 0) {
            return fail("no live UnityEntry with a valid Game reference",
                        skipped_bytes == 0 ? ERROR_NOT_FOUND : ERROR_PARTIAL_COPY);
        }

        scarlex::diagnostic_printf("[ClassScanner] UnityEntry=0x%llX UnityEntry+0x28 -> Game=0x%llX\n",
                    static_cast<unsigned long long>(result.entry), static_cast<unsigned long long>(result.game));
        SetLastError(ERROR_SUCCESS);
        return result;
    } catch (const std::bad_alloc&) {
        return fail("allocating scan buffer", ERROR_OUTOFMEMORY);
    }
}

