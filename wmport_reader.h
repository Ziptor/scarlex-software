#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <windows.h>

class WmPortReader final {
public:
    // Borrow a synchronous handle. Its owner closes it after the last read.
    explicit WmPortReader(HANDLE device) noexcept;

    // On failure the buffer may contain the prefix from completed chunks.
    // Read GetLastError() immediately after a false result.
    [[nodiscard]] bool read(std::uint32_t pid, std::uint64_t address,
                            void* buffer, std::size_t size) const noexcept;

    struct Counters {
        std::uint64_t read_calls;
        std::uint64_t ioctl_count;
    };
    [[nodiscard]] Counters counters() const noexcept;

    struct ReadRange {
        std::uint64_t address;
        void* buffer;
        std::size_t size;
        bool success{};
    };
    // Each range reports whether its entire payload was copied.
    void read_batch(std::uint32_t pid, std::span<ReadRange> ranges) const;

    // name is a UTF-8 base name, for example "UnityPlayer.dll".
    // Read GetLastError() immediately after an empty result.
    [[nodiscard]] std::optional<std::uint64_t> module_base(
        std::uint32_t pid, const std::string& name) const;
    [[nodiscard]] std::optional<std::size_t> module_size(
        std::uint32_t pid, const std::string& name) const;

private:
    HANDLE device_;
    mutable std::atomic<std::uint32_t> key_{};
    mutable std::atomic<std::uint64_t> read_calls_{};
    mutable std::atomic<std::uint64_t> ioctl_count_{};
    mutable std::atomic<bool> batch_unsupported_{};
};
