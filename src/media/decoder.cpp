#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace kanariv::media {

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

} // namespace kanariv::media
