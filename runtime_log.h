#pragma once
#include "release_build.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <iostream>
#include <streambuf>

namespace scarlex {
namespace detail {
class DiscardBuffer final : public std::streambuf {
    int_type overflow(int_type value) override { return traits_type::not_eof(value); }
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
};
inline std::ostream& discard_stream() {
    thread_local DiscardBuffer buffer;
    thread_local std::ostream stream(&buffer);
    return stream;
}
}
inline std::ostream& diagnostic_out() {
    if constexpr (build::diagnostics) return std::cout;
    return detail::discard_stream();
}
inline std::ostream& diagnostic_err() {
    if constexpr (build::diagnostics) return std::cerr;
    return detail::discard_stream();
}
template<class... Args> int diagnostic_printf(const char* format, Args... args) {
    if constexpr (build::diagnostics) return std::printf(format, args...);
    return 0;
}
template<class... Args> int diagnostic_fprintf(std::FILE* file, const char* format, Args... args) {
    if constexpr (build::diagnostics) return std::fprintf(file, format, args...);
    return 0;
}
inline void diagnostic_debug(const char* value) {
    if constexpr (build::diagnostics) OutputDebugStringA(value);
}
inline void diagnostic_debug(const wchar_t* value) {
    if constexpr (build::diagnostics) OutputDebugStringW(value);
}
}
