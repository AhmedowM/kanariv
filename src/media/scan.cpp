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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

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
    // Direct FFmpeg decode (same pattern as extractMetadata -- no custom AVIO,
    // so avformat_close_input frees everything and there is nothing to leak).
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::NoMem, "avformat_alloc_context failed")};
    }

    if (avformat_open_input(&fmt, path.data(), nullptr, nullptr) < 0) {
        avformat_free_context(fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Io, "avformat_open_input failed")};
    }

    fmt->probesize = 1048576;
    fmt->max_analyze_duration = 500000;

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Io, "avformat_find_stream_info failed")};
    }

    int audioStreamIdx =
        av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx < 0) {
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported, "no audio stream")};
    }

    AVStream* stream = fmt->streams[audioStreamIdx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported, "no decoder found")};
    }

    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!dec) {
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::NoMem, "avcodec_alloc_context3 failed")};
    }

    if (avcodec_parameters_to_context(dec, stream->codecpar) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Io, "avcodec_parameters_to_context failed")};
    }

    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Io, "avcodec_open2 failed")};
    }

    // Resampler to float interleaved (same target format as the engine's
    // Decoder::Impl), so RMS is computed on a consistent sample format.
    SwrContext* swr = swr_alloc();
    if (!swr) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::NoMem, "swr_alloc failed")};
    }

    AVChannelLayout inLayout{};
    AVChannelLayout outLayout{};
    av_channel_layout_copy(&inLayout, &dec->ch_layout);
    av_channel_layout_copy(&outLayout, &dec->ch_layout);

    av_opt_set_chlayout(swr, "in_chlayout", &inLayout, 0);
    av_opt_set_int(swr, "in_sample_rate", dec->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", dec->sample_fmt, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &outLayout, 0);
    av_opt_set_int(swr, "out_sample_rate", dec->sample_rate, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(swr) < 0) {
        swr_free(&swr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Io, "swr_init failed")};
    }

    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);

    const uint32_t sr = dec->sample_rate ? dec->sample_rate : 48000;
    const uint32_t ch = dec->ch_layout.nb_channels ? dec->ch_layout.nb_channels : 2;

    // ReplayGain head filter: first-order high-pass at ~65 Hz.
    // y[n] = x[n] - x[n-1] + a * y[n-1],  a = exp(-2*pi*fc/sr)
    const double fc = 65.0;
    const double a = std::exp(-2.0 * 3.14159265358979323846 * fc / (double)sr);
    double hp_x1 = 0.0, hp_y1 = 0.0;

    double sumSq = 0.0;
    uint64_t totalSamples = 0;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::NoMem, "av_packet/av_frame alloc failed")};
    }

    constexpr std::size_t convertFrames = 4096;
    std::vector<float> convertBuf(convertFrames * ch);
    uint8_t* outPtrs[1] = {reinterpret_cast<uint8_t*>(convertBuf.data())};

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != audioStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(dec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }

        av_frame_unref(frame);
        while (avcodec_receive_frame(dec, frame) >= 0) {
            int n = swr_convert(swr, outPtrs, static_cast<int>(convertFrames),
                                const_cast<const uint8_t**>(frame->extended_data),
                                frame->nb_samples);
            if (n > 0) {
                std::size_t samples = static_cast<std::size_t>(n) * ch;
                for (std::size_t i = 0; i < samples; ++i) {
                    double x = (double)convertBuf[i];
                    double y = x - hp_x1 + a * hp_y1;
                    hp_x1 = x;
                    hp_y1 = y;
                    sumSq += y * y;
                    ++totalSamples;
                }
            }
        }
        av_packet_unref(pkt);
    }

    // Flush decoder
    avcodec_send_packet(dec, nullptr);
    for (;;) {
        av_frame_unref(frame);
        int ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            break;
        int n = swr_convert(swr, outPtrs, static_cast<int>(convertFrames),
                            const_cast<const uint8_t**>(frame->extended_data),
                            frame->nb_samples);
        if (n > 0) {
            std::size_t samples = static_cast<std::size_t>(n) * ch;
            for (std::size_t i = 0; i < samples; ++i) {
                double x = (double)convertBuf[i];
                double y = x - hp_x1 + a * hp_y1;
                hp_x1 = x;
                hp_y1 = y;
                sumSq += y * y;
                ++totalSamples;
            }
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);

    if (totalSamples == 0) {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, "no audio samples decoded")};
    }

    double rms = std::sqrt(sumSq / (double)totalSamples);
    if (rms <= 0.0) {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, "silent audio")};
    }

    double dbfs = 20.0 * std::log10(rms);
    double gainDb = -14.0 - dbfs;
    return gainDb;
}

} // namespace kanariv::media
