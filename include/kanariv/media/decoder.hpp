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
 * 1. Save current reader position
 * 2. Seek to start (offset 0) and read 32 bytes
 * 3. Restore original position BEFORE probing
 * 4. Probe the header data (any input of 4+ bytes is attempted; FFmpeg
 *    initialization fails gracefully for unsupported data)
 * 5. On probe success, seek to 0 and open the decoder (FFmpeg AVIO requires
 *    start at 0)
 * 6. On open failure, restore the original position
 *
 * @par Thread safety
 * A Decoder instance is not thread-safe for concurrent calls.
 * Callers must externally serialize decode() and seek(); the Player keeps
 * decode() on its decode thread and coordinates control-thread seeks.
 * sampleRate(), channels() and totalFrames() are thread-safe (const noexcept).
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
     */
    std::size_t decode(std::span<float> out);

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
