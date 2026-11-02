#pragma once

/**
 * @file print.hpp
 * @brief Portable `print`/`println` facade over `<format>`.
 * @ingroup kanariv_utils
 * @details Routes through `std::format` + classic insertion on every
 * toolchain. MinGW GCC 16.x is included: its `<print>` stream/`FILE*`
 * overloads lower to missing Windows-console helpers, so the format-based
 * path is used universally for byte-identical output. Always call
 * `kanariv::print`/`kanariv::println`, never `std::` directly.
 */

#include <cstdio>
#include <format>
#include <iostream>
#include <ostream>
#include <string>
#include <utility>

namespace kanariv {

/**
 * @brief Formats arguments and writes them to a C++ stream.
 * @param os Destination stream.
 * @param fmt Compile-time checked format string.
 * @param args Values consumed by the format string.
 */
template <typename... A>
void print(std::ostream& os, std::format_string<A...> fmt, A&&... args) {
    os << std::format(fmt, std::forward<A>(args)...);
}

/**
 * @brief Formats arguments, writes them to a C++ stream, and appends a newline.
 * @param os Destination stream.
 * @param fmt Compile-time checked format string.
 * @param args Values consumed by the format string.
 */
template <typename... A>
void println(std::ostream& os, std::format_string<A...> fmt, A&&... args) {
    os << std::format(fmt, std::forward<A>(args)...) << '\n';
}

/**
 * @brief Formats arguments and writes them to a C stream.
 * @param f Destination `FILE*`; it must be non-null and writable.
 * @param fmt Compile-time checked format string.
 * @param args Values consumed by the format string.
 */
template <typename... A>
void print(std::FILE* f, std::format_string<A...> fmt, A&&... args) {
    std::fputs(std::format(fmt, std::forward<A>(args)...).c_str(), f);
}

/**
 * @brief Formats arguments, writes them to a C stream, and appends a newline.
 * @param f Destination `FILE*`; it must be non-null and writable.
 * @param fmt Compile-time checked format string.
 * @param args Values consumed by the format string.
 */
template <typename... A>
void println(std::FILE* f, std::format_string<A...> fmt, A&&... args) {
    std::string s = std::format(fmt, std::forward<A>(args)...);
    s.push_back('\n');
    std::fputs(s.c_str(), f);
}

/**
 * @brief Formats arguments and writes them to `stdout`.
 * @param fmt Compile-time checked format string.
 * @param args Values consumed by the format string.
 */
template <typename... A>
void print(std::format_string<A...> fmt, A&&... args) {
    std::cout << std::format(fmt, std::forward<A>(args)...);
}

/**
 * @brief Formats arguments, writes them to `stdout`, and appends a newline.
 * @param fmt Compile-time checked format string.
 * @param args Values consumed by the format string.
 */
template <typename... A>
void println(std::format_string<A...> fmt, A&&... args) {
    std::cout << std::format(fmt, std::forward<A>(args)...) << '\n';
}

} // namespace kanariv
