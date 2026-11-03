#pragma once

/**
 * @file reader.hpp
 * @brief Input reader abstractions for audio data sources
 * @ingroup kanariv_media
 *
 * This header provides the Reader abstract interface and concrete implementations
 * for reading audio data from various sources. It mirrors the C ca_reader.c
 * interface and provides:
 * - Reader: Abstract base class for all input sources
 * - FileReader: File-based reading with 64-bit seek support
 * - MemoryReader: In-memory buffer reading
 * - DescriptorReader: Native file-descriptor reading with explicit ownership
 *
 * @par Thread safety
 * - Reader interface: Thread-compatible (single-threaded use expected)
 * - FileReader: Thread-safe for concurrent read/seek/tell (internal mutex)
 * - MemoryReader: Not thread-safe (no internal synchronization)
 * - DescriptorReader: Thread-safe for concurrent read/seek/tell (internal mutex)
 *
 * Error Codes (via Expected<void>):
 * - InvalidArg: Null file, bad whence, seek out of range, empty path, bad descriptor
 * - NotFound: File not found or cannot be opened
 * - Unsupported: Seek requested on a non-seekable descriptor
 * - Internal: fseek/ftell/read failure, size not cached
 * - NoMem: Allocation failure
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <kanariv/fwd.hpp>
#include <memory>
#include <span>
#include <vector>

namespace kanariv::media {

/**
 * @struct FileDescriptor
 * @brief Native descriptor plus explicit ownership and seekability metadata
 * @ingroup kanariv_media
 *
 * This is a plain integer descriptor (POSIX `int` or Windows CRT `int`), not
 * an Android `ParcelFileDescriptor` or any other platform object. It exists so
 * hosts can hand an already-open file to the media layer without inventing a
 * filesystem path for content URIs.
 *
 * @par Ownership
 * - When `closeOnDestroy` is true, the reader created from this descriptor
 *   closes it exactly once.
 * - When false, the caller retains ownership and must keep the descriptor open
 *   for the reader's lifetime; the media layer never closes a borrowed
 *   descriptor.
 *
 * Seekability is explicit. Set `seekable` only when the host knows the
 * descriptor supports `lseek`/`_lseeki64`; callers must not infer it from the
 * descriptor number or the containing object.
 *
 * On Windows, the descriptor must be opened in binary mode by the caller.
 */
struct FileDescriptor {
    int value{-1};              ///< Native descriptor/handle; negative = invalid.
    bool readable{true};        ///< Caller asserts the descriptor is readable.
    bool seekable{false};       ///< Caller asserts seek() is supported.
    bool closeOnDestroy{false}; ///< Transfer ownership to the created reader.
};

/**
 * @class Reader
 * @brief Abstract base class for audio data input sources
 * @ingroup kanariv_media
 *
 * Defines the interface for reading audio data. All concrete readers
 * (FileReader, MemoryReader, etc.) must implement this interface.
 * Mirrors the C ca_reader.c virtual table.
 *
 * @par Thread safety
 * Thread-compatible. The Player uses a single reader
 * from the decode thread. Concurrent access requires external synchronization
 * (except FileReader which has internal mutex for seek/tell).
 *
 * NOTE: The `data()` method from C ca_reader.c was removed to avoid
 * GCC 14.2 module ICE. Decoders needing direct memory access (e.g., Vorbis)
 * should use MemoryReader which provides contiguous memory access.
 */
class KANARIV_API Reader {
  public:
    virtual ~Reader() = default;

    /**
     * @brief Read data into the provided buffer
     * @param dst Destination buffer
     * @return Number of bytes read (0 = EOF or error)
     *
     * Reads up to dst.size() bytes from the current position.
     * Advances the internal position by the number of bytes read.
     * Returns 0 on EOF or if the source is invalid.
     */
    virtual std::size_t read(std::span<std::byte> dst) = 0;

    /**
     * @brief Seek to a new position
     * @param offset Byte offset from whence
     * @param whence SEEK_SET, SEEK_CUR, or SEEK_END
     * @return void on success, Error on failure
     *
     * Sets the read position relative to whence. Validates that the
     * resulting position is within [0, size()].
     *
     * @retval StatusCode::InvalidArg whence invalid or seek out of range
     * @retval StatusCode::Internal fseek/ftell failed
     */
    [[nodiscard]] virtual kanariv::utils::Expected<void> seek(int64_t offset, int whence) = 0;

    /**
     * @brief Get current read position
     * @return Current byte offset from start, or -1 on error
     */
    [[nodiscard]] virtual int64_t tell() noexcept = 0;

    /**
     * @brief Get total size of the data source
     * @return Total size in bytes, or -1 if unknown
     */
    [[nodiscard]] virtual int64_t size() noexcept = 0;

    /**
     * @brief Report whether seek() is supported by this source
     * @return true when seek() may succeed; false for streaming sources
     *
     * The default is true for the existing file and memory readers. A
     * descriptor reader reports the caller-supplied `FileDescriptor::seekable`
     * flag so FFmpeg and other consumers can avoid seek-dependent operations
     * on streams. `seek()` on a non-seekable reader returns
     * `StatusCode::Unsupported` instead of silently changing position.
     */
    [[nodiscard]] virtual bool seekable() const noexcept {
        return true;
    }
};

/**
 * @class FileReader
 * @brief File-based reader with 64-bit seek support
 * @ingroup kanariv_media
 *
 * Reads audio data from a file on disk. Uses platform-specific 64-bit
 * file APIs (_ftelli64/_fseeki64 on Windows, ftello/fseeko on POSIX)
 * to support files larger than 2GB.
 *
 * @par Thread safety
 * Thread-safe for concurrent read/seek/tell operations
 * via internal mutex. The FILE* is owned exclusively by this instance.
 *
 * Error Handling:
 * - open(): Returns Error on file not found, permission denied, or seek failure
 * - read(): Returns 0 on EOF or read error
 * - seek(): Returns Error on invalid whence, out of range, or fseek failure
 * - tell(): Returns -1 on error
 * - size(): Returns cached size or -1
 */
class KANARIV_API FileReader final : public Reader {
  public:
    FileReader() = delete;

    /**
     * @brief Open a file for reading
     * @param path Path to the audio file
     * @return Expected containing unique_ptr<Reader> on success, Error on failure
     *
     * Opens the file in binary mode, seeks to end to determine size,
     * then seeks back to start. On Windows, tries wide-char path first
     * then falls back to narrow string.
     *
     * @retval StatusCode::InvalidArg Empty path
     * @retval StatusCode::NotFound Cannot open file
     * @retval StatusCode::Internal fseek/ftell failed during size detection
     *
     * @par Thread safety
     * Thread-safe; construction uses no shared state.
     */
    static kanariv::utils::Expected<std::unique_ptr<Reader>>
    open(const std::filesystem::path& path);

    ~FileReader() override;

    /** @brief Reads bytes from the owned file; returns zero at EOF or on read error. */
    std::size_t read(std::span<std::byte> dst) override;

    /** @brief Moves the file position, using the platform's 64-bit seek API. */
    [[nodiscard]] kanariv::utils::Expected<void> seek(int64_t offset, int whence) override;

    /** @brief Returns the current file position, or -1 on error. */
    [[nodiscard]] int64_t tell() noexcept override;

    /** @brief Returns the cached file size, or -1 if unavailable. */
    [[nodiscard]] int64_t size() noexcept override;

    /** @brief Files are always seekable. */
    [[nodiscard]] bool seekable() const noexcept override;

  private:
    struct FileToken {};
    explicit FileReader(FileToken token);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @class DescriptorReader
 * @brief Reader backed by a native file descriptor with explicit ownership
 * @ingroup kanariv_media
 *
 * DescriptorReader adapts an already-open descriptor to the Reader interface
 * using 64-bit seek operations (`_lseeki64` on Windows, `lseek` on POSIX) and
 * the platform read primitive (`_read`/`read`). It is the media-layer bridge
 * for Android Storage Access Framework descriptors and any other host that can
 * open a file but cannot expose a stable filesystem path.
 *
 * Ownership follows FileDescriptor::closeOnDestroy: the reader closes an owned
 * descriptor in its destructor and never closes a borrowed one.
 *
 * @par Thread safety
 * Thread-safe for concurrent read/seek/tell via an internal
 * mutex. The descriptor's position is serialized by the reader.
 *
 * Error Handling:
 * - open(): InvalidArg for a negative or unreadable descriptor; Unsupported
 *   when the descriptor claims seekability but the platform seek probe fails.
 * - read(): Returns 0 at EOF or on read error; interrupted reads are retried.
 * - seek(): Unsupported for a non-seekable descriptor; InvalidArg for a bad
 *   whence or out-of-range target; Io for a failed platform seek.
 * - tell(): Returns the tracked position, or -1 after a failed seek.
 * - size(): Returns the cached size for seekable descriptors, or -1.
 */
class KANARIV_API DescriptorReader final : public Reader {
  public:
    DescriptorReader() = delete;

    /**
     * @brief Create a reader from a native descriptor
     * @param descriptor Descriptor and ownership/seekability metadata
     * @return Expected containing unique_ptr<Reader> on success, Error on failure
     *
     * On success, ownership of an owned descriptor has transferred to the
     * returned reader. On failure the descriptor remains owned by the caller.
     *
     * @retval StatusCode::InvalidArg Descriptor is negative or not readable
     * @retval StatusCode::Unsupported The descriptor is marked seekable but
     *         the platform cannot determine its position
     */
    static kanariv::utils::Expected<std::unique_ptr<Reader>> open(FileDescriptor descriptor);

    ~DescriptorReader() override;

    /** @brief Reads bytes and advances the tracked position; 0 at EOF/error. */
    std::size_t read(std::span<std::byte> dst) override;

    /** @brief Moves the descriptor position when the source is seekable. */
    [[nodiscard]] kanariv::utils::Expected<void> seek(int64_t offset, int whence) override;

    /** @brief Returns the tracked descriptor position, or -1 on error. */
    [[nodiscard]] int64_t tell() noexcept override;

    /** @brief Returns the cached descriptor size, or -1 when unknown. */
    [[nodiscard]] int64_t size() noexcept override;

    /** @brief Returns the caller-supplied seekability flag. */
    [[nodiscard]] bool seekable() const noexcept override;

  private:
    struct DescriptorToken {};
    explicit DescriptorReader(DescriptorToken token, FileDescriptor descriptor);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @class MemoryReader
 * @brief In-memory buffer reader
 * @ingroup kanariv_media
 *
 * Reads audio data from an in-memory buffer. Provides fast, seekable
 * access without file I/O. Copies input data to an owned buffer for
 * lifetime safety.
 *
 * @par Thread safety
 * Not thread-safe. No internal synchronization. Intended
 * for single-threaded use (e.g., decode thread).
 *
 * Use Cases:
 * - Embedded audio data
 * - Network streams buffered in memory
 * - Decoders requiring direct memory access (Vorbis, etc.)
 */
class KANARIV_API MemoryReader final : public Reader {
  public:
    MemoryReader() = delete;

    /**
     * @brief Open a memory reader from a byte span
     * @param data Input data (copied to owned buffer)
     * @return Expected containing unique_ptr<Reader> on success
     *
     * Copies the input data to an internal buffer. The original data
     * can be freed after this call returns.
     */
    static kanariv::utils::Expected<std::unique_ptr<Reader>> open(std::span<const std::byte> data);

    /**
     * @brief Open a memory reader from a vector
     * @param data Input vector (copied)
     * @return Expected containing unique_ptr<Reader> on success
     */
    static kanariv::utils::Expected<std::unique_ptr<Reader>>
    open(const std::vector<std::byte>& data);

    /**
     * @brief Open a memory reader from raw pointer and length
     * @param data Pointer to data
     * @param len Length in bytes
     * @return Expected containing unique_ptr<Reader> on success, Error if null data with len>0
     */
    static kanariv::utils::Expected<std::unique_ptr<Reader>> open(const std::byte* data,
                                                                  std::size_t len);

    /** @brief Reads bytes from the current in-memory position. */
    std::size_t read(std::span<std::byte> dst) override;

    /** @brief Moves the in-memory position within the buffer. */
    [[nodiscard]] kanariv::utils::Expected<void> seek(int64_t offset, int whence) override;

    /** @brief Returns the current in-memory position. */
    [[nodiscard]] int64_t tell() noexcept override;
    /** @brief Returns the size of the owned buffer. */
    [[nodiscard]] int64_t size() noexcept override;
    /** @brief Memory buffers are always seekable. */
    [[nodiscard]] bool seekable() const noexcept override;

  private:
    explicit MemoryReader(std::span<const std::byte> src);
    std::vector<std::byte> buf_;
    std::size_t pos_{0};
};

} // namespace kanariv::media
