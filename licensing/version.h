#pragma once

#include <array>
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace scarlex {

inline std::array<unsigned, 3> parse_version(std::string_view text) {
    std::array<unsigned, 3> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (part.empty() || (part.size() > 1 && part.front() == '0') ||
            (i < 2 ? dot == text.npos : dot != text.npos)) throw std::runtime_error("Invalid version");
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), parts[i]);
        if (error != std::errc{} || end != part.data() + part.size() || parts[i] > 65535)
            throw std::runtime_error("Invalid version");
        if (dot != text.npos) text.remove_prefix(dot + 1);
    }
    return parts;
}

inline int compare_versions(std::string_view left, std::string_view right) {
    const auto a = parse_version(left), b = parse_version(right);
    return a < b ? -1 : a > b ? 1 : 0;
}

} // namespace scarlex
