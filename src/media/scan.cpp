#include <blake3.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/fingerprint.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/generator.hpp>
#include <media/convert_loop.hpp>
#include <media/ffmpeg_error.hpp>
#include <media/scan_walker.hpp>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace kanariv::media::internal {

namespace {

using kanariv::utils::Error;
using kanariv::utils::makeError;
using kanariv::utils::StatusCode;

/// Full-file BLAKE3: the `ScanMode::Full` counterpart of `computeFingerprint`.
std::expected<std::array<uint8_t, 32>, Error>
fullFileFingerprint(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::unexpected{makeError(StatusCode::Io, "cannot open file for hashing")};
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<std::byte, 8192> buf{};
    while (f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size())) ||
           f.gcount())
        blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(f.gcount()));
    // A read error mid-file would hash a prefix and yield a digest that looks
    // valid; report it instead of persisting a wrong fingerprint.
    if (f.bad())
        return std::unexpected{makeError(StatusCode::Io, "read error while hashing")};
    std::array<uint8_t, 32> out{};
    blake3_hasher_finalize(&hasher, out.data(), out.size());
    return out;
}

FingerprintProvider defaultFingerprintProvider(ScanMode mode) {
    if (mode == ScanMode::Full)
        return
            [](const std::filesystem::path& path, ScanMode) { return fullFileFingerprint(path); };
    return [](const std::filesystem::path& path, ScanMode) { return computeFingerprint(path); };
}

/// Copies the tags FFmpeg can supply into `t`; a metadata failure is not fatal
/// (the file is still usable without tags), so it leaves them empty.
void applyMetadata(kanariv::types::Track& t) {
    auto meta = kanariv::media::extractMetadata(t.path);
    if (!meta)
        return;
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

/// Builds a Track for one audio file. Never returns a Track without a usable
/// fingerprint: a hashing failure (or the all-zero sentinel) is an error.
std::expected<kanariv::types::Track, Error> buildTrack(const std::filesystem::path& path,
                                                       ScanMode mode,
                                                       const FingerprintProvider& fingerprint) {
    kanariv::types::Track t;
    // UTF-8, not generic_string(): the latter throws on Windows for characters
    // outside the ANSI code page, which aborted the whole scan at the first
    // such file (and surfaced as a broken pipe).
    t.path = kanariv::utils::detail::pathToUtf8(path);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec)
        t.size = static_cast<int64_t>(size);
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec)
        t.mtime = static_cast<int64_t>(mtime.time_since_epoch().count());

    auto fp = fingerprint(path, mode);
    if (!fp) {
        return std::unexpected{
            makeError(StatusCode::Io, "fingerprint failed: " + fp.error().message)};
    }
    if (isZeroFingerprint(*fp))
        return std::unexpected{makeError(StatusCode::Io, "fingerprint is the all-zero sentinel")};
    t.fingerprint = *fp;

    applyMetadata(t);
    return t;
}

} // namespace

struct ScanWalker::Impl {
    std::filesystem::path root;
    ScanMode mode{ScanMode::Sampled};
    ScanIssueSink onIssue;
    FingerprintProvider fingerprint;
    std::filesystem::recursive_directory_iterator it;
    std::filesystem::recursive_directory_iterator end;
    bool opened{false};
    std::optional<kanariv::utils::Error> walkError;

    void report(ScanIssueKind kind, std::string path, std::string message) {
        if (onIssue)
            onIssue(ScanIssue{kind, std::move(path), std::move(message)});
    }

    void failWalk(const std::filesystem::path& where, const std::string& cause) {
        const std::string whereUtf8 = kanariv::utils::detail::pathToUtf8(where);
        auto error =
            makeError(StatusCode::Io, "directory walk failed at '" + whereUtf8 + "': " + cause);
        report(ScanIssueKind::WalkError, whereUtf8, error.message);
        walkError = std::move(error);
        // Standard-library contract: an iterator whose increment failed is
        // end, but be explicit so the loop cannot resume on stale state.
        it = end;
    }
};

ScanWalker::ScanWalker(std::filesystem::path root, ScanMode mode, ScanIssueSink onIssue,
                       FingerprintProvider fingerprint)
    : impl_(std::make_unique<Impl>()) {
    impl_->root = std::move(root);
    impl_->mode = mode;
    impl_->onIssue = std::move(onIssue);
    impl_->fingerprint = fingerprint ? std::move(fingerprint) : defaultFingerprintProvider(mode);
}

ScanWalker::~ScanWalker() = default;

std::expected<void, kanariv::utils::Error> ScanWalker::open() {
    if (!impl_)
        return std::unexpected{makeError(StatusCode::Internal, "moved-from walker")};
    std::error_code ec;
    if (!std::filesystem::exists(impl_->root, ec)) {
        impl_->report(ScanIssueKind::WalkError, kanariv::utils::detail::pathToUtf8(impl_->root),
                      "scan root does not exist");
        return std::unexpected{
            makeError(StatusCode::NotFound, "scan root does not exist: " +
                                                kanariv::utils::detail::pathToUtf8(impl_->root))};
    }
    // The constructor's error_code is checked explicitly: without it a
    // permission or I/O problem would look like an empty directory.
    impl_->it = std::filesystem::recursive_directory_iterator(
        impl_->root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        const std::string cause = ec.message();
        impl_->failWalk(impl_->root, cause);
        return std::unexpected{makeError(
            StatusCode::Io, "cannot open scan root '" +
                                kanariv::utils::detail::pathToUtf8(impl_->root) + "': " + cause)};
    }
    impl_->opened = true;
    return {};
}

std::optional<kanariv::types::Track> ScanWalker::next() {
    if (!impl_ || !impl_->opened)
        return std::nullopt;
    while (impl_->it != impl_->end) {
        // Copy the path: the iterator is invalidated by increment() below,
        // which must happen before any return so the next call continues past
        // this entry instead of re-processing it forever.
        const auto path = impl_->it->path();
        std::error_code entryEc;
        const bool regular = impl_->it->is_regular_file(entryEc);
        std::optional<kanariv::types::Track> result;
        if (entryEc) {
            impl_->report(ScanIssueKind::FileSkipped, kanariv::utils::detail::pathToUtf8(path),
                          entryEc.message());
        } else if (regular && kanariv::media::hasAudioExt(path)) {
            if (auto track = buildTrack(path, impl_->mode, impl_->fingerprint))
                result = std::move(*track);
            else
                impl_->report(ScanIssueKind::FileSkipped, kanariv::utils::detail::pathToUtf8(path),
                              track.error().message);
        }
        std::error_code stepEc;
        impl_->it.increment(stepEc);
        if (stepEc) {
            impl_->failWalk(path, stepEc.message());
            return std::nullopt;
        }
        if (result)
            return result;
    }
    return std::nullopt;
}

const std::optional<kanariv::utils::Error>& ScanWalker::walkError() const noexcept {
    static const std::optional<kanariv::utils::Error> none{};
    return impl_ ? impl_->walkError : none;
}

} // namespace kanariv::media::internal

namespace kanariv::media {

kanariv::utils::Generator<kanariv::types::Track> scanTracks(const std::filesystem::path& root,
                                                            ScanMode mode) {
    return scanTracks(root, mode, ScanIssueSink{});
}

kanariv::utils::Generator<kanariv::types::Track> scanTracks(const std::filesystem::path& root,
                                                            ScanMode mode, ScanIssueSink onIssue) {
    // The sink is taken by value: a coroutine body runs only once iteration
    // starts, so a reference to a caller temporary could dangle by then.
    internal::ScanWalker walker(root, mode, std::move(onIssue));
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        co_return;
    if (auto opened = walker.open(); !opened)
        co_return;
    while (auto track = walker.next())
        co_yield std::move(*track);
}

std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode) {
    return scanTracksToVector(root, mode, ScanIssueSink{});
}

std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode, ScanIssueSink onIssue) {
    internal::ScanWalker walker(root, mode, std::move(onIssue));
    if (auto opened = walker.open(); !opened)
        return std::unexpected{opened.error()};
    std::vector<kanariv::types::Track> out;
    while (auto track = walker.next())
        out.push_back(std::move(*track));
    // Discard the partial vector: a truncated walk must not look like a
    // complete (smaller) library.
    if (const auto& walkError = walker.walkError())
        return std::unexpected{*walkError};
    return out;
}

kanariv::utils::Expected<double> computeLoudnessGain(std::string_view path) {
    // Direct FFmpeg decode (same pattern as extractMetadata -- no custom AVIO,
    // so avformat_close_input frees everything and there is nothing to leak).
    // FFmpeg needs a NUL-terminated UTF-8 path; a string_view is not guaranteed
    // to be terminated, so own the bytes first (M5).
    const std::string ownedPath(path);
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    // Every FFmpeg free helper nulls its pointer, so this is safe on any path,
    // including one that fails before the handle was created.
    auto release = [&] {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    };
    auto fail = [&](kanariv::utils::StatusCode code,
                    std::string message) -> std::expected<double, kanariv::utils::Error> {
        release();
        return std::unexpected{kanariv::utils::makeError(code, message)};
    };

    // Bound probing before the open; assigning these after it only bounded
    // avformat_find_stream_info (M13).
    AVDictionary* openOpts = nullptr;
    av_dict_set(&openOpts, "probesize", "1048576", 0);
    av_dict_set(&openOpts, "fprobesize", "1048576", 0);
    av_dict_set(&openOpts, "analyzeduration", "500000", 0);
    const int openRet = avformat_open_input(&fmt, ownedPath.c_str(), nullptr, &openOpts);
    av_dict_free(&openOpts);
    if (openRet < 0) {
        // avformat_open_input frees a caller-provided context on failure.
        fmt = nullptr;
        return fail(kanariv::utils::StatusCode::Io,
                    "avformat_open_input failed: " + internal::ffErrorString(openRet));
    }

    const int infoRet = avformat_find_stream_info(fmt, nullptr);
    if (infoRet < 0) {
        return fail(kanariv::utils::StatusCode::Io,
                    "avformat_find_stream_info failed: " + internal::ffErrorString(infoRet));
    }

    const int audioStreamIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx < 0) {
        return fail(kanariv::utils::StatusCode::Unsupported,
                    "no audio stream: " + internal::ffErrorString(audioStreamIdx));
    }

    AVStream* stream = fmt->streams[audioStreamIdx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        return fail(kanariv::utils::StatusCode::Unsupported,
                    std::string("no decoder found for codec ") +
                        avcodec_get_name(stream->codecpar->codec_id));
    }

    dec = avcodec_alloc_context3(codec);
    if (!dec)
        return fail(kanariv::utils::StatusCode::NoMem, "avcodec_alloc_context3 failed");

    const int paramsRet = avcodec_parameters_to_context(dec, stream->codecpar);
    if (paramsRet < 0) {
        return fail(kanariv::utils::StatusCode::Io,
                    "avcodec_parameters_to_context failed: " + internal::ffErrorString(paramsRet));
    }

    const int openCodecRet = avcodec_open2(dec, codec, nullptr);
    if (openCodecRet < 0) {
        return fail(kanariv::utils::StatusCode::Io,
                    "avcodec_open2 failed: " + internal::ffErrorString(openCodecRet));
    }

    // Resampler to float interleaved (same target format as the engine's
    // Decoder::Impl), so RMS is computed on a consistent sample format.
    swr = swr_alloc();
    if (!swr)
        return fail(kanariv::utils::StatusCode::NoMem, "swr_alloc failed");

    {
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
        const int swrRet = swr_init(swr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        if (swrRet < 0) {
            return fail(kanariv::utils::StatusCode::Io,
                        "swr_init failed: " + internal::ffErrorString(swrRet));
        }
    }

    const uint32_t sr = dec->sample_rate ? dec->sample_rate : 48000;
    const uint32_t ch = dec->ch_layout.nb_channels ? dec->ch_layout.nb_channels : 2;

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame)
        return fail(kanariv::utils::StatusCode::NoMem, "av_packet/av_frame alloc failed");

    // ReplayGain 1.0 "head" filter: first-order high-pass at ~65 Hz.
    // y[n] = x[n] - x[n-1] + a * y[n-1],  a = exp(-2*pi*fc/sr)
    const double fc = 65.0;
    const double a = std::exp(-2.0 * 3.14159265358979323846 * fc / (double)sr);
    double hp_x1 = 0.0, hp_y1 = 0.0;

    double sumSq = 0.0;
    uint64_t totalSamples = 0;

    constexpr std::size_t convertFrames = 4096;
    std::vector<float> convertBuf(convertFrames * ch);
    auto consume = [&](std::span<const float> block) {
        for (float sample : block) {
            const double x = static_cast<double>(sample);
            const double y = x - hp_x1 + a * hp_y1;
            hp_x1 = x;
            hp_y1 = y;
            sumSq += y * y;
            ++totalSamples;
        }
    };

    // Converts one decoded frame through swr and folds every converted sample
    // into the RMS; `source == nullptr` flushes the resampler instead. The pump
    // repeats swr_convert until it reports empty, so frames larger than the
    // 4096-frame window are not truncated and swr's filter delay is kept
    // (mirrors Decoder::Impl::convertFrameBuffered).
    auto pumpSwr = [&](const AVFrame* source) {
        bool fed = false;
        return internal::pumpConvert(
            convertBuf, ch,
            [&](float* out, std::size_t frames) {
                uint8_t* outPtrs[1] = {reinterpret_cast<uint8_t*>(out)};
                const uint8_t** in = nullptr;
                int inSamples = 0;
                if (source && !fed) {
                    in = const_cast<const uint8_t**>(source->extended_data);
                    inSamples = source->nb_samples;
                    fed = true;
                }
                return swr_convert(swr, outPtrs, static_cast<int>(frames), in, inSamples);
            },
            consume);
    };

    // Drains every frame the codec holds right now. Returns the number of
    // frames converted, or a negative FFmpeg/swr error.
    auto drainFrames = [&]() -> int {
        int received = 0;
        for (;;) {
            av_frame_unref(frame);
            const int rec = avcodec_receive_frame(dec, frame);
            if (rec == AVERROR(EAGAIN) || rec == AVERROR_EOF)
                return received;
            if (rec < 0)
                return rec;
            ++received;
            const int rc = pumpSwr(frame);
            if (rc < 0)
                return rc;
        }
    };

    for (;;) {
        const int readRet = av_read_frame(fmt, pkt);
        if (readRet == AVERROR_EOF)
            break;
        if (readRet < 0) {
            return fail(kanariv::utils::StatusCode::Io,
                        "av_read_frame failed: " + internal::ffErrorString(readRet));
        }
        if (pkt->stream_index != audioStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }
        for (;;) {
            const int sendRet = avcodec_send_packet(dec, pkt);
            if (sendRet == AVERROR(EAGAIN)) {
                // The codec still holds output: receive it, then retry the
                // same packet instead of dropping audio.
                if (drainFrames() <= 0) {
                    av_packet_unref(pkt);
                    return fail(kanariv::utils::StatusCode::Internal,
                                "decoder stalled while draining for EAGAIN");
                }
                continue;
            }
            av_packet_unref(pkt);
            if (sendRet < 0) {
                return fail(kanariv::utils::StatusCode::Io,
                            "avcodec_send_packet failed: " + internal::ffErrorString(sendRet));
            }
            break;
        }
        const int drained = drainFrames();
        if (drained < 0) {
            return fail(kanariv::utils::StatusCode::Io,
                        "decode failed: " + internal::ffErrorString(drained));
        }
    }

    // Flush the codec once, then the resampler: the last frames and swr's
    // filter-delay tail are part of the loudness, not padding.
    int flushRet = avcodec_send_packet(dec, nullptr);
    if (flushRet == AVERROR(EAGAIN)) {
        if (drainFrames() < 0)
            return fail(kanariv::utils::StatusCode::Io, "decode flush failed during drain");
        flushRet = avcodec_send_packet(dec, nullptr);
    }
    if (flushRet < 0 && flushRet != AVERROR_EOF) {
        return fail(kanariv::utils::StatusCode::Io,
                    "decoder flush failed: " + internal::ffErrorString(flushRet));
    }
    const int flushed = drainFrames();
    if (flushed < 0) {
        return fail(kanariv::utils::StatusCode::Io,
                    "decode failed while flushing: " + internal::ffErrorString(flushed));
    }
    const int swrFlush = pumpSwr(nullptr);
    if (swrFlush < 0) {
        return fail(kanariv::utils::StatusCode::Io,
                    "swr_convert flush failed: " + internal::ffErrorString(swrFlush));
    }

    release();

    if (totalSamples == 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         "no audio samples decoded")};
    }

    const double rms = std::sqrt(sumSq / (double)totalSamples);
    if (rms <= 0.0) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "silent audio")};
    }

    const double dbfs = 20.0 * std::log10(rms);
    return -14.0 - dbfs;
}

} // namespace kanariv::media
