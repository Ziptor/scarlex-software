#pragma once

#include <cstdint>
#include <optional>

class WmPortReader;

class ClassScanner final {
public:
    using Address = std::uint64_t;

    struct Match {
        Address entry{};
        Address game{};
        Address entry_class{};
        Address class_table_global_rva{};
    };

    explicit ClassScanner(WmPortReader& reader, std::uint32_t pid) noexcept
        : reader_(reader), pid_(pid) {}

    // Call once after the game's UnityEntry has initialized Game.
    // On failure, GetLastError() describes the stage logged to stderr.
    [[nodiscard]] std::optional<Match> find_unity_entry() const;

private:
    WmPortReader& reader_;
    std::uint32_t pid_{};
};
