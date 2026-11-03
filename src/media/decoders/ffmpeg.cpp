#include <algorithm>
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
#include <kanariv/utils/result.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wglobal-module"
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic pop
#endif

namespace kanariv::media {

constexpr int64_t probesizeDefault = 1048576;
constexpr int64_t maxAnalyzeDurationUs = 500000;
constexpr int whenceMaskLowBits = 0x3;
constexpr double bitsPerByte = 8.0;

struct Decoder::Impl {
    Impl() = default;
    ~Impl();

    [[nodiscard]] auto sampleRate() const noexcept -> uint32_t;
    [[nodiscard]] auto channels() const noexcept -> uint32_t;
    [[nodiscard]] auto totalFrames() const noexcept -> uint64_t;

    auto decode(std::span<float> out) -> std::size_t;

    auto seek(double seconds) -> kanariv::utils::Expected<void>;

    auto init() -> bool;

    void cleanup();

    struct PacketDeleter {
        void operator()(AVPacket* p) const noexcept {
            if (p != nullptr) {
                av_packet_free(&p);
            }
        }
    };
    struct FrameDeleter {
        void operator()(AVFrame* p) const noexcept {
            if (p != nullptr) {
                av_frame_free(&p);
            }
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
        auto operator=(const LayoutGuard&) -> LayoutGuard& = delete;
    };

    auto convertFrameBuffered(AVFrame* frame) -> int;
    auto drainResampler() -> int;
    auto serveSpill(std::span<float> out, std::size_t totalDecoded, std::size_t frames)
        -> std::size_t;

    static auto readCallback(void* opaque, uint8_t* buf, int bufSize) -> int;

    // AVIO seek: AVSEEK_SIZE queries size, AVSEEK_FORCE stripped. The callback
    // is only installed when the reader reports seekability.
    static auto seekCallback(void* opaque, int64_t offset, int whence) -> int64_t;

    Reader* reader_{nullptr};
    AVFormatContext* fmt_{nullptr};
    AVCodecContext* dec_{nullptr};
    AVIOContext* avio_{nullptr};
    struct SwrContext* swr_{nullptr};
    int audioStreamIdx_{-1};
    uint32_t sampleRate_{0};
    uint32_t channels_{0};
    uint64_t totalFrames_{0};
    uint64_t pos_{0};
    PacketPtr scratchPkt_;
    FramePtr scratchFrame_;
    static constexpr std::size_t convertFrames = 4096;
    std::vector<float> convertBuf_;
    std::vector<float> spill_;
    std::size_t spillPos_{0};
};

auto extractMetadata(std::string_view path)
    -> kanariv::utils::Expected<kanariv::types::TrackMetadata> {
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::NoMem,
                                                         "avformat_alloc_context failed"));
    }

    if (avformat_open_input(&fmt, path.data(), nullptr, nullptr) < 0) {
        avformat_free_context(fmt);
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "avformat_open_input failed"));
    }

    // Bound probing (P1-1): defaults (5 MB / unbounded-ish analysis) let a
    // single open buffer megabytes per file. 1 MB / 0.5 s covers headers,
    // Xing/Info tables and attached pictures for sane files.
    fmt->probesize = probesizeDefault;
    fmt->max_analyze_duration = maxAnalyzeDurationUs;

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "avformat_find_stream_info failed"));
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
            if (!dict) {
                continue;
            }
            for (const char* key : keys) {
                AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
                if (entry && entry->value && *entry->value) {
                    return std::string(entry->value);
                }
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
        if (stream->codecpar != nullptr) {
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

auto Decoder::Impl::sampleRate() const noexcept -> uint32_t {
    return sampleRate_;
}

auto Decoder::Impl::channels() const noexcept -> uint32_t {
    return channels_;
}

auto Decoder::Impl::totalFrames() const noexcept -> uint64_t {
    return totalFrames_;
}

auto Decoder::Impl::decode(std::span<float> out) -> std::size_t {
    if (out.empty() || fmt_ == nullptr || dec_ == nullptr || swr_ == nullptr || channels_ == 0) {
        return 0;
    }

    std::size_t frames = out.size() / channels_;
    if (frames == 0) {
        return 0;
    }

    std::size_t totalDecoded = 0;
    bool eofReached = false;
    // Reusable scratch packet/frame (P-1). Lazily (re)allocated if init()
    // failed to pre-allocate or cleanup() released them.
    if (!scratchPkt_) {
        scratchPkt_.reset(av_packet_alloc());
    }
    if (!scratchFrame_) {
        scratchFrame_.reset(av_frame_alloc());
    }
    if (!scratchPkt_ || !scratchFrame_) {
        return 0;
    }
    AVPacket* pkt = scratchPkt_.get();
    AVFrame* frame = scratchFrame_.get();
    // Decode at most what `out` (plus the bounded spill_) can hold: each
    // iteration serves spill_ first and converts at most one more frame, so
    // swr_convert always sees the full convertFrames window (P-7).
    while (totalDecoded < frames && !eofReached) {
        totalDecoded += serveSpill(out, totalDecoded, frames);
        if (totalDecoded >= frames) {
            break;
        }
        av_packet_unref(pkt);

        int ret = av_read_frame(fmt_, pkt);
        if (ret < 0) {
            // EOF: flush decoder internal buffers
            if (ret == AVERROR_EOF || ret < 0) {
                (void)avcodec_send_packet(dec_, nullptr);
                for (;;) {
                    av_frame_unref(frame);
                    ret = avcodec_receive_frame(dec_, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    }
                    if (ret < 0) {
                        break;
                    }
                    if (convertFrameBuffered(frame) < 0) {
                        break;
                    }
                }
                if (drainResampler() < 0) {
                    break;
                }
                totalDecoded += serveSpill(out, totalDecoded, frames);
            }
            eofReached = true;
            break;
        }

        if (pkt->stream_index != audioStreamIdx_) {
            continue;
        }

        ret = avcodec_send_packet(dec_, pkt);
        if (ret == AVERROR(EAGAIN)) {
            av_frame_unref(frame);
            if (avcodec_receive_frame(dec_, frame) == 0) {
                (void)convertFrameBuffered(frame);
            }
            continue;
        }
        if (ret < 0) {
            continue;
        }

        av_frame_unref(frame);
        ret = avcodec_receive_frame(dec_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            continue;
        }
        if (ret < 0) {
            continue;
        }
        if (convertFrameBuffered(frame) < 0) {
            break;
        }
    }

    pos_ += totalDecoded;
    return totalDecoded;
}

auto Decoder::Impl::seek(double seconds) -> kanariv::utils::Expected<void> {
    if (seconds < 0.0 || !std::isfinite(seconds) || fmt_ == nullptr || dec_ == nullptr || audioStreamIdx_ < 0) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                     std::string_view("bad seconds")});
    }
    if (reader_ != nullptr && !reader_->seekable()) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Unsupported,
                                                     std::string_view("source is not seekable")});
    }
    AVStream* stream = fmt_->streams[audioStreamIdx_];
    if (stream == nullptr) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Internal,
                                                     std::string_view("no stream")});
    }
    // Use stream time_base for seeking - dec time_base is codec, not correct for container
    int64_t seekTarget = av_rescale_q(static_cast<int64_t>(seconds * AV_TIME_BASE),
                                      AVRational{.num = 1, .den = AV_TIME_BASE}, stream->time_base);
    // Adjust by stream start_time if present
    if (stream->start_time != AV_NOPTS_VALUE) {
        seekTarget += stream->start_time;
    } else if (fmt_->start_time != AV_NOPTS_VALUE) {
        // fmt start_time is in AV_TIME_BASE units, convert to stream units
        seekTarget += av_rescale_q(fmt_->start_time, AVRational{.num = 1, .den = AV_TIME_BASE},
                                   stream->time_base);
    }
    int ret = avformat_seek_file(fmt_, audioStreamIdx_, INT64_MIN, seekTarget, INT64_MAX, 0);
    if (ret < 0) {
        // Fallback to simple av_seek_frame
        ret = av_seek_frame(fmt_, audioStreamIdx_, seekTarget, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Io,
                                                         std::string_view("seek failed")});
        }
    }
    avcodec_flush_buffers(dec_);
    // flush resampler as well
    if (swr_ != nullptr) {
        swr_init(swr_);
    }
    // Drop converted-but-undelivered residue: positions before the seek
    // target must never surface after it.
    spill_.clear();
    spillPos_ = 0;
    pos_ = static_cast<uint64_t>(seconds * sampleRate_);
    return {};
}

Decoder::Impl::~Impl() {
    cleanup();
}

auto Decoder::Impl::convertFrameBuffered(AVFrame* frame) -> int {
    uint32_t ch = channels_;
    if (ch == 0 || frame == nullptr || frame->nb_samples <= 0) {
        return 0;
    }
    try {
        if (convertBuf_.size() < convertFrames * ch) {
            convertBuf_.resize(convertFrames * ch);
        }
        uint8_t* outPtrs[1] = {reinterpret_cast<uint8_t*>(convertBuf_.data())};
        int appended = 0;
        int n = swr_convert(swr_, outPtrs, static_cast<int>(convertFrames),
                            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        if (n < 0) {
            return n;
        }
        if (n > 0) {
            spill_.insert(spill_.end(), convertBuf_.data(),
                          convertBuf_.data() + static_cast<std::size_t>(n) * ch);
            appended += n;
        }
        appended += drainResampler();
        return appended;
    } catch (...) {
        return -1;
    }
}

auto Decoder::Impl::drainResampler() -> int {
    uint32_t ch = channels_;
    if (ch == 0) {
        return 0;
    }
    try {
        if (convertBuf_.size() < convertFrames * ch) {
            convertBuf_.resize(convertFrames * ch);
        }
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

auto Decoder::Impl::serveSpill(std::span<float> out, std::size_t totalDecoded, std::size_t frames)
    -> std::size_t {
    if (spillPos_ >= spill_.size() || channels_ == 0) {
        return 0;
    }
    std::size_t availFrames = (spill_.size() - spillPos_) / channels_;
    std::size_t want = frames - totalDecoded;
    std::size_t n = std::min(availFrames, want);
    if (n == 0) {
        return 0;
    }
    std::memcpy(out.data() + totalDecoded * channels_, spill_.data() + spillPos_,
                n * channels_ * sizeof(float));
    spillPos_ += n * channels_;
    if (spillPos_ == spill_.size()) {
        spill_.clear();
        spillPos_ = 0;
    }
    return n;
}

auto Decoder::Impl::readCallback(void* opaque, uint8_t* buf, int bufSize) -> int {
    auto* self = static_cast<Impl*>(opaque);
    if (self->reader_ == nullptr) {
        return AVERROR(EIO);
    }
    std::span<std::byte> dst(reinterpret_cast<std::byte*>(buf), bufSize);
    std::size_t n = self->reader_->read(dst);
    return n > 0 ? static_cast<int>(n) : AVERROR_EOF;
}

auto Decoder::Impl::seekCallback(void* opaque, int64_t offset, int whence) -> int64_t {
    auto* self = static_cast<Impl*>(opaque);
    if (self->reader_ == nullptr) {
        return AVERROR(EIO);
    }
    // Handle AVSEEK_SIZE (0x10000) -- query file size without seeking
    if (whence & AVSEEK_SIZE) { // NOLINT(readability-implicit-bool-conversion)
        int64_t sz = self->reader_->size();
        if (sz < 0) {
            return AVERROR(EIO);
        }
        return sz;
    }
    // Strip AVSEEK_FORCE flag (0x20000) if present
    int whenceMasked = whence & ~AVSEEK_FORCE;
    int w = SEEK_SET;
    if (whenceMasked == SEEK_CUR) {
        w = SEEK_CUR;
    } else if (whenceMasked == SEEK_END) {
        w = SEEK_END;
    } else if (whenceMasked == SEEK_SET) {
        w = SEEK_SET;
    } else {
        // Unknown whence after masking -- treat low 2 bits
        int low = whenceMasked & whenceMaskLowBits;
        if (low == SEEK_CUR) {
            w = SEEK_CUR;
        } else if (low == SEEK_END) {
            w = SEEK_END;
        } else {
            w = SEEK_SET;
        }
    }
    auto result = self->reader_->seek(offset, w);
    if (!result.has_value()) {
        return AVERROR(EIO);
    }
    return self->reader_->tell();
}

auto Decoder::Impl::init() -> bool {
    fmt_ = avformat_alloc_context();
    if (fmt_ == nullptr) {
        return false;
    }

    // Allocate AVIOContext with our callbacks -- use larger buffer for high-rate FLAC/WAV
    // probing
    constexpr size_t bufferSize = 8192;
    const bool canSeek = reader_ != nullptr && reader_->seekable();
    auto* avioBuffer = static_cast<uint8_t*>(av_malloc(bufferSize));
    if (avioBuffer == nullptr) {
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }

    avio_ = avio_alloc_context(avioBuffer, static_cast<int>(bufferSize), 0, this, &readCallback,
                               nullptr, canSeek ? &seekCallback : nullptr);
    if (avio_ == nullptr) {
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

    // Silence benign probe spam like "Header missing" / "CRC mismatch" during decode --
    // real errors still surfaced via return codes; use QUIET to eliminate spam that
    // masked restart bugs (Header missing x3, CRC mismatch). Keep silent after init.
    av_log_set_level(AV_LOG_QUIET);

    // Open input (probes format)
    if (avformat_open_input(&fmt_, nullptr, nullptr, nullptr) < 0) {
        cleanup();
        return false;
    }

    // Bound probing (P1-1): see extractMetadata above.
    fmt_->probesize = probesizeDefault;
    fmt_->max_analyze_duration = maxAnalyzeDurationUs;

    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        cleanup();
        return false;
    }

    // Find audio stream
    audioStreamIdx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIdx_ < 0) {
        cleanup();
        return false;
    }

    AVStream* stream = fmt_->streams[audioStreamIdx_];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        cleanup();
        return false;
    }

    dec_ = avcodec_alloc_context3(codec);
    if (dec_ == nullptr) {
        cleanup();
        return false;
    }

    if (avcodec_parameters_to_context(dec_, stream->codecpar) < 0) {
        cleanup();
        return false;
    }

    if (avcodec_open2(dec_, codec, nullptr) < 0) {
        cleanup();
        return false;
    }

    // Setup resampler to float interleaved -- preserve input channel layout exactly
    swr_ = swr_alloc();
    if (swr_ == nullptr) {
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

    if (swr_init(swr_) < 0) {
        cleanup();
        return false;
    }

    // Pre-allocate reusable decode scratch (P-1). Lazy fallback in decode()
    // covers the (near-impossible) alloc failure here.
    scratchPkt_.reset(av_packet_alloc());
    scratchFrame_.reset(av_frame_alloc());
    if (!scratchPkt_ || !scratchFrame_) {
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
        int64_t sz = reader_ != nullptr ? reader_->size() : -1;
        if (sz > 0 && stream->codecpar->bit_rate > 0) {
            // fallback for CBR MP3 with no duration
            double secs = (sz * bitsPerByte) / stream->codecpar->bit_rate;
            totalFrames_ = static_cast<uint64_t>(secs * sampleRate_);
        } else {
            totalFrames_ = 0;
        }
    } else {
        totalFrames_ = 0; // Unknown -- will still decode to EOF
    }

    pos_ = 0;
    return true;
}

void Decoder::Impl::cleanup() {
    scratchPkt_.reset();
    scratchFrame_.reset();
    convertBuf_.clear();
    spill_.clear();
    spillPos_ = 0;
    if (swr_ != nullptr) {
        swr_free(&swr_);
        swr_ = nullptr;
    }
    if (dec_ != nullptr) {
        avcodec_free_context(&dec_);
        dec_ = nullptr;
    }
    if (fmt_ != nullptr) {
        // Prevent avformat_close_input from freeing our custom pb.
        // With AVFMT_FLAG_CUSTOM_IO, avformat_close_input does NOT free pb.
        fmt_->pb = nullptr;
        avformat_close_input(&fmt_);
        fmt_ = nullptr;
    }
    if (avio_ != nullptr) {
        // avio_context_free also frees the buffer allocated via av_malloc
        avio_context_free(&avio_);
        avio_ = nullptr;
    }
}

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}

Decoder::~Decoder() = default;

Decoder::Decoder(Decoder&&) noexcept = default;

auto Decoder::operator=(Decoder&&) noexcept -> Decoder& = default;

auto Decoder::init(Reader& reader) -> kanariv::utils::Expected<void> {
    impl_->reader_ = &reader;
    if (!impl_->init()) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Unsupported,
                                                     std::string_view("FFmpeg init failed")});
    }
    return {};
}

auto Decoder::sampleRate() const noexcept -> uint32_t {
    return impl_->sampleRate();
}

auto Decoder::channels() const noexcept -> uint32_t {
    return impl_->channels();
}

auto Decoder::totalFrames() const noexcept -> uint64_t {
    return impl_->totalFrames();
}

auto Decoder::decode(std::span<float> out) -> std::size_t {
    return impl_->decode(out);
}

auto Decoder::seek(double seconds) -> kanariv::utils::Expected<void> {
    return impl_->seek(seconds);
}

} // namespace kanariv::media
