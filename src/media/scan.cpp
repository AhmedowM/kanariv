#include <blake3.h>

#include <array>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/generator.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <media/fingerprint.hpp>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>

namespace kanariv::media {

kanariv::utils::Generator<kanariv::types::Track> scanTracks(const std::filesystem::path& root,
                                                          ScanMode mode) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        co_return;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec) && kanariv::media::hasAudioExt(it->path())) {
            kanariv::types::Track t;
            // UTF-8, not generic_string(): the latter throws on Windows for
            // characters outside the ANSI code page, which aborted the whole
            // scan at the first such file (and surfaced as a broken pipe).
            t.path = kanariv::utils::detail::pathToUtf8(it->path());
            std::error_code e2;
            auto sz = it->file_size(e2);
            if (!e2)
                t.size = (int64_t)sz;
            auto ftime = it->last_write_time(e2);
            if (!e2)
                t.mtime = (int64_t)ftime.time_since_epoch().count();
            if (mode == ScanMode::Sampled) {
                auto fp = internal::computeFingerprint(it->path());
                if (fp)
                    t.fingerprint = *fp;
            } else {
                std::ifstream f(it->path(), std::ios::binary);
                if (f) {
                    blake3_hasher hasher;
                    blake3_hasher_init(&hasher);
                    std::array<std::byte, 8192> buf{};
                    while (f.read(reinterpret_cast<char*>(buf.data()),
                                  static_cast<std::streamsize>(buf.size())) ||
                           f.gcount())
                        blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(f.gcount()));
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

std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode) {
    std::vector<kanariv::types::Track> out;
    for (auto t : scanTracks(root, mode))
        out.push_back(std::move(t));
    return out;
}

kanariv::utils::Expected<double> computeReplayGainTrack(std::string_view path) {
    // Open the file for decoding.
    auto pathW = kanariv::utils::detail::pathFromUtf8(std::string(path));
    auto rRes = kanariv::media::FileReader::open(pathW);
    if (!rRes)
        return std::unexpected{rRes.error()};
    auto reader = std::move(rRes.value());
    auto dRes = kanariv::media::Decoder::open(*reader);
    if (!dRes)
        return std::unexpected{dRes.error()};
    auto& decoder = *dRes.value();

    const uint32_t sr = decoder.sampleRate() ? decoder.sampleRate() : 48000;
    const uint32_t ch = decoder.channels() ? decoder.channels() : 2;

    // ReplayGain head filter: first-order high-pass at ~65 Hz.
    // y[n] = x[n] - x[n-1] + a * y[n-1],  a = exp(-2*pi*fc/sr)
    const double fc = 65.0;
    const double a = std::exp(-2.0 * 3.14159265358979323846 * fc / (double)sr);
    double hp_x1 = 0.0, hp_y1 = 0.0;

    double sumSq = 0.0;
    uint64_t totalSamples = 0;
    constexpr std::size_t chunkFrames = 4096;
    std::vector<float> buf(chunkFrames * ch);

    while (true) {
        std::size_t frames = decoder.decode(std::span<float>(buf.data(), buf.size()));
        if (frames == 0)
            break;
        std::size_t samples = frames * ch;
        for (std::size_t i = 0; i < samples; ++i) {
            double x = (double)buf[i];
            // Head filter (high-pass).
            double y = x - hp_x1 + a * hp_y1;
            hp_x1 = x;
            hp_y1 = y;
            sumSq += y * y;
            ++totalSamples;
        }
    }

    if (totalSamples == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "no audio samples decoded")};

    double rms = std::sqrt(sumSq / (double)totalSamples);
    if (rms <= 0.0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "silent audio")};

    // dBFS of filtered RMS.
    double dbfs = 20.0 * std::log10(rms);
    // ReplayGain reference: 89 dB SPL. Approximate digital-to-acoustic
    // calibration: -20 dBFS RMS broadband pink noise ~= 83 dB SPL.
    // gain_dB = target(89) - measured(83 + (dbfs - (-20)))
    //         = 89 - 83 - dbfs - 20
    //         = -14 - dbfs
    double gainDb = -14.0 - dbfs;
    return gainDb;
}

} // namespace kanariv::media
