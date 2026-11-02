#pragma once

/**
 * @file audio_ext.hpp
 * @brief Audio file extension helpers.
 * @ingroup kanariv_media
 * @details Shared detection of supported audio file extensions for scanning
 * and path expansion.
 */

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace kanariv::media {

/**
 * @brief Checks if a path has a supported audio extension.
 * @ingroup kanariv_media
 * @param p Path to check.
 * @return true if extension is a supported audio extension
 * (decode + tag-write sets: .mp3/.flac/.ogg/.opus/.oga/.wav/.m4a/.mp4/.wma/
 * .asf/.aiff/.aif/.ape/.mpc/.wv, case-insensitive).
 */
inline bool hasAudioExt(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".mp3" || ext == ".flac" || ext == ".ogg" || ext == ".opus" || ext == ".oga" ||
           ext == ".wav" || ext == ".m4a" || ext == ".mp4" || ext == ".wma" || ext == ".asf" ||
           ext == ".aiff" || ext == ".aif" || ext == ".ape" || ext == ".mpc" || ext == ".wv";
}

} // namespace kanariv::media
