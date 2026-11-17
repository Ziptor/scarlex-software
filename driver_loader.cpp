#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "driver_loader.h"
#include "release_build.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

constexpr char kDevicePath[] = R"(\\.\WmPort)";
constexpr DWORD kTimeoutMs = 30'000;
constexpr DWORD kDevicePollMs = 50;

struct Result {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DriverLoadError error{};
};

class ProcessHandle {
public:
    ProcessHandle() = default;
    ~ProcessHandle() { reset(); }

    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    void reset(HANDLE value = nullptr) noexcept {
        if (value_) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

struct LoaderState {
    std::timed_mutex mutex;
    ProcessHandle mapper;
};

Result failure(DriverLoadStage stage, DWORD error, DWORD mapper_exit_code = 0) noexcept {
    return {INVALID_HANDLE_VALUE, {stage, error, mapper_exit_code}};
}

Result open_device() noexcept {
    const HANDLE device = CreateFileA(kDevicePath, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        return failure(DriverLoadStage::open_device, GetLastError());
    }
    return {device, {}};
}

bool device_missing(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND ||
           error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_DEV_NOT_EXIST;
}

DWORD remaining_ms(ULONGLONG deadline) noexcept {
    const ULONGLONG now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}

DWORD resolve_directory(std::string_view input, std::string& absolute) {
    if (input.empty() || input.find('\0') != std::string_view::npos) {
        return ERROR_INVALID_PARAMETER;
    }

    const std::string path(input);
    DWORD capacity = GetFullPathNameA(path.c_str(), 0, nullptr, nullptr);
    if (capacity == 0) {
        return GetLastError();
    }
    for (;;) {
        absolute.resize(capacity);
        const DWORD length = GetFullPathNameA(path.c_str(), capacity, absolute.data(), nullptr);
        if (length == 0) {
            return GetLastError();
        }
        if (length < capacity) {
            absolute.resize(length);
            return ERROR_SUCCESS;
        }
        capacity = length;
    }
}

DriverLoadError start_mapper(std::string_view working_directory, ProcessHandle& mapper) {
    std::string directory;
    if (const DWORD error = resolve_directory(working_directory, directory);
        error != ERROR_SUCCESS) {
        return {DriverLoadStage::resolve_directory, error};
    }

    std::string executable = directory;
    if (executable.back() != '\\' && executable.back() != '/') {
        executable.push_back('\\');
    }
    executable += "kd2.exe";

    const DWORD attributes = GetFileAttributesA(executable.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return {DriverLoadStage::find_mapper, GetLastError()};
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {DriverLoadStage::find_mapper, ERROR_DIRECTORY};
    }

    std::string command_line = '"' + executable + "\" --auto";
    STARTUPINFOA startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    constexpr DWORD flags = scarlex::build::diagnostics ? 0 : CREATE_NO_WINDOW;
    if constexpr (!scarlex::build::diagnostics) {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(executable.c_str(), command_line.data(), nullptr, nullptr,
                        FALSE, flags, nullptr, directory.c_str(), &startup, &process)) {
        return {DriverLoadStage::start_mapper, GetLastError()};
    }

    mapper.reset(process.hProcess);
    CloseHandle(process.hThread);
    return {};
}

Result wait_for_device(ULONGLONG deadline) noexcept {
    for (;;) {
        Result device = open_device();
        if (device.handle != INVALID_HANDLE_VALUE) {
            return device;
        }
        if (!device_missing(device.error.win32_error)) {
            return failure(DriverLoadStage::wait_for_device, device.error.win32_error);
        }
        const DWORD remaining = remaining_ms(deadline);
        if (remaining == 0) {
            return failure(DriverLoadStage::wait_for_device, ERROR_TIMEOUT);
        }
        Sleep(std::min(remaining, kDevicePollMs));
    }
}

Result load_impl(std::string_view working_directory) {
    Result device = open_device();
    if (device.handle != INVALID_HANDLE_VALUE || !device_missing(device.error.win32_error)) {
        return device;
    }

    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    static LoaderState state;
    std::unique_lock lock(state.mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(remaining_ms(deadline)))) {
        return failure(DriverLoadStage::wait_for_slot, ERROR_TIMEOUT);
    }

    // A preceding caller may have loaded the device while we waited.
    device = open_device();
    if (device.handle != INVALID_HANDLE_VALUE || !device_missing(device.error.win32_error)) {
        return device;
    }

    if (!state.mapper.get()) {
        if (remaining_ms(deadline) == 0) {
            return failure(DriverLoadStage::wait_for_slot, ERROR_TIMEOUT);
        }
        const DriverLoadError error = start_mapper(working_directory, state.mapper);
        if (error.win32_error != ERROR_SUCCESS) {
            return {INVALID_HANDLE_VALUE, error};
        }
    }

    const DWORD wait = WaitForSingleObject(state.mapper.get(), remaining_ms(deadline));
    if (wait != WAIT_OBJECT_0) {
        // Keep the handle so a retry can wait on the same mapper.
        return failure(DriverLoadStage::wait_for_mapper,
                       wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(state.mapper.get(), &exit_code)) {
        const DWORD error = GetLastError();
        state.mapper.reset();
        return failure(DriverLoadStage::read_mapper_exit_code, error);
    }
    state.mapper.reset();
    if (exit_code != 0) {
        return failure(DriverLoadStage::mapper_exit, ERROR_PROCESS_ABORTED, exit_code);
    }
    return wait_for_device(deadline);
}

}  // namespace

HANDLE DriverLoader::open_or_load(std::string_view working_directory,
                                  DriverLoadError* error) noexcept {
    Result result;
    try {
        result = load_impl(working_directory);
    } catch (const std::bad_alloc&) {
        result = failure(DriverLoadStage::resolve_directory, ERROR_NOT_ENOUGH_MEMORY);
    } catch (const std::length_error&) {
        result = failure(DriverLoadStage::resolve_directory, ERROR_FILENAME_EXCED_RANGE);
    } catch (const std::system_error&) {
        result = failure(DriverLoadStage::wait_for_slot, ERROR_GEN_FAILURE);
    }

    if (error) {
        *error = result.error;
    }
    SetLastError(result.error.win32_error);
    return result.handle;
}
