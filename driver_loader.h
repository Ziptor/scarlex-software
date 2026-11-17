#pragma once

#include <string_view>
#include <windows.h>

enum class DriverLoadStage {
    none,
    open_device,
    wait_for_slot,
    resolve_directory,
    find_mapper,
    start_mapper,
    wait_for_mapper,
    read_mapper_exit_code,
    mapper_exit,
    wait_for_device,
};

struct DriverLoadError {
    DriverLoadStage stage = DriverLoadStage::none;
    DWORD win32_error = ERROR_SUCCESS;
    DWORD mapper_exit_code = 0;
};

class DriverLoader final {
public:
    // Paths use the active Win32 ANSI code page. "." uses the current directory.
    // The caller closes the returned device handle with CloseHandle.
    // Failure returns INVALID_HANDLE_VALUE and sets GetLastError().
    [[nodiscard]] static HANDLE open_or_load(
        std::string_view working_directory = ".",
        DriverLoadError* error = nullptr) noexcept;
};
