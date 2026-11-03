/**
 * @file paths.cpp
 * @brief File/glob/path utilities shared by all frontends.
 * @ingroup kanariv_app
 * @details Moved verbatim out of the CLI shell (Phase 0).
 */

#include <algorithm>
#include <app/paths_internal.hpp>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <kanariv/app/paths.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace kanariv::app {

namespace {

// Case-insensitive wildcard match (* and ? only) for queue-add globs.
auto wildcardMatch(std::string_view pat, std::string_view name) -> bool {
    std::size_t px = 0, nx = 0, star = std::string_view::npos, ss = 0;
    auto lower = [](char c) -> char {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    while (nx < name.size()) {
        if (px < pat.size() && (pat[px] == '?' || lower(pat[px]) == lower(name[nx]))) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            ++px;
            ++nx;
        } else if (px < pat.size() && pat[px] == '*') { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            star = px++;
            ss = nx;
        } else if (star != std::string_view::npos) {
            px = star + 1;
            nx = ++ss;
        } else {
            return false;
        }
    }
    while (px < pat.size() && pat[px] == '*') { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ++px;
    }
    return px == pat.size();
}

} // namespace

auto hasGlobChars(std::string_view s) -> bool {
    return s.find_first_of("*?") != std::string_view::npos;
}

void expandAddToken(const std::string& token, bool recursive, std::vector<std::string>& files,
                    std::vector<std::string>& unmatched) {
    std::error_code ec;
    if (hasGlobChars(token)) {
        std::filesystem::path p(token);
        std::filesystem::path dir = p.parent_path();
        if (dir.empty()) {
            dir = ".";
        }
        std::string pat = p.filename().generic_string();
        std::vector<std::string> hits;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             it != std::filesystem::directory_iterator(); ++it) {
            if (ec) {
                break;
            }
            std::error_code e2;
            if (!it->is_regular_file(e2) || e2) {
                continue;
            }
            std::string fn = it->path().filename().generic_string();
            if (wildcardMatch(pat, fn) && kanariv::media::hasAudioExt(it->path())) {
                auto abs = std::filesystem::absolute(it->path(), e2);
                hits.push_back(e2 ? it->path().generic_string() : abs.generic_string());
            }
        }
        if (hits.empty()) {
            unmatched.push_back(token);
            return;
        }
        std::sort(hits.begin(), hits.end());
        files.insert(files.end(), hits.begin(), hits.end());
        return;
    }
    std::filesystem::path p(token);
    if (std::filesystem::is_directory(p, ec) && !ec) {
        std::vector<std::string> hits;
        if (recursive) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     p, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); ++it) {
                if (ec) {
                    break;
                }
                std::error_code e2;
                if (it->is_regular_file(e2) && !e2 && kanariv::media::hasAudioExt(it->path())) {
                    auto abs = std::filesystem::absolute(it->path(), e2);
                    hits.push_back(e2 ? it->path().generic_string() : abs.generic_string());
                }
            }
        } else {
            for (auto it = std::filesystem::directory_iterator(p, ec);
                 it != std::filesystem::directory_iterator(); ++it) {
                if (ec) {
                    break;
                }
                std::error_code e2;
                if (it->is_regular_file(e2) && !e2 && kanariv::media::hasAudioExt(it->path())) {
                    auto abs = std::filesystem::absolute(it->path(), e2);
                    hits.push_back(e2 ? it->path().generic_string() : abs.generic_string());
                }
            }
        }
        if (hits.empty()) {
            unmatched.push_back(token);
            return;
        }
        std::sort(hits.begin(), hits.end());
        files.insert(files.end(), hits.begin(), hits.end());
        return;
    }
    if (std::filesystem::is_regular_file(p, ec) && !ec) {
        auto abs = std::filesystem::absolute(p, ec);
        files.push_back(ec ? p.generic_string() : abs.generic_string());
        return;
    }
    unmatched.push_back(token);
}

auto pathKey(const std::string& p) -> std::string {
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    std::string s = ec ? p : abs.lexically_normal().generic_string();
#ifdef _WIN32
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return s;
}

auto isNumeric(const std::string& s) -> bool {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace kanariv::app
