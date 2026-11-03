/**
 * @file format.cpp
 * @brief Display formatting shared by all frontends.
 * @ingroup kanariv_app
 * @details Moved verbatim out of the CLI shell (Phase 0).
 */

#include <app/format_internal.hpp>
#include <cmath>
#include <filesystem>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/record_json.hpp>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::app {

auto trackWho(const kanariv::ipc::Status& st) -> std::string {
    if (!st.artist.empty() && !st.title.empty()) {
        return st.artist + " - " + st.title;
    }
    if (!st.artist.empty()) {
        return st.artist;
    }
    if (!st.title.empty()) {
        return st.title;
    }
    if (!st.path.empty()) {
        std::string fn = std::filesystem::path(st.path).filename().generic_string();
        if (!fn.empty()) {
            return fn;
        }
    }
    return "unknown track";
}

auto addedLabel(const kanariv::types::Track& t) -> std::string {
    std::string fn = std::filesystem::path(t.path).filename().generic_string();
    std::string who;
    if (!t.artist.empty() && !t.title.empty()) {
        who = t.artist + " - " + t.title;
    } else {
        who = t.artist + t.title;
    }
    if (!who.empty() && !fn.empty()) {
        return who + ": " + fn;
    }
    if (!fn.empty()) {
        return fn;
    }
    if (!who.empty()) {
        return who;
    }
    return "track " + std::to_string(t.id);
}

void writePlaylistText(std::ostream& os, const std::vector<kanariv::types::Track>& tracks,
                       std::string_view format) {
    if (format == "m3u") {
        os << "#EXTM3U\n";
        for (const auto& t : tracks) {
            int dur = static_cast<int>(std::round(t.duration));
            std::string title = t.title.empty() ? t.path : t.title;
            std::string artist = t.artist.empty() ? "" : t.artist;
            os << "#EXTINF:" << dur;
            if (!artist.empty()) {
                os << "," << artist << " - " << title;
            } else {
                os << "," << title;
            }
            os << "\n";
            os << t.path << "\n";
        }
    } else if (format == "pls") {
        os << "[playlist]\n";
        int i = 1;
        for (const auto& t : tracks) {
            os << "File" << i << "=" << t.path << "\n";
            std::string title = t.title.empty() ? t.path : t.title;
            if (!t.artist.empty()) {
                title = t.artist + " - " + title;
            }
            os << "Title" << i << "=" << title << "\n";
            int dur = static_cast<int>(std::round(t.duration));
            os << "Length" << i << "=" << dur << "\n";
            ++i;
        }
        os << "NumberOfEntries=" << tracks.size() << "\n";
        os << "Version=2\n";
    }
}

void writePlaylistJson(std::ostream& os, const std::vector<kanariv::types::Track>& tracks) {
    kanariv::utils::Json j;
    j["format"] = "kanariv-playlist"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["version"] = 1; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks"] = kanariv::utils::Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& t : tracks) {
        j["tracks"].push_back(kanariv::utils::trackToJson(t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    os << j.dump(2) << "\n";
}

} // namespace kanariv::app
