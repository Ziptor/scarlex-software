#include "runtime_log.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "wmport_reader.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <tlhelp32.h>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace {

constexpr DWORD kReadIoctl = 0x00292284;
constexpr DWORD kPebIoctl = 0x00292294;
constexpr DWORD kBatchReadIoctl = 0x0029229C;
constexpr std::size_t kReadLimit = 64 * 1024;
constexpr std::size_t kBatchLimit = 64;
constexpr std::size_t kModuleLimit = 4096;
constexpr int kSnapshotAttempts = 4;

struct Request {
    std::uint32_t key;
    std::uint32_t size;
    std::uint32_t pid;
    std::uint32_t reserved;
    std::uint64_t address;
};

static_assert(sizeof(Request) == 24);
static_assert(offsetof(Request, size) == 4);
static_assert(offsetof(Request, pid) == 8);
static_assert(offsetof(Request, reserved) == 12);
static_assert(offsetof(Request, address) == 16);

struct BatchEntry {
    std::uint32_t output_offset;
    std::uint32_t size;
    std::uint64_t address;
};

struct BatchRequest {
    std::uint32_t key;
    std::uint32_t count;
    std::uint32_t payload_size;
    std::uint32_t pid;
    std::array<BatchEntry, kBatchLimit> entries;
};

static_assert(sizeof(BatchEntry) == 0x10 && offsetof(BatchRequest, entries) == 0x10);
static_assert(sizeof(BatchRequest) == 0x410);

struct ListEntry64 {
    std::uint64_t flink;
    std::uint64_t blink;
};

struct Peb64 {
    std::byte prefix[0x18];
    std::uint64_t ldr;
};

struct Ldr64 {
    std::byte prefix[0x10];
    ListEntry64 load_order;
};

struct UnicodeString64 {
    std::uint16_t length;
    std::uint16_t maximum_length;
    std::uint32_t reserved;
    std::uint64_t buffer;
};

struct ModuleEntry64 {
    ListEntry64 load_order;
    std::byte reserved[0x20];
    std::uint64_t dll_base;
    std::uint64_t entry_point;
    std::uint32_t size_of_image;
    std::uint32_t padding;
    UnicodeString64 full_name;
    UnicodeString64 base_name;
};

static_assert(sizeof(wchar_t) == 2);
static_assert(offsetof(Peb64, ldr) == 0x18);
static_assert(offsetof(Ldr64, load_order) == 0x10);
static_assert(offsetof(ModuleEntry64, dll_base) == 0x30);
static_assert(offsetof(ModuleEntry64, size_of_image) == 0x40);
static_assert(offsetof(ModuleEntry64, base_name) == 0x58);
static_assert(sizeof(ModuleEntry64) == 0x68);

struct ModuleInfo {
    std::uint64_t base;
    std::size_t size;
};

class Snapshot {
public:
    explicit Snapshot(HANDLE handle) noexcept : handle_(handle) {}
    ~Snapshot() { CloseHandle(handle_); }
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

private:
    HANDLE handle_;
};

void log_error(const char* operation, std::uint32_t pid, DWORD error) noexcept {
    char line[160]{};
    std::snprintf(line, sizeof(line), "[WmPortReader] %s pid=%lu win32=%lu\n",
                  operation, static_cast<unsigned long>(pid), error);
    scarlex::diagnostic_debug(line);
}

bool valid_handle(HANDLE device) noexcept {
    return device != nullptr && device != INVALID_HANDLE_VALUE;
}

DWORD read_key(std::uint32_t& key) noexcept {
    DWORD value = 0;
    DWORD bytes = static_cast<DWORD>(sizeof(value));
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"SYSTEM\\WmPort", L"Key", RRF_RT_REG_DWORD,
        nullptr, &value, &bytes);
    if (status != ERROR_SUCCESS) {
        return static_cast<DWORD>(status);
    }
    if (bytes != sizeof(value) || value == 0) {
        return ERROR_INVALID_DATA;
    }
    key = static_cast<std::uint32_t>(value);
    return ERROR_SUCCESS;
}

DWORD exact_ioctl(HANDLE device, DWORD code, Request& request,
                  void* output, DWORD expected, std::size_t completed,
                  std::atomic<std::uint64_t>& ioctl_count) noexcept {
    DWORD returned = 0;
    ioctl_count.fetch_add(1, std::memory_order_relaxed);
    const BOOL ok = DeviceIoControl(device, code, &request, static_cast<DWORD>(sizeof(request)),
                                    output, expected, &returned, nullptr);
    if (ok && returned == expected) {
        return ERROR_SUCCESS;
    }
    DWORD error = ERROR_SUCCESS;
    const char* annotation = "";
    if (!ok) {
        error = GetLastError();
        if (code == kReadIoctl && error == ERROR_PARTIAL_COPY) {
            annotation = " (Win32 equivalent of STATUS_PARTIAL_COPY, 0x8000000D)";
        }
    } else {
        error = returned < expected ? ERROR_PARTIAL_COPY : ERROR_INVALID_DATA;
        annotation = " (unexpected output length)";
    }
    if (error == ERROR_SUCCESS) {
        error = ERROR_GEN_FAILURE;
    }
    char line[320]{};
    std::snprintf(
        line, sizeof(line),
        "[WmPortReader] IOCTL=0x%08lX pid=%lu address=0x%016llX "
        "win32=%lu returned=%lu expected=%lu completed=%zu%s\n",
        code, static_cast<unsigned long>(request.pid),
        static_cast<unsigned long long>(request.address),
        error, returned, expected, completed, annotation);
    scarlex::diagnostic_debug(line);
    return error;
}

DWORD read_chunks(HANDLE device, std::uint32_t key, std::uint32_t pid,
                  std::uint64_t address, void* buffer, std::size_t size,
                  std::atomic<std::uint64_t>& ioctl_count) noexcept {
    auto* destination = static_cast<std::byte*>(buffer);
    std::size_t completed = 0;
    while (completed < size) {
        const auto chunk = static_cast<DWORD>(std::min(size - completed, kReadLimit));
        Request request{key, chunk, pid, 0, address + completed};
        const DWORD error = exact_ioctl(device, kReadIoctl, request, destination + completed,
                                        chunk, completed, ioctl_count);
        if (error != ERROR_SUCCESS) {
            return error;
        }
        completed += chunk;
    }
    return ERROR_SUCCESS;
}

bool user_range(const SYSTEM_INFO& system, std::uint64_t address, std::size_t size) noexcept {
    const auto first = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto last = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    return size != 0 && address >= first && address <= last && size - 1 <= last - address;
}

bool same_module_name(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::optional<ModuleInfo> find_in_peb(HANDLE device, std::uint32_t pid,
                                      std::wstring_view wanted, DWORD& error,
                                      std::atomic<std::uint64_t>& ioctl_count) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    if (sizeof(void*) != 8 || system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        error = ERROR_NOT_SUPPORTED;
        return std::nullopt;
    }

    std::uint32_t key = 0;
    error = read_key(key);
    if (error != ERROR_SUCCESS) {
        return std::nullopt;
    }
    Request peb_request{key, 0, pid, 0, 0};
    std::uint64_t peb_address = 0;
    error = exact_ioctl(device, kPebIoctl, peb_request, &peb_address,
                        static_cast<DWORD>(sizeof(peb_address)), 0, ioctl_count);
    if (error != ERROR_SUCCESS) {
        return std::nullopt;
    }

    const auto read_remote = [&](std::uint64_t address, void* output, std::size_t size) {
        error = user_range(system, address, size)
            ? read_chunks(device, key, pid, address, output, size, ioctl_count)
            : ERROR_INVALID_ADDRESS;
        return error == ERROR_SUCCESS;
    };

    Peb64 peb{};
    Ldr64 ldr{};
    if (!read_remote(peb_address, &peb, sizeof(peb)) ||
        !read_remote(peb.ldr, &ldr, sizeof(ldr))) {
        return std::nullopt;
    }
    const std::uint64_t head = peb.ldr + offsetof(Ldr64, load_order);
    std::uint64_t node = ldr.load_order.flink;
    std::uint64_t previous = head;
    std::size_t visited = 0;
    std::wstring name;
    while (node != head && visited < kModuleLimit) {
        ModuleEntry64 entry{};
        if ((node & 7) != 0) {
            error = ERROR_INVALID_DATA;
            return std::nullopt;
        }
        if (!read_remote(node, &entry, sizeof(entry))) {
            return std::nullopt;
        }
        if (entry.load_order.blink != previous || entry.load_order.flink == node) {
            error = ERROR_RETRY;
            return std::nullopt;
        }

        const auto& remote_name = entry.base_name;
        if (remote_name.length != 0) {
            if (remote_name.length % sizeof(wchar_t) != 0 ||
                remote_name.length > remote_name.maximum_length) {
                error = ERROR_INVALID_DATA;
                return std::nullopt;
            }
            if (remote_name.length / sizeof(wchar_t) == wanted.size()) {
                name.resize(remote_name.length / sizeof(wchar_t));
                if (!read_remote(remote_name.buffer, name.data(), remote_name.length)) {
                    return std::nullopt;
                }
                if (same_module_name(name, wanted)) {
                    if (!user_range(system, entry.dll_base, entry.size_of_image)) {
                        error = ERROR_INVALID_DATA;
                        return std::nullopt;
                    }
                    return ModuleInfo{entry.dll_base, entry.size_of_image};
                }
            }
        }
        previous = node;
        node = entry.load_order.flink;
        ++visited;
    }
    error = node == head && previous == ldr.load_order.blink
        ? ERROR_MOD_NOT_FOUND : ERROR_RETRY;
    return std::nullopt;
}

std::optional<ModuleInfo> find_with_toolhelp(std::uint32_t pid,
                                            std::wstring_view wanted, DWORD& error) noexcept {
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
        handle = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }
        error = GetLastError();
        if (error != ERROR_BAD_LENGTH) {
            return std::nullopt;
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const Snapshot snapshot(handle);
    MODULEENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    BOOL more = Module32FirstW(handle, &entry);
    while (more) {
        if (same_module_name(entry.szModule, wanted)) {
            error = ERROR_SUCCESS;
            return ModuleInfo{reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize};
        }
        more = Module32NextW(handle, &entry);
    }
    error = GetLastError();
    if (error == ERROR_NO_MORE_FILES) {
        error = ERROR_MOD_NOT_FOUND;
    }
    return std::nullopt;
}

std::optional<ModuleInfo> find_module(HANDLE device, std::uint32_t pid,
                                      const std::string& name, DWORD& error,
                                      std::atomic<std::uint64_t>& ioctl_count) {
    if (pid == 0 || name.empty() || name.find('\0') != std::string::npos ||
        name.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        error = ERROR_INVALID_PARAMETER;
        log_error("module name or PID", pid, error);
        return std::nullopt;
    }
    try {
        const int length = static_cast<int>(name.size());
        const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                   name.data(), length, nullptr, 0);
        if (characters == 0) {
            error = GetLastError();
            log_error("UTF-8 module name", pid, error);
            return std::nullopt;
        }
        std::wstring wanted(static_cast<std::size_t>(characters), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), length,
                                wanted.data(), characters) == 0) {
            error = GetLastError();
            log_error("UTF-8 module name", pid, error);
            return std::nullopt;
        }

        DWORD peb_error = ERROR_INVALID_HANDLE;
        if (valid_handle(device)) {
            if (auto module = find_in_peb(device, pid, wanted, peb_error, ioctl_count)) {
                error = ERROR_SUCCESS;
                return module;
            }
        }
        DWORD snapshot_error = ERROR_SUCCESS;
        if (auto module = find_with_toolhelp(pid, wanted, snapshot_error)) {
            error = ERROR_SUCCESS;
            return module;
        }
        log_error("PEB module lookup", pid, peb_error);
        log_error("Toolhelp module lookup", pid, snapshot_error);
        error = snapshot_error;
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        log_error("module lookup allocation", pid, error);
        return std::nullopt;
    }
}

}  // namespace

WmPortReader::WmPortReader(HANDLE device) noexcept : device_(device) {}

WmPortReader::Counters WmPortReader::counters() const noexcept {
    return {read_calls_.load(std::memory_order_relaxed), ioctl_count_.load(std::memory_order_relaxed)};
}

void WmPortReader::read_batch(std::uint32_t pid, std::span<ReadRange> ranges) const {
    for (auto& range : ranges) {
        range.success = false;
    }
    const auto single_reads = [&](std::span<ReadRange> subset) {
        for (auto& range : subset) {
            range.success = read(pid, range.address, range.buffer, range.size);
        }
    };
    auto key = key_.load(std::memory_order_relaxed);
    if (ranges.empty() || !valid_handle(device_) || pid == 0 ||
        batch_unsupported_.load(std::memory_order_relaxed) ||
        (key == 0 && read_key(key) != ERROR_SUCCESS)) {
        single_reads(ranges);
        return;
    }
    key_.store(key, std::memory_order_relaxed);
    std::vector<std::byte> reply;
    while (!ranges.empty()) {
        BatchRequest request{key, 0, 0, pid, {}};
        while (request.count < ranges.size() && request.count < request.entries.size()) {
            const auto& range = ranges[request.count];
            const auto used = request.payload_size + request.count;
            if (used >= kReadLimit) {
                break;
            }
            const auto limit = kReadLimit - used - 1;
            if (!range.buffer || range.address == 0 || range.size == 0 || range.size > limit ||
                range.size - 1 > (std::numeric_limits<std::uint64_t>::max)() - range.address) {
                break;
            }
            const auto size = static_cast<std::uint32_t>(range.size);
            request.entries[request.count++] = {request.payload_size, size, range.address};
            request.payload_size += size;
        }
        if (request.count == 0) {
            single_reads(ranges.first(1));
            ranges = ranges.subspan(1);
            continue;
        }

        // The dispatcher requires output capacity >= 2 * payload_size. It returns only
        // payload_size bytes of data followed by one success byte per range.
        const DWORD expected = request.payload_size + request.count;
        reply.resize((std::max<std::size_t>)(expected, 2 * request.payload_size));
        DWORD returned{};
        ioctl_count_.fetch_add(1, std::memory_order_relaxed);
        const BOOL ok = DeviceIoControl(device_, kBatchReadIoctl, &request, static_cast<DWORD>(sizeof(request)),
                                        reply.data(), static_cast<DWORD>(reply.size()), &returned, nullptr);
        if (!ok || returned != expected) {
            const DWORD error = ok ? ERROR_INVALID_DATA : GetLastError();
            if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
                error == ERROR_INVALID_PARAMETER) {
                batch_unsupported_.store(true, std::memory_order_relaxed);
            }
            single_reads(ranges);
            return;
        }
        for (std::size_t i = 0; i < request.count; ++i) {
            if (reply[request.payload_size + i] == std::byte{1}) {
                const auto& entry = request.entries[i];
                std::memcpy(ranges[i].buffer, reply.data() + entry.output_offset, entry.size);
                ranges[i].success = true;
            }
        }
        ranges = ranges.subspan(request.count);
    }
}

bool WmPortReader::read(std::uint32_t pid, std::uint64_t address,
                       void* buffer, std::size_t size) const noexcept {
    read_calls_.fetch_add(1, std::memory_order_relaxed);
    if (size == 0) {
        SetLastError(ERROR_SUCCESS);
        return true;
    }
    if (!valid_handle(device_)) {
        log_error("read handle", pid, ERROR_INVALID_HANDLE);
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    if (pid == 0 || !buffer || address == 0 ||
        size - 1 > (std::numeric_limits<std::uint64_t>::max)() - address) {
        log_error("read range", pid, ERROR_INVALID_PARAMETER);
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    auto key = key_.load(std::memory_order_relaxed);
    if (key == 0) {
        const DWORD key_error = read_key(key);
        if (key_error != ERROR_SUCCESS) {
            log_error("read HKLM\\SYSTEM\\WmPort\\Key", pid, key_error);
            SetLastError(key_error);
            return false;
        }
        key_.store(key, std::memory_order_relaxed);
    }
    DWORD error = read_chunks(device_, key, pid, address, buffer, size, ioctl_count_);
    if (error == ERROR_ACCESS_DENIED) {
        std::uint32_t refreshed{};
        if (read_key(refreshed) == ERROR_SUCCESS && refreshed != key) {
            key_.store(refreshed, std::memory_order_relaxed);
            error = read_chunks(device_, refreshed, pid, address, buffer, size, ioctl_count_);
        }
    }
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

std::optional<std::uint64_t> WmPortReader::module_base(
    std::uint32_t pid, const std::string& name) const {
    DWORD error = ERROR_SUCCESS;
    const auto module = find_module(device_, pid, name, error, ioctl_count_);
    SetLastError(error);
    return module ? std::optional<std::uint64_t>{module->base} : std::nullopt;
}

std::optional<std::size_t> WmPortReader::module_size(
    std::uint32_t pid, const std::string& name) const {
    DWORD error = ERROR_SUCCESS;
    const auto module = find_module(device_, pid, name, error, ioctl_count_);
    SetLastError(error);
    return module ? std::optional<std::size_t>{module->size} : std::nullopt;
}

