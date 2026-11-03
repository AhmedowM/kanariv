#pragma once
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <kanariv/utils/error.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace kanariv::app::cli::parse {

// Use std::string for error message to keep header standalone (no kanariv::utils dependency).
// App module wraps errors into kanariv::utils::Error.

// Parse a whole non-empty token as double (nullopt = invalid). Uses strtod
// instead of the floating-point std::from_chars overloads: Apple libc++
// gates those on macOS 26+ (deployment target), while the integer overloads
// and strtod work everywhere. Mirrors from_chars strictness, which strtod
// alone does not: no leading whitespace (strtod skips it), no hex floats
// (strtod accepts "0x1p3", from_chars general format does not), full
// consumption required. Overflow surfaces as inf and is rejected by the
// callers' isfinite checks, same as from_chars' out-of-range path.
// Locale note: strtod honors LC_NUMERIC while from_chars always takes
// '.', but the process never leaves the default "C" locale, so '.' parses
// identically in practice.
inline std::optional<double> parseDoubleToken(std::string_view token) {
    if (token.empty() || token.front() == ' ' || token.front() == '\t' || token.front() == '\r' ||
        token.front() == '\n')
        return std::nullopt;
    size_t digits = (token.front() == '+' || token.front() == '-') ? 1 : 0;
    if (token.size() > digits + 2 && token[digits] == '0' &&
        (token[digits + 1] == 'x' || token[digits + 1] == 'X'))
        return std::nullopt;
    std::string s(token);
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(begin, &end);
    if (end != begin + s.size())
        return std::nullopt;
    return v;
}

inline std::expected<double, std::string> parseTime(std::string_view s) {
    // trim whitespace
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    if (s.empty())
        return std::unexpected<std::string>{"empty time"};

    // Check for ':' presence -> mm:ss or hh:mm:ss
    size_t colon = s.find(':');
    if (colon != std::string_view::npos) {
        // split by ':'
        double total = 0.0;
        std::string_view remaining = s;
        // Count colons
        size_t count = 0;
        for (char c : s)
            if (c == ':')
                ++count;
        // parse each part using from_chars
        double parts[3] = {0, 0, 0};
        int idx = 0;
        while (!remaining.empty() && idx < 3) {
            size_t nxt = remaining.find(':');
            std::string_view token =
                (nxt == std::string_view::npos) ? remaining : remaining.substr(0, nxt);
            if (token.empty())
                return std::unexpected<std::string>{"empty component"};
            // allow fractional for last component only
            double val = 0;
            if (idx == 2 || (count == 1 && idx == 1)) {
                // last component may be double
                auto parsed = parseDoubleToken(token);
                if (!parsed)
                    return std::unexpected<std::string>{"invalid time component"};
                val = *parsed;
            } else {
                int iv = 0;
                auto res = std::from_chars(token.data(), token.data() + token.size(), iv);
                if (res.ec != std::errc{} || res.ptr != token.data() + token.size()) {
                    return std::unexpected<std::string>{"invalid time component"};
                }
                if (iv < 0)
                    return std::unexpected<std::string>{"negative component"};
                val = static_cast<double>(iv);
            }
            parts[idx++] = val;
            if (nxt == std::string_view::npos)
                break;
            remaining.remove_prefix(nxt + 1);
        }
        if (count == 1) {
            // mm:ss: minutes unbounded (long mixes exceed 59), seconds in
            // range -- overflow is almost always a typo, fail loudly.
            double mm = parts[0];
            double ss = parts[1];
            if (ss < 0 || ss >= 60)
                return std::unexpected<std::string>{"seconds out of range (ss < 60)"};
            total = mm * 60.0 + ss;
        } else if (count == 2) {
            // hh:mm:ss: same rule per component, hours unbounded.
            double hh = parts[0];
            double mm = parts[1];
            double ss = parts[2];
            if (mm < 0 || mm >= 60)
                return std::unexpected<std::string>{"minutes out of range (mm < 60)"};
            if (ss < 0 || ss >= 60)
                return std::unexpected<std::string>{"seconds out of range (ss < 60)"};
            total = hh * 3600.0 + mm * 60.0 + ss;
        } else {
            return std::unexpected<std::string>{"too many colons"};
        }
        if (!std::isfinite(total) || total < 0)
            return std::unexpected<std::string>{"invalid time"};
        return total;
    } else {
        auto parsed = parseDoubleToken(s);
        if (!parsed)
            return std::unexpected<std::string>{"invalid time"};
        double v = *parsed;
        if (!std::isfinite(v) || v < 0)
            return std::unexpected<std::string>{"invalid time"};
        return v;
    }
}

inline std::expected<double, std::string> parseSeek(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    if (s.empty())
        return std::unexpected<std::string>{"empty seek"};
    bool relative = false;
    bool negative = false;
    std::string_view core = s;
    if (core.front() == '+' || core.front() == '-') {
        relative = true;
        negative = (core.front() == '-');
        core.remove_prefix(1);
        if (core.empty())
            return std::unexpected<std::string>{"missing seek after sign"};
    }
    auto t = parseTime(core);
    if (!t)
        return t;
    double v = *t;
    if (relative) {
        if (negative)
            v = -v;
        // caller will add currentPos; return delta
        return v;
    }
    return v;
}

// Volume parsed result matches Command VolumeSet fields but header standalone
struct ParsedVolume {
    std::optional<float> level{};
    std::optional<bool> mute{};
    std::optional<int> deltaPct{};
};

inline std::expected<ParsedVolume, std::string> parseVolume(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    if (s.empty()) {
        // no arg -> show, return empty
        return ParsedVolume{};
    }
    if (s == "mute")
        return ParsedVolume{std::nullopt, true, std::nullopt};
    if (s == "unmute")
        return ParsedVolume{std::nullopt, false, std::nullopt};
    if (s.front() == '+' || s.front() == '-') {
        bool neg = s.front() == '-';
        std::string_view num = s.substr(1);
        if (num.empty())
            return std::unexpected<std::string>{"invalid delta"};
        int iv = 0;
        auto res = std::from_chars(num.data(), num.data() + num.size(), iv);
        if (res.ec != std::errc{} || res.ptr != num.data() + num.size()) {
            return std::unexpected<std::string>{"invalid delta"};
        }
        if (neg)
            iv = -iv;
        return ParsedVolume{std::nullopt, std::nullopt, iv};
    }
    // absolute 0-100
    {
        int iv = 0;
        auto res = std::from_chars(s.data(), s.data() + s.size(), iv);
        if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
            return std::unexpected<std::string>{"invalid volume"};
        }
        if (iv < 0 || iv > 100)
            return std::unexpected<std::string>{"out of range"};
        return ParsedVolume{static_cast<float>(iv), std::nullopt, std::nullopt};
    }
}

inline std::chrono::duration<double> durationFromSeconds(double s) {
    return std::chrono::duration<double>{s};
}

} // namespace kanariv::app::cli::parse
