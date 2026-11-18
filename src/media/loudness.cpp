/**
 * @file loudness.cpp
 * @brief FFmpeg decode + EBU R128 measurement of a media file.
 * @ingroup kanariv_media
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/dsp/loudness.hpp>
#include <kanariv/media/loudness.hpp>
#include <kanariv/utils/error.hpp>
#include <media/convert_loop.hpp>
#include <media/ffmpeg_error.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace kanariv::media {

kanariv::utils::Expected<LoudnessAnalysis> analyzeLoudness(std::string_view path) {
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
                    std::string message) -> std::expected<LoudnessAnalysis, kanariv::utils::Error> {
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
    // Decoder::Impl), so the meter sees one consistent sample format.
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

    auto meterRes = kanariv::dsp::loudness::LoudnessMeter::create(static_cast<double>(sr), ch);
    if (!meterRes) {
        return fail(kanariv::utils::StatusCode::Unsupported,
                    "loudness meter: " + meterRes.error().message);
    }
    auto meter = std::move(*meterRes);

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame)
        return fail(kanariv::utils::StatusCode::NoMem, "av_packet/av_frame alloc failed");

    uint64_t totalSamples = 0;
    bool meterFailed = false;

    constexpr std::size_t convertFrames = 4096;
    std::vector<float> convertBuf(convertFrames * ch);
    auto consume = [&](std::span<const float> block) {
        // The meter was created with this stream's channel count, so a size
        // error would mean the frame assembly is broken; report, do not ignore.
        if (!meter->addFrames(block))
            meterFailed = true;
        totalSamples += block.size();
    };

    // Converts one decoded frame through swr and feeds every converted sample
    // to the meter; `source == nullptr` flushes the resampler instead. The pump
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
    // filter-delay tail are part of the measurement, not padding.
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
    if (meterFailed) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         "loudness meter rejected decoded frames")};
    }

    const auto measured = meter->result();
    LoudnessAnalysis analysis;
    analysis.integratedLufs = measured.integratedLufs;
    analysis.truePeakDbtp = measured.truePeakDbtp;
    analysis.gainDb = kanariv::dsp::loudness::replayGainDb(measured);
    const auto energies = meter->blockEnergies();
    analysis.blockEnergies.assign(energies.begin(), energies.end());
    return analysis;
}

} // namespace kanariv::media
