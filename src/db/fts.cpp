#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <db/fts.hpp>
#include <string>
#include <string_view>

namespace kanariv::db::internal {

// Strips FTS special characters from term into out. Doubled quotes are kept
// as escaped quotes ("" -> "").
void stripFtsSpecialChars(std::string_view term, std::string& out);

// Replaces FTS operator keywords (AND, OR, NOT, NEAR) with spaces in-place.
// Handles both " OP " and trailing/leading "OP"/" OP" forms.
void replaceFtsOperators(std::string& upper, std::string& cleaned);

// Collapses runs of spaces into a single space.
auto collapseWhitespace(const std::string& s) -> std::string;

// Trims leading and trailing spaces.
auto trimSpaces(const std::string& s) -> std::string;

auto escapeLike(std::string_view term) -> std::string {
    std::string result;
    result.reserve(term.size() + 4);
    for (char c : term) {
        if (c == '%' || c == '_' || c == '\\') {
            result.push_back('\\');
        }
        result.push_back(c);
    }
    return result;
}

auto sanitizeFtsTerm(std::string_view term) -> std::string {
    if (term.empty()) {
        return {};
    }

    std::string cleaned;
    cleaned.reserve(term.size());
    for (char c : term) {
        if (c == '\'' || c == '*' || c == ':' || c == '-' || c == '(' || c == ')' || c == '^' ||
            c == '~') {
            continue;
        }
        if (c == '"') {
            cleaned.push_back('"');
            cleaned.push_back('"');
        } else {
            cleaned.push_back(c);
        }
    }

    std::string upper = cleaned;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) -> int { return std::toupper(c); });

    replaceFtsOperators(upper, cleaned);
    cleaned = collapseWhitespace(cleaned);
    cleaned = trimSpaces(cleaned);

    if (cleaned.empty()) {
        return {};
    }

    return cleaned;
}

// Strips FTS special characters from term into out. Doubled quotes are kept
// as escaped quotes ("" -> "").
void stripFtsSpecialChars(std::string_view term, std::string& out) {
    for (char c : term) {
        if (c == '\'' || c == '*' || c == ':' || c == '-' || c == '(' || c == ')' || c == '^' ||
            c == '~') {
            continue;
        }
        if (c == '"') {
            out.push_back('"');
            out.push_back('"');
        } else {
            out.push_back(c);
        }
    }
}

// Replaces FTS operator keywords (AND, OR, NOT, NEAR) with spaces in-place.
// Handles both " OP " and trailing/leading "OP"/" OP" forms.
void replaceFtsOperators(std::string& upper, std::string& cleaned) {
    constexpr std::array<std::string_view, 4> opsWithSpaces = {
        {" AND ", " OR ", " NOT ", " NEAR "}};
    for (const auto& op : opsWithSpaces) {
        size_t pos = 0;
        while ((pos = upper.find(op, pos)) != std::string::npos) {
            upper.replace(pos, op.size(), std::string(op.size(), ' '));
            cleaned.replace(pos, op.size(), std::string(op.size(), ' '));
            pos += op.size();
        }
    }

    constexpr std::array<std::string_view, 4> opsStart = {{"AND ", "OR ", "NOT ", "NEAR "}};
    for (const auto& op : opsStart) {
        if (upper.rfind(op, 0) == 0) {
            upper.replace(0, op.size(), std::string(op.size(), ' '));
            cleaned.replace(0, op.size(), std::string(op.size(), ' '));
        }
    }

    constexpr std::array<std::string_view, 4> opsEnd = {{" AND", " OR", " NOT", " NEAR"}};
    for (const auto& op : opsEnd) {
        if (upper.size() >= op.size() &&
            upper.compare(upper.size() - op.size(), op.size(), op) == 0) {
            size_t pos = upper.size() - op.size();
            upper.replace(pos, op.size(), std::string(op.size(), ' '));
            cleaned.replace(pos, op.size(), std::string(op.size(), ' '));
        }
    }
}

// Collapses runs of spaces into a single space.
auto collapseWhitespace(const std::string& s) -> std::string {
    std::string result;
    result.reserve(s.size());
    bool lastWasSpace = false;
    for (char c : s) {
        if (c == ' ') {
            if (!lastWasSpace) {
                result.push_back(' ');
                lastWasSpace = true;
            }
        } else {
            result.push_back(c);
            lastWasSpace = false;
        }
    }
    return result;
}

// Trims leading and trailing spaces.
auto trimSpaces(const std::string& s) -> std::string {
    size_t start = 0;
    while (start < s.size() &&
           s[start] ==
               ' ') { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        start++;
    }
    size_t end = s.size();
    while (end > start &&
           s[end - 1] ==
               ' ') { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        end--;
    }
    if (start >= end) {
        return {};
    }
    return s.substr(start, end - start);
}

} // namespace kanariv::db::internal
