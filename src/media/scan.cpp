#include <blake3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/generator.hpp>
#include <media/fingerprint.hpp>
#include <system_error>
#include <utility>

namespace kanariv::media {

constexpr std::size_t scanBufferSize = 8192;

auto scanTracks(const std::filesystem::path& root, ScanMode mode)
    -> kanariv::utils::Generator<kanariv::types::Track> {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        co_return;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec) && kanariv::media::hasAudioExt(it->path())) {
            kanariv::types::Track t;
            t.path = it->path().generic_string();
            std::error_code e2;
            auto sz = it->file_size(e2);
            if (!e2) {
                t.size = static_cast<int64_t>(sz);
            }
            auto ftime = it->last_write_time(e2);
            if (!e2) {
                t.mtime = static_cast<int64_t>(ftime.time_since_epoch().count());
            }
            if (mode == ScanMode::Sampled) {
                auto fp = internal::computeFingerprint(it->path());
                if (fp) {
                    t.fingerprint = *fp;
                }
            } else {
                std::ifstream f(it->path(), std::ios::binary);
                if (f) {
                    blake3_hasher hasher;
                    blake3_hasher_init(&hasher);
                    std::array<std::byte, scanBufferSize> buf{};
                    while (f.read(reinterpret_cast<char*>(buf.data()),
                                  static_cast<std::streamsize>(buf.size())) ||
                           f.gcount() > 0) {
                        blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(f.gcount()));
                    }
                    blake3_hasher_finalize(&hasher, t.fingerprint.data(), t.fingerprint.size());
                }
            }
            if (auto meta = kanariv::media::extractMetadata(t.path); meta) {
                t.title = std::move(meta->title);
                t.artist = std::move(meta->artist);
                t.album = std::move(meta->album);
                t.album_artist = std::move(meta->album_artist);
                t.genre = std::move(meta->genre);
                t.year = meta->year;
                t.track_num = meta->track_num;
                t.disc_num = meta->disc_num;
                t.duration = meta->duration;
                t.sample_rate = static_cast<uint32_t>(meta->sample_rate);
                t.channels = static_cast<uint32_t>(meta->channels);
                t.bitrate = meta->bitrate;
            }
            co_yield t;
        }
    }
}

auto scanTracksToVector(const std::filesystem::path& root, ScanMode mode)
    -> std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error> {
    std::vector<kanariv::types::Track> out;
    for (auto t : scanTracks(root, mode)) {
        out.push_back(std::move(t));
    }
    return out;
}

} // namespace kanariv::media
