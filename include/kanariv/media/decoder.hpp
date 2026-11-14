#pragma once

/**
 * @file decoder.hpp
 * @brief Single audio decoder (FFmpeg backend) with format probing.
 * @ingroup kanariv_media
 *
 * This header provides the Decoder class, the sole audio decoder, implemented
 * with FFmpeg for all common audio formats including OGG/Vorbis, FLAC, MP3,
 * WAV, M4A/AAC, Opus, and WMA. FFmpeg types stay in the private implementation;
 * this header exposes only standard C++ types.
 *
 * Decoding follows a probe-based workflow matching the C reference
 * implementation (ca_decode.c):
 * 1. For seekable readers: save the current position, seek to start (offset
 *    0), read 32 bytes, restore the position, and probe the header bytes.
 * 2. Seek back to 0 and open the decoder (FFmpeg AVIO requires start at 0).
 * 3. For non-seekable readers the read-ahead is skipped entirely: the probe
 *    bytes could not be restored, so consuming them would start playback
 *    mid-stream. FFmpeg's own format probe reads from offset 0 instead.
 * 4. On open failure the original position is restored.
 *
 * @par Thread safety
 * A Decoder instance is not thread-safe for concurrent calls.
 * Callers must externally serialize decode() and seek(); the Player keeps
 * decode() on its decode thread and coordinates control-thread seeks.
 * sampleRate(), channels() and totalFrames() are thread-safe (const noexcept).
 *
 * @par Moved-from state
 * A moved-from Decoder is only destructible or assignable, but its accessors
 * are null-safe: sampleRate()/channels()/totalFrames() return 0, decode()
 * returns 0 and reports DecodeStatus::Error, and seek() reports
 * StatusCode::Internal. A moved-from decoder is never reopened.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace kanariv::media {

/**
 * @enum DecodeStatus
 * @brief Outcome of the most recent Decoder::decode() call.
 * @ingroup kanariv_media
 *
 * decode() returns a frame count for historical and hot-path reasons, so the
 * count alone cannot distinguish "the track ended" from "the source failed".
 * The optional status out-parameter carries that distinction without an
 * allocation per block.
 */
enum class DecodeStatus : std::uint8_t {
    Ok,   ///< Frames were decoded; more audio may be pending.
    Eof,  ///< End of stream reached (possibly after decoding tail frames).
    Error ///< I/O or codec failure; frames decoded before it remain valid.
};

/**
 * @class Decoder
 * @brief Single audio format decoder (FFmpeg backend).
 * @ingroup kanariv_media
 *
 * Created via open(), which probes the reader and initializes FFmpeg.
 * All operations after open() run against the initialized stream.
 *
 * Error Codes (via Expected<void>):
 * - InvalidArg: Invalid seek time (negative, NaN, infinity)
 * - Io: Decoder-level I/O error during seek
 * - Internal: Decoder not properly initialized
 */
class KANARIV_API Decoder final {
  public:
    /**
     * @brief Probe raw header bytes for a supported format.
     * @param data Header bytes (reads up to 32 from the reader start).
     * @return true if a decode attempt should proceed.
     */
    [[nodiscard]] static bool probe(std::span<const std::byte> data) noexcept;

    /**
     * @brief Probe and open a decoder for the given reader.
     * @param reader Input reader positioned at start of audio data.
     * @param logger Non-owning logger (may be null = silent). Must outlive
     * the open call; stored for decode-path messages, never from destructors.
     * @return Expected containing unique_ptr to Decoder on success, Error on failure.
     * @retval StatusCode::Unsupported No decoder recognized the format, or init failed
     * @retval StatusCode::Io FFmpeg initialization failed
     * @retval StatusCode::InvalidArg Reader seek/tell returned invalid values
     */
    [[nodiscard]] static kanariv::utils::Expected<std::unique_ptr<Decoder>>
    open(Reader& reader, kanariv::utils::Logger* logger = nullptr);

    /** @brief Default-constructs an unopened decoder; use open() factory. */
    Decoder();
    /** @brief Tears down the FFmpeg backend. */
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    /** @brief Move-constructs, transferring the backend. */
    Decoder(Decoder&&) noexcept;
    /** @brief Move-assigns, transferring the backend. */
    Decoder& operator=(Decoder&&) noexcept;

    /**
     * @brief Get the sample rate of the decoded audio in Hz.
     * @return Native sample rate of the audio stream, set at open.
     */
    [[nodiscard]] uint32_t sampleRate() const noexcept;

    /**
     * @brief Get the number of audio channels (1=mono, 2=stereo).
     * @return Channel count; output is always interleaved float samples.
     */
    [[nodiscard]] uint32_t channels() const noexcept;

    /**
     * @brief Get total frame count, or 0 if unknown (e.g. live streams).
     * @return Total frames (samples per channel).
     */
    [[nodiscard]] uint64_t totalFrames() const noexcept;

    /**
     * @brief Decode audio samples into the provided buffer.
     * @param out Output buffer for interleaved float samples.
     * @return Number of frames decoded (0 = EOF or error).
     * @details Decodes up to out.size() / channels() frames. Must be called
     * from the decode thread only; not safe concurrently with seek().
     * A pending input packet is never discarded: when FFmpeg reports
     * AVERROR(EAGAIN) the packet is retained and retried on the next
     * iteration, so no sample is lost and no packet is sent twice.
     * Use the overload below when EOF must be told apart from failure.
     */
    [[nodiscard]] std::size_t decode(std::span<float> out);

    /**
     * @brief Decode audio samples and report why decoding stopped.
     * @param out Output buffer for interleaved float samples.
     * @param status Receives Ok, Eof, or Error; must not be null.
     * @return Number of frames decoded (0 = EOF or error; see @p status).
     * @details Same contract as the single-argument overload. Kept as a
     * separate overload (not a defaulted parameter) so the original exported
     * symbol and its ABI stay unchanged.
     */
    [[nodiscard]] std::size_t decode(std::span<float> out, DecodeStatus* status);

    /**
     * @brief Seek to a time position in seconds from stream start.
     * @param seconds Target time (must be >= 0, finite, not NaN).
     * @return void on success, Error on failure.
     */
    [[nodiscard]] kanariv::utils::Expected<void> seek(double seconds);

  private:
    /// Opaque FFmpeg backend; defined in the private implementation.
    struct Impl;
    std::unique_ptr<Impl> impl_;
    /// Non-owning log sink (null = silent). See open().
    kanariv::utils::Logger* logger_{nullptr};

    /**
     * @brief Initialize the backend against an opened reader.
     * @param reader Input reader (repositioned to 0 by open()).
     * @return void on success, Error with StatusCode::Unsupported on init failure.
     */
    kanariv::utils::Expected<void> init(Reader& reader);
};

/**
 * @brief Extracts metadata tags from an audio file.
 * @ingroup kanariv_media
 * @param path Filesystem path to probe.
 * @return Metadata on success, Error on failure.
 */
KANARIV_API kanariv::utils::Expected<kanariv::types::TrackMetadata> extractMetadata(std::string_view path);

} // namespace kanariv::media
