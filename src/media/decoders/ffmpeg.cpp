#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <media/convert_loop.hpp>
#include <media/ffmpeg_error.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wglobal-module"
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic pop
#endif

namespace kanariv::media {

namespace {

using internal::ffErrorString; // readable av_strerror text for every failure path (M4)

// Probing bounds shared by every FFmpeg session (P1-1). Defaults (5 MB and an
// unbounded-ish analysis window) let a single open buffer megabytes per file;
// 1 MB / 0.5 s covers headers, Xing/Info tables and attached pictures.
constexpr int probeSizeBytes = 1048576;
constexpr int analyzeDurationUs = 500000;
constexpr const char* probeSizeOption = "1048576";
constexpr const char* analyzeDurationOption = "500000";

} // namespace

struct Decoder::Impl {
    Impl() = default;
    ~Impl();

    [[nodiscard]] uint32_t sampleRate() const noexcept;
    [[nodiscard]] uint32_t channels() const noexcept;
    [[nodiscard]] uint64_t totalFrames() const noexcept;

    struct DecodeResult {
        std::size_t frames{0};
        DecodeStatus status{DecodeStatus::Ok};
    };
    DecodeResult decode(std::span<float> out);

    kanariv::utils::Expected<void> seek(double seconds);

    bool init();

    void cleanup();

    struct PacketDeleter {
        void operator()(AVPacket* p) const noexcept {
            if (p)
                av_packet_free(&p);
        }
    };
    struct FrameDeleter {
        void operator()(AVFrame* p) const noexcept {
            if (p)
                av_frame_free(&p);
        }
    };
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    struct LayoutGuard {
        AVChannelLayout l{};
        LayoutGuard() = default;
        ~LayoutGuard() {
            av_channel_layout_uninit(&l);
        }
        LayoutGuard(const LayoutGuard&) = delete;
        LayoutGuard& operator=(const LayoutGuard&) = delete;
    };

    int convertFrameBuffered(AVFrame* frame);
    int drainResampler();
    // Receives every frame the codec currently holds into spill_. Returns the
    // number of frames consumed, or a negative FFmpeg/swr error. A codec-side
    // AVERROR_EOF sets codecEof_ and is not an error.
    int drainDecoder(AVFrame* frame);
    void resetDecodeState();
    std::size_t serveSpill(std::span<float> out, std::size_t totalDecoded, std::size_t frames);

    static int readCallback(void* opaque, uint8_t* buf, int bufSize);

    // AVIO seek: AVSEEK_SIZE queries size, AVSEEK_FORCE stripped. The callback
    // is only installed when the reader reports seekability.
    static int64_t seekCallback(void* opaque, int64_t offset, int whence);

    Reader* reader_{nullptr};
    kanariv::utils::Logger* logger_{nullptr};
    // Readable cause of the last init() failure. FFmpeg's own logging is muted
    // process-wide, so without this the caller sees only "FFmpeg init failed"
    // while the actual reason ("Header missing", "Invalid data ...") is lost (M4).
    std::string initError_{};
    AVFormatContext* fmt_{nullptr};
    AVCodecContext* dec_{nullptr};
    AVIOContext* avio_{nullptr};
    struct SwrContext* swr_{nullptr};
    int audioStreamIdx_{-1};
    uint32_t sampleRate_{0};
    uint32_t channels_{0};
    uint64_t totalFrames_{0};
    // Send/receive bookkeeping (M1). The packet in scratchPkt_ may have been
    // read but not yet accepted by the codec (EAGAIN): it stays pending and is
    // retried verbatim; it is unref'd only after avcodec_send_packet accepted
    // it, so no packet is dropped or delivered twice.
    bool packetPending_{false};
    bool eofReached_{false};
    bool flushed_{false};
    bool codecEof_{false};
    PacketPtr scratchPkt_{};
    FramePtr scratchFrame_{};
    static constexpr std::size_t convertFrames = 4096;
    std::vector<float> convertBuf_{};
    std::vector<float> spill_{};
    std::size_t spillPos_{0};
};

kanariv::utils::Expected<kanariv::types::TrackMetadata> extractMetadata(std::string_view path) {
    // FFmpeg takes a NUL-terminated UTF-8 path; a string_view is not
    // guaranteed to be terminated, so own the bytes first (M5).
    const std::string ownedPath(path);
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::NoMem,
                                                         "avformat_alloc_context failed"));
    }

    // Probing bounds must be in place before the context is opened:
    // avformat_open_input runs the format probe itself, so assigning them
    // afterwards bounded only avformat_find_stream_info (M13).
    AVDictionary* openOpts = nullptr;
    av_dict_set(&openOpts, "probesize", probeSizeOption, 0);
    av_dict_set(&openOpts, "fprobesize", probeSizeOption, 0);
    av_dict_set(&openOpts, "analyzeduration", analyzeDurationOption, 0);
    const int openRet = avformat_open_input(&fmt, ownedPath.c_str(), nullptr, &openOpts);
    av_dict_free(&openOpts);
    if (openRet < 0) {
        // avformat_open_input frees a caller-provided context on failure.
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                      "avformat_open_input failed: " + ffErrorString(openRet)));
    }

    const int infoRet = avformat_find_stream_info(fmt, nullptr);
    if (infoRet < 0) {
        avformat_close_input(&fmt);
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "avformat_find_stream_info failed: " +
                                                             ffErrorString(infoRet)));
    }

    kanariv::types::TrackMetadata meta;

    int audioStreamIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVDictionary* streamMeta =
        (audioStreamIdx >= 0) ? fmt->streams[audioStreamIdx]->metadata : nullptr;

    // Format metadata first (current behavior), then the audio stream's own
    // dict: Ogg-container tags live at stream level, invisible otherwise.
    // Aliases cover FFmpeg's per-format exports (ALBUMARTIST, DISCNUMBER,
    // WM/Year); first non-empty hit wins.
    auto getTag = [](AVDictionary* fmtMeta, AVDictionary* strMeta,
                     std::initializer_list<const char*> keys) -> std::string {
        for (auto* dict : {fmtMeta, strMeta}) {
            if (!dict)
                continue;
            for (const char* key : keys) {
                AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
                if (entry && entry->value && *entry->value)
                    return std::string(entry->value);
            }
        }
        return {};
    };

    meta.title = getTag(fmt->metadata, streamMeta, {"title"});
    meta.artist = getTag(fmt->metadata, streamMeta, {"artist"});
    meta.album = getTag(fmt->metadata, streamMeta, {"album"});
    meta.album_artist =
        getTag(fmt->metadata, streamMeta, {"album_artist", "albumartist", "album artist"});
    meta.genre = getTag(fmt->metadata, streamMeta, {"genre"});

    if (auto dateStr = getTag(fmt->metadata, streamMeta, {"date", "WM/Year", "year"});
        !dateStr.empty()) {
        try {
            meta.year = std::stoi(dateStr.substr(0, 4));
        } catch (...) {
        }
    }
    if (auto trackStr = getTag(fmt->metadata, streamMeta, {"track"}); !trackStr.empty()) {
        try {
            meta.track_num = std::stoi(trackStr);
        } catch (...) {
        }
    }
    if (auto discStr = getTag(fmt->metadata, streamMeta, {"disc", "discnumber"});
        !discStr.empty()) {
        try {
            meta.disc_num = std::stoi(discStr);
        } catch (...) {
        }
    }

    if (audioStreamIdx >= 0) {
        AVStream* stream = fmt->streams[audioStreamIdx];
        if (stream->codecpar) {
            meta.sample_rate = static_cast<int>(stream->codecpar->sample_rate);
            meta.channels = static_cast<int>(stream->codecpar->ch_layout.nb_channels);
            meta.bitrate = static_cast<int>(stream->codecpar->bit_rate);
        }
        if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
            meta.duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        } else if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            meta.duration = stream->duration * av_q2d(stream->time_base);
        }
    }

    avformat_close_input(&fmt);
    return meta;
}

uint32_t Decoder::Impl::sampleRate() const noexcept {
    return sampleRate_;
}

uint32_t Decoder::Impl::channels() const noexcept {
    return channels_;
}

uint64_t Decoder::Impl::totalFrames() const noexcept {
    return totalFrames_;
}

Decoder::Impl::DecodeResult Decoder::Impl::decode(std::span<float> out) {
    DecodeResult result{};
    if (out.empty())
        return result; // nothing requested: not an error
    if (!fmt_ || !dec_ || !swr_ || channels_ == 0) {
        result.status = DecodeStatus::Error;
        return result;
    }

    std::size_t frames = out.size() / channels_;
    if (frames == 0) {
        result.status = DecodeStatus::Error;
        return result;
    }

    std::size_t totalDecoded = 0;
    // Reusable scratch packet/frame (P-1). Lazily (re)allocated if init()
    // failed to pre-allocate or cleanup() released them.
    if (!scratchPkt_)
        scratchPkt_.reset(av_packet_alloc());
    if (!scratchFrame_)
        scratchFrame_.reset(av_frame_alloc());
    if (!scratchPkt_ || !scratchFrame_) {
        result.status = DecodeStatus::Error;
        return result;
    }
    AVPacket* pkt = scratchPkt_.get();
    AVFrame* frame = scratchFrame_.get();
    // Decode at most what `out` (plus the bounded spill_) can hold. Each
    // iteration serves spill_ first, then makes at most one send/receive step,
    // so swr_convert always sees the full convertFrames window (P-7).
    while (totalDecoded < frames) {
        totalDecoded += serveSpill(out, totalDecoded, frames);
        if (totalDecoded >= frames)
            break; // output full: status stays Ok, more audio may follow

        if (codecEof_) {
            result.status = DecodeStatus::Eof;
            break;
        }

        if (packetPending_) {
            // Invariant (M1): scratchPkt_ holds a packet the codec has not
            // accepted yet. It is retained across iterations and calls and is
            // unref'd only after a successful send, so EAGAIN never drops it
            // and the packet is never delivered twice.
            const int ret = avcodec_send_packet(dec_, pkt);
            if (ret == AVERROR(EAGAIN)) {
                // The codec still holds output: receive it, keep the packet
                // pending, and retry the send on the next iteration.
                if (drainDecoder(frame) <= 0) {
                    if (logger_)
                        logger_->warn("decoder stalled while draining for EAGAIN");
                    result.status = DecodeStatus::Error;
                    break;
                }
                continue;
            }
            if (ret == AVERROR_EOF) {
                // The codec is already flushed and holds no more frames.
                packetPending_ = false;
                av_packet_unref(pkt);
                codecEof_ = true;
                result.status = DecodeStatus::Eof;
                break;
            }
            if (ret < 0) {
                // InvalidData and friends: corrupt input, not end of stream.
                // Drop the packet so an Error-aware caller can resume after it,
                // then report the failure instead of silently continuing.
                packetPending_ = false;
                av_packet_unref(pkt);
                if (logger_)
                    logger_->warn("avcodec_send_packet failed: {}", ffErrorString(ret));
                result.status = DecodeStatus::Error;
                break;
            }
            packetPending_ = false;
            av_packet_unref(pkt);
            if (drainDecoder(frame) < 0) {
                result.status = DecodeStatus::Error;
                break;
            }
            continue;
        }

        if (eofReached_) {
            // Flush the codec once, drain every remaining frame, then drain the
            // resampler so its held-back tail is not lost.
            if (!flushed_) {
                const int ret = avcodec_send_packet(dec_, nullptr);
                if (ret == AVERROR(EAGAIN)) {
                    if (drainDecoder(frame) <= 0) {
                        result.status = DecodeStatus::Error;
                        break;
                    }
                    continue;
                }
                if (ret < 0 && ret != AVERROR_EOF) {
                    if (logger_)
                        logger_->warn("decoder flush failed: {}", ffErrorString(ret));
                    result.status = DecodeStatus::Error;
                    break;
                }
                flushed_ = true;
            }
            if (drainDecoder(frame) < 0) {
                result.status = DecodeStatus::Error;
                break;
            }
            if (drainResampler() < 0) {
                result.status = DecodeStatus::Error;
                break;
            }
            codecEof_ = true;
            totalDecoded += serveSpill(out, totalDecoded, frames);
            result.status = DecodeStatus::Eof;
            break;
        }

        av_packet_unref(pkt);
        const int ret = av_read_frame(fmt_, pkt);
        if (ret == AVERROR_EOF) {
            eofReached_ = true;
            continue;
        }
        if (ret < 0) {
            // Only AVERROR_EOF is a clean end (M2). AVERROR(EIO) from a reader
            // failure, AVERROR_INVALIDDATA and everything else are real errors
            // and must not truncate playback silently.
            if (logger_)
                logger_->warn("av_read_frame failed: {}", ffErrorString(ret));
            result.status = DecodeStatus::Error;
            break;
        }
        if (pkt->stream_index != audioStreamIdx_)
            continue; // non-audio packet: consumed and dropped
        packetPending_ = true;
    }

    // Deliver anything already converted -- including frames decoded just
    // before a mid-stream failure -- before reporting the final status.
    totalDecoded += serveSpill(out, totalDecoded, frames);
    result.frames = totalDecoded;
    return result;
}

kanariv::utils::Expected<void> Decoder::Impl::seek(double seconds) {
    if (seconds < 0.0 || !std::isfinite(seconds) || !fmt_ || !dec_ || audioStreamIdx_ < 0) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string_view("bad seconds")));
    }
    if (reader_ && !reader_->seekable()) {
        return std::unexpected(kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported, std::string_view("source is not seekable")));
    }
    AVStream* stream = fmt_->streams[audioStreamIdx_];
    if (!stream)
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         std::string_view("no stream")));
    // Use stream time_base for seeking - dec time_base is codec, not correct for container
    int64_t seekTarget = av_rescale_q(static_cast<int64_t>(seconds * AV_TIME_BASE),
                                      AVRational{1, AV_TIME_BASE}, stream->time_base);
    // Adjust by stream start_time if present
    if (stream->start_time != AV_NOPTS_VALUE)
        seekTarget += stream->start_time;
    else if (fmt_->start_time != AV_NOPTS_VALUE) {
        // fmt start_time is in AV_TIME_BASE units, convert to stream units
        seekTarget +=
            av_rescale_q(fmt_->start_time, AVRational{1, AV_TIME_BASE}, stream->time_base);
    }
    int ret = avformat_seek_file(fmt_, audioStreamIdx_, INT64_MIN, seekTarget, INT64_MAX, 0);
    if (ret < 0) {
        // Fallback to simple av_seek_frame
        ret = av_seek_frame(fmt_, audioStreamIdx_, seekTarget, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            // Both attempts failed and FFmpeg logging is muted, so carry the
            // av_strerror text in the returned message (M4).
            std::string message = "seek failed: " + ffErrorString(ret);
            if (logger_)
                logger_->warn("{}", message);
            return std::unexpected(
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, message));
        }
    }
    avcodec_flush_buffers(dec_);
    // flush resampler as well
    if (swr_) {
        if (const int ret = swr_init(swr_); ret < 0 && logger_)
            logger_->warn("swr_init after seek failed: {}", ffErrorString(ret));
    }
    // Drop converted-but-undelivered residue: positions before the seek
    // target must never surface after it. Also forget any packet the codec
    // had not accepted yet; it belongs to the pre-seek stream.
    spill_.clear();
    spillPos_ = 0;
    resetDecodeState();
    return {};
}

Decoder::Impl::~Impl() {
    cleanup();
}

int Decoder::Impl::drainDecoder(AVFrame* frame) {
    int received = 0;
    for (;;) {
        av_frame_unref(frame);
        const int ret = avcodec_receive_frame(dec_, frame);
        if (ret == AVERROR(EAGAIN))
            return received;
        if (ret == AVERROR_EOF) {
            codecEof_ = true;
            return received;
        }
        if (ret < 0) {
            if (logger_)
                logger_->warn("avcodec_receive_frame failed: {}", ffErrorString(ret));
            return ret;
        }
        ++received;
        const int converted = convertFrameBuffered(frame);
        if (converted < 0) {
            if (logger_)
                logger_->warn("swr_convert failed: {}", ffErrorString(converted));
            return converted;
        }
    }
}

void Decoder::Impl::resetDecodeState() {
    packetPending_ = false;
    eofReached_ = false;
    flushed_ = false;
    codecEof_ = false;
    if (scratchPkt_)
        av_packet_unref(scratchPkt_.get());
}

int Decoder::Impl::convertFrameBuffered(AVFrame* frame) {
    uint32_t ch = channels_;
    if (ch == 0 || !frame || frame->nb_samples <= 0)
        return 0;
    try {
        if (convertBuf_.size() < convertFrames * ch)
            convertBuf_.resize(convertFrames * ch);
        int appended = 0;
        // Feed the frame once, then keep pulling until swresample reports
        // empty: one swr_convert call truncates a frame larger than the window
        // and drops the filter-delay tail (the fixed spill loop lives in
        // pumpConvert so the same code is covered by a unit test).
        bool fed = false;
        auto convert = [&](float* out, std::size_t frames) {
            uint8_t* outPtrs[1] = {reinterpret_cast<uint8_t*>(out)};
            if (!fed) {
                fed = true;
                return swr_convert(swr_, outPtrs, static_cast<int>(frames),
                                   const_cast<const uint8_t**>(frame->extended_data),
                                   frame->nb_samples);
            }
            return swr_convert(swr_, outPtrs, static_cast<int>(frames), nullptr, 0);
        };
        auto append = [&](std::span<const float> block) {
            spill_.insert(spill_.end(), block.begin(), block.end());
            appended += static_cast<int>(block.size() / ch);
        };
        const int rc = internal::pumpConvert(
            std::span<float>(convertBuf_.data(), convertBuf_.size()), ch, convert, append);
        if (rc < 0)
            return rc;
        return appended;
    } catch (...) {
        return -1;
    }
}

int Decoder::Impl::drainResampler() {
    uint32_t ch = channels_;
    if (ch == 0)
        return 0;
    try {
        if (convertBuf_.size() < convertFrames * ch)
            convertBuf_.resize(convertFrames * ch);
        uint8_t* outPtrs[1] = {reinterpret_cast<uint8_t*>(convertBuf_.data())};
        // Pull out anything swr held back (oversize frame, filter delay) so
        // swr-internal storage stays bounded by one frame instead of growing
        // one residue per decoded frame.
        int appended = 0;
        int n = 0;
        while ((n = swr_convert(swr_, outPtrs, static_cast<int>(convertFrames), nullptr, 0)) > 0) {
            spill_.insert(spill_.end(), convertBuf_.data(),
                          convertBuf_.data() + static_cast<std::size_t>(n) * ch);
            appended += n;
        }
        return n < 0 ? n : appended;
    } catch (...) {
        return -1;
    }
}

std::size_t Decoder::Impl::serveSpill(std::span<float> out, std::size_t totalDecoded,
                                      std::size_t frames) {
    if (spillPos_ >= spill_.size() || channels_ == 0)
        return 0;
    std::size_t availFrames = (spill_.size() - spillPos_) / channels_;
    std::size_t want = frames - totalDecoded;
    std::size_t n = std::min(availFrames, want);
    if (n == 0)
        return 0;
    std::memcpy(out.data() + totalDecoded * channels_, spill_.data() + spillPos_,
                n * channels_ * sizeof(float));
    spillPos_ += n * channels_;
    if (spillPos_ == spill_.size()) {
        spill_.clear();
        spillPos_ = 0;
    }
    return n;
}

int Decoder::Impl::readCallback(void* opaque, uint8_t* buf, int bufSize) {
    auto* self = static_cast<Impl*>(opaque);
    if (!self->reader_)
        return AVERROR(EIO);
    std::span<std::byte> dst(reinterpret_cast<std::byte*>(buf), static_cast<std::size_t>(bufSize));
    std::size_t n = self->reader_->read(dst);
    if (n > 0)
        return static_cast<int>(n);
    // Only a real end-of-stream is EOF; an I/O failure must surface so
    // av_read_frame reports it instead of pretending the file ended (M2).
    return self->reader_->lastReadStatus() == ReadStatus::Error ? AVERROR(EIO) : AVERROR_EOF;
}

int64_t Decoder::Impl::seekCallback(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<Impl*>(opaque);
    if (!self->reader_)
        return AVERROR(EIO);
    // Handle AVSEEK_SIZE (0x10000) -- query file size without seeking
    if (whence & AVSEEK_SIZE) {
        int64_t sz = self->reader_->size();
        if (sz < 0)
            return AVERROR(EIO);
        return sz;
    }
    // Strip AVSEEK_FORCE flag (0x20000) if present
    int whenceMasked = whence & ~AVSEEK_FORCE;
    int w = SEEK_SET;
    if (whenceMasked == SEEK_CUR)
        w = SEEK_CUR;
    else if (whenceMasked == SEEK_END)
        w = SEEK_END;
    else if (whenceMasked == SEEK_SET)
        w = SEEK_SET;
    else {
        // Unknown whence after masking -- treat low 2 bits
        int low = whenceMasked & 0x3;
        if (low == SEEK_CUR)
            w = SEEK_CUR;
        else if (low == SEEK_END)
            w = SEEK_END;
        else
            w = SEEK_SET;
    }
    auto result = self->reader_->seek(offset, w);
    if (!result.has_value())
        return AVERROR(EIO);
    return self->reader_->tell();
}

bool Decoder::Impl::init() {
    initError_.clear();
    fmt_ = avformat_alloc_context();
    if (!fmt_) {
        initError_ = "avformat_alloc_context failed (out of memory)";
        return false;
    }

    // Allocate AVIOContext with our callbacks -- use larger buffer for high-rate FLAC/WAV
    // probing
    constexpr size_t bufferSize = 8192;
    const bool canSeek = reader_ && reader_->seekable();
    uint8_t* avioBuffer = static_cast<uint8_t*>(av_malloc(bufferSize));
    if (!avioBuffer) {
        initError_ = "av_malloc for the AVIO buffer failed (out of memory)";
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }

    avio_ = avio_alloc_context(avioBuffer, static_cast<int>(bufferSize), 0, this, &readCallback,
                               nullptr, canSeek ? &seekCallback : nullptr);
    if (!avio_) {
        initError_ = "avio_alloc_context failed (out of memory)";
        av_free(avioBuffer);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    // Non-seekable sources (pipes, one-shot host descriptors) must not
    // advertise seeking: FFmpeg otherwise probes/rewinds and reports a
    // confusing I/O failure instead of decoding forward.
    avio_->seekable = canSeek ? 1 : 0;

    fmt_->pb = avio_;
    fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Probe spam like "Header missing" / "CRC mismatch" is noise during decode and
    // used to mask restart bugs, so the process-wide level is pinned to QUIET here
    // (M4). Because that also mutes genuine failures, every failure site below
    // records the av_strerror() text into initError_, which Decoder::init() returns
    // to the caller -- the level is never the only channel for a real error.
    av_log_set_level(AV_LOG_QUIET);

    // Open input (probes format). The probing bounds are set on the context
    // *before* the open so avformat_open_input's own format probe is bounded
    // too; assigning them afterwards only bounded find_stream_info (M13).
    // `probesize` bounds stream-info reads, `fprobesize` bounds the
    // input-format probe (ignored gracefully when the option is absent).
    fmt_->probesize = probeSizeBytes;
    (void)av_opt_set_int(fmt_, "fprobesize", probeSizeBytes, 0);
    fmt_->max_analyze_duration = analyzeDurationUs;
    if (const int ret = avformat_open_input(&fmt_, nullptr, nullptr, nullptr); ret < 0) {
        initError_ = "avformat_open_input failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    if (const int ret = avformat_find_stream_info(fmt_, nullptr); ret < 0) {
        initError_ = "avformat_find_stream_info failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    // Find audio stream
    audioStreamIdx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx_ < 0) {
        initError_ = "av_find_best_stream found no audio stream: " + ffErrorString(audioStreamIdx_);
        cleanup();
        return false;
    }

    AVStream* stream = fmt_->streams[audioStreamIdx_];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        initError_ = "avcodec_find_decoder failed for codec id " +
                     std::to_string(static_cast<int>(stream->codecpar->codec_id));
        cleanup();
        return false;
    }

    dec_ = avcodec_alloc_context3(codec);
    if (!dec_) {
        initError_ = "avcodec_alloc_context3 failed (out of memory)";
        cleanup();
        return false;
    }

    if (const int ret = avcodec_parameters_to_context(dec_, stream->codecpar); ret < 0) {
        initError_ = "avcodec_parameters_to_context failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    if (const int ret = avcodec_open2(dec_, codec, nullptr); ret < 0) {
        initError_ = "avcodec_open2 failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    // Setup resampler to float interleaved -- preserve input channel layout exactly
    swr_ = swr_alloc();
    if (!swr_) {
        initError_ = "swr_alloc failed (out of memory)";
        cleanup();
        return false;
    }

    LayoutGuard inLayout;
    av_channel_layout_copy(&inLayout.l, &dec_->ch_layout);
    LayoutGuard outLayout;
    av_channel_layout_copy(&outLayout.l, &dec_->ch_layout);

    av_opt_set_chlayout(swr_, "in_chlayout", &inLayout.l, 0);
    av_opt_set_int(swr_, "in_sample_rate", dec_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_, "in_sample_fmt", dec_->sample_fmt, 0);
    av_opt_set_chlayout(swr_, "out_chlayout", &outLayout.l, 0);
    av_opt_set_int(swr_, "out_sample_rate", dec_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (const int ret = swr_init(swr_); ret < 0) {
        initError_ = "swr_init failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    // Pre-allocate reusable decode scratch (P-1). Lazy fallback in decode()
    // covers the (near-impossible) alloc failure here.
    scratchPkt_.reset(av_packet_alloc());
    scratchFrame_.reset(av_frame_alloc());
    if (!scratchPkt_ || !scratchFrame_) {
        initError_ = "av_packet_alloc/av_frame_alloc failed (out of memory)";
        cleanup();
        return false;
    }

    sampleRate_ = static_cast<uint32_t>(dec_->sample_rate);
    channels_ = static_cast<uint32_t>(dec_->ch_layout.nb_channels);

    // Estimate total frames from duration -- try all sources
    if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0) {
        totalFrames_ = static_cast<uint64_t>(fmt_->duration * sampleRate_ / AV_TIME_BASE);
    } else if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        totalFrames_ =
            static_cast<uint64_t>(stream->duration * av_q2d(stream->time_base) * sampleRate_);
    } else if (stream->nb_frames > 0) {
        totalFrames_ = stream->nb_frames;
    } else if (stream->codecpar->bit_rate > 0 && fmt_->duration == AV_NOPTS_VALUE) {
        int64_t sz = reader_ ? reader_->size() : -1;
        if (sz > 0 && stream->codecpar->bit_rate > 0) {
            // fallback for CBR MP3 with no duration
            double secs = (sz * 8.0) / stream->codecpar->bit_rate;
            totalFrames_ = static_cast<uint64_t>(secs * sampleRate_);
        } else {
            totalFrames_ = 0;
        }
    } else {
        totalFrames_ = 0; // Unknown -- will still decode to EOF
    }

    resetDecodeState();
    return true;
}

void Decoder::Impl::cleanup() {
    resetDecodeState();
    scratchPkt_.reset();
    scratchFrame_.reset();
    convertBuf_.clear();
    spill_.clear();
    spillPos_ = 0;
    if (swr_) {
        swr_free(&swr_);
        swr_ = nullptr;
    }
    if (dec_) {
        avcodec_free_context(&dec_);
        dec_ = nullptr;
    }
    if (fmt_) {
        // Prevent avformat_close_input from freeing our custom pb.
        // With AVFMT_FLAG_CUSTOM_IO, avformat_close_input does NOT free pb.
        fmt_->pb = nullptr;
        avformat_close_input(&fmt_);
        fmt_ = nullptr;
    }
    if (avio_) {
        // avio_context_free also frees the buffer allocated via av_malloc
        avio_context_free(&avio_);
        avio_ = nullptr;
    }
}

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}

Decoder::~Decoder() = default;

Decoder::Decoder(Decoder&&) noexcept = default;

Decoder& Decoder::operator=(Decoder&&) noexcept = default;

bool Decoder::probe(std::span<const std::byte> data) noexcept {
    // When FFmpeg is available, be permissive - try to decode anything
    // FFmpeg's avformat_open_input will fail gracefully if format not supported
    // Only skip obviously empty data
    return data.size() >= 4;
}

kanariv::utils::Expected<std::unique_ptr<Decoder>> Decoder::open(Reader& reader,
                                                                 kanariv::utils::Logger* logger) {
    auto decoder = std::make_unique<Decoder>();
    decoder->logger_ = logger;

    const bool seekable = reader.seekable();
    int64_t orig = reader.tell();
    if (orig < 0)
        orig = 0;

    if (seekable) {
        // Probe from start: seek to 0, read 32 bytes, then restore BEFORE
        // init -- ca_decode.c:60-72. A seekable reader can replay the bytes.
        constexpr std::size_t probeBytes = 32;
        std::array<std::byte, probeBytes> buf{};
        (void)reader.seek(0, SEEK_SET);
        const std::size_t n = reader.read(std::span<std::byte>(buf.data(), buf.size()));
        (void)reader.seek(orig, SEEK_SET);

        if (!Decoder::probe(std::span<const std::byte>(buf.data(), n))) {
            return std::unexpected(kanariv::utils::makeError(
                kanariv::utils::StatusCode::Unsupported, std::string_view("no decoder matched")));
        }
    }
    // Non-seekable sources (pipes, one-shot host descriptors) are handed to
    // FFmpeg at offset 0 without a read-ahead probe: the 32 bytes could not be
    // restored, so consuming them would start playback mid-stream. FFmpeg's
    // own format probe (init()) reads from the real start and rejects garbage.

    // FFmpeg's AVIO owns the position after open and must start at 0. Always
    // seek to 0 before init; do NOT restore after success or AVIO pos and file
    // pos will diverge (causing Header missing/CRC).
    if (seekable)
        (void)reader.seek(0, SEEK_SET);
    auto initResult = decoder->init(reader);
    if (initResult.has_value()) {
        if (logger)
            logger->debug("decoder opened rate={} channels={}", decoder->sampleRate(),
                          decoder->channels());
        return kanariv::utils::Expected<std::unique_ptr<Decoder>>{std::move(decoder)};
    }

    // Failure: the decoder is discarded; restore a seekable reader.
    if (seekable)
        (void)reader.seek(orig, SEEK_SET);
    if (logger)
        logger->debug("decoder open failed: {}", initResult.error().message);
    return std::unexpected(initResult.error());
}

kanariv::utils::Expected<void> Decoder::init(Reader& reader) {
    impl_->reader_ = &reader;
    impl_->logger_ = logger_;
    if (!impl_->init()) {
        // Report the recorded cause, not just "init failed": FFmpeg logging is
        // muted process-wide, so initError_ is the only readable explanation (M4).
        std::string message =
            impl_->initError_.empty() ? std::string{"FFmpeg init failed"} : impl_->initError_;
        if (logger_)
            logger_->error("decoder init failed: " + message);
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Unsupported, message));
    }
    return {};
}

uint32_t Decoder::sampleRate() const noexcept {
    return impl_ ? impl_->sampleRate() : 0;
}

uint32_t Decoder::channels() const noexcept {
    return impl_ ? impl_->channels() : 0;
}

uint64_t Decoder::totalFrames() const noexcept {
    return impl_ ? impl_->totalFrames() : 0;
}

std::size_t Decoder::decode(std::span<float> out) {
    if (!impl_)
        return 0;
    return impl_->decode(out).frames;
}

std::size_t Decoder::decode(std::span<float> out, DecodeStatus* status) {
    if (!status)
        return decode(out);
    if (!impl_) {
        *status = DecodeStatus::Error;
        return 0;
    }
    const auto result = impl_->decode(out);
    *status = result.status;
    return result.frames;
}

kanariv::utils::Expected<void> Decoder::seek(double seconds) {
    if (!impl_)
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         std::string_view("moved-from decoder")));
    return impl_->seek(seconds);
}

} // namespace kanariv::media
