#include <kanariv/db/smart.hpp>
#include <kanariv/utils/error.hpp>
#include <charconv>
#include <cctype>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::db {

namespace {

// Trims whitespace from both ends of a view.
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

// Splits on commas, trimming each token; empty tokens are skipped.
std::vector<std::string_view> splitPredicates(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto comma = s.find(',', start);
        if (comma == std::string_view::npos)
            comma = s.size();
        auto tok = trim(s.substr(start, comma - start));
        if (!tok.empty())
            out.push_back(tok);
        if (comma == s.size())
            break;
        start = comma + 1;
    }
    return out;
}

// Parses a whole integer; requires the entire string to be consumed.
bool parseInt(std::string_view s, int64_t& out) {
    s = trim(s);
    if (s.empty())
        return false;
    int64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        return false;
    out = v;
    return true;
}

// Parses a floating-point value; requires the entire string to be consumed.
bool parseDouble(std::string_view s, double& out) {
    s = trim(s);
    if (s.empty())
        return false;
    try {
        std::size_t pos = 0;
        double v = std::stod(std::string(s), &pos);
        if (pos != s.size())
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// Reads the leading operator from s and advances past it. Returns the op
// string ("=", ">=", "<=", ">", "<") or nullopt when no operator is present.
std::optional<std::string_view> readOp(std::string_view& s) {
    if (s.size() >= 2 && s[0] == '>' && s[1] == '=') {
        s.remove_prefix(2);
        return ">=";
    }
    if (s.size() >= 2 && s[0] == '<' && s[1] == '=') {
        s.remove_prefix(2);
        return "<=";
    }
    if (s.size() >= 1 && s[0] == '=') {
        s.remove_prefix(1);
        return "=";
    }
    if (s.size() >= 1 && s[0] == '>') {
        s.remove_prefix(1);
        return ">";
    }
    if (s.size() >= 1 && s[0] == '<') {
        s.remove_prefix(1);
        return "<";
    }
    return std::nullopt;
}

} // namespace

std::expected<kanariv::types::TrackQuery, kanariv::utils::Error> parseSmartQuery(std::string_view q) {
    kanariv::types::TrackQuery tq{};
    q = trim(q);
    if (q.empty())
        return tq;

    for (auto pred : splitPredicates(q)) {
        // Locate the first operator character to split field from value.
        std::size_t opPos = pred.size();
        for (std::size_t i = 0; i < pred.size(); ++i) {
            char c = pred[i];
            if (c == '=' || c == '>' || c == '<') {
                opPos = i;
                break;
            }
        }
        if (opPos == 0 || opPos == pred.size())
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::InvalidArg,
                "smart query: malformed predicate '" + std::string(pred) + "'")};

        std::string field = std::string(trim(pred.substr(0, opPos)));
        std::string_view rest = pred.substr(opPos);
        auto op = readOp(rest);
        if (!op.has_value())
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::InvalidArg,
                "smart query: missing operator in '" + std::string(pred) + "'")};
        std::string value = std::string(trim(rest));

        if (field == "rating") {
            if (*op != "=")
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg,
                    "smart query: rating supports only '='")};
            int64_t v;
            if (!parseInt(value, v) || v < 0 || v > 5)
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg,
                    "smart query: rating must be an integer 0..5")};
            tq.rating = static_cast<int32_t>(v);
        } else if (field == "year") {
            int64_t v;
            if (!parseInt(value, v))
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg,
                    "smart query: invalid year value '" + value + "'")};
            if (*op == "=")
                tq.year = static_cast<int32_t>(v);
            else if (*op == ">=")
                tq.year_min = static_cast<int32_t>(v);
            else if (*op == "<=")
                tq.year_max = static_cast<int32_t>(v);
            else if (*op == ">")
                tq.year_min = static_cast<int32_t>(v + 1);
            else if (*op == "<")
                tq.year_max = static_cast<int32_t>(v - 1);
        } else if (field == "play_count") {
            int64_t v;
            if (!parseInt(value, v) || v < 0)
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg,
                    "smart query: invalid play_count value '" + value + "'")};
            if (*op == "=")
                tq.play_count_min = v;
            else if (*op == ">=")
                tq.play_count_min = v;
            else if (*op == "<=")
                tq.play_count_max = v;
            else if (*op == ">")
                tq.play_count_min = v + 1;
            else if (*op == "<")
                tq.play_count_max = v - 1;
        } else if (field == "duration") {
            double v;
            if (!parseDouble(value, v) || v < 0)
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg,
                    "smart query: invalid duration value '" + value + "'")};
            if (*op == "=") {
                tq.duration_min = v;
                tq.duration_max = v;
            } else if (*op == ">=")
                tq.duration_min = v;
            else if (*op == "<=")
                tq.duration_max = v;
            else if (*op == ">")
                tq.duration_min = v + 1.0;
            else if (*op == "<")
                tq.duration_max = v - 1.0;
        } else if (field == "artist" || field == "album" || field == "genre") {
            if (*op != "=")
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg,
                    "smart query: " + field + " supports only '='")};
            if (field == "artist")
                tq.artist = value;
            else if (field == "album")
                tq.album = value;
            else
                tq.genre = value;
        } else if (field == "search") {
            if (*op != "=")
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg,
                    "smart query: search supports only '='")};
            tq.search = value;
        } else {
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::InvalidArg,
                "smart query: unknown field '" + field + "'")};
        }
    }

    return tq;
}

} // namespace kanariv::db
