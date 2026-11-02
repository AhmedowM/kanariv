#pragma once

/**
 * @file source.hpp
 * @brief Media source abstractions: paths and native file descriptors.
 * @ingroup kanariv_media
 *
 * A media source separates *where bytes come from* from the reader that
 * consumes them. The path source preserves existing desktop behavior; the
 * descriptor source lets a host (for example an Android foreground service)
 * supply an already-open native descriptor for content that has no real
 * filesystem path.
 *
 * Design rules:
 * - No Android, JNI, or content-URI types appear here. A descriptor is an
 *   ordinary native `int` supplied through `FileDescriptor`.
 * - Descriptor ownership and seekability are explicit; see FileDescriptor.
 * - A descriptor source is single-use: it transfers its descriptor to the
 *   reader returned by open(). The host reopens the source for every playback
 *   operation instead of persisting a transient descriptor.
 * - Source identity persistence lives in `kanariv::types::SourceRef`; this
 *   header only describes how to obtain a reader now.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/media/reader.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace kanariv::media {

/**
 * @enum SourceKind
 * @brief Transport mechanism used to read a media source.
 * @ingroup kanariv_media
 *
 * This is deliberately separate from `kanariv::types::SourceKind`, which
 * describes the durable identity recorded in the database (ordinary path,
 * host-issued external id, imported copy, or unavailable source). A durable
 * external identity is reopened by the host and handed over as a
 * `DescriptorSource`; the media layer never guesses a path for it.
 */
enum class SourceKind : std::uint8_t {
    path,      ///< Ordinary filesystem path.
    descriptor ///< Caller-supplied native descriptor.
};

/**
 * @class Source
 * @brief Abstract factory for readers over a media source.
 * @ingroup kanariv_media
 *
 * Implementations are expected to be cheap value-like objects. `open()` is
 * non-const because descriptor-backed sources transfer ownership on success.
 *
 * @par Thread safety
 * Implementations are not required to be thread-safe. A host
 * should create one source per playback operation.
 */
class KANARIV_API Source {
  public:
    virtual ~Source() = default;

    /** @brief Returns the transport mechanism used by this source. */
    [[nodiscard]] virtual SourceKind kind() const noexcept = 0;

    /** @brief Returns a user-facing name that may be shown in diagnostics. */
    [[nodiscard]] virtual std::string_view displayName() const noexcept = 0;

    /**
     * @brief Reports whether readers from this source support seeking.
     * @details For descriptor sources this is the caller-supplied flag, not an
     * inferred property of the descriptor.
     */
    [[nodiscard]] virtual bool seekable() const noexcept = 0;

    /**
     * @brief Opens a reader for this source.
     * @return Reader on success, or an Error from the underlying reader.
     *
     * Path sources may be opened repeatedly. Descriptor sources are single-use
     * because ownership moves to the returned reader on success.
     */
    [[nodiscard]] virtual kanariv::utils::Expected<std::unique_ptr<Reader>> open() = 0;
};

/**
 * @class PathSource
 * @brief Source backed by an ordinary filesystem path.
 * @ingroup kanariv_media
 *
 * This is the existing desktop behavior expressed through the Source
 * interface. It remains seekable and may be opened any number of times.
 */
class KANARIV_API PathSource final : public Source {
  public:
    /**
     * @brief Constructs a path source.
     * @param path Filesystem path. An empty path is rejected by open().
     */
    explicit PathSource(std::filesystem::path path);

    /** @brief Returns SourceKind::path. */
    [[nodiscard]] SourceKind kind() const noexcept override;

    /** @brief Returns the path's generic/native string for diagnostics. */
    [[nodiscard]] std::string_view displayName() const noexcept override;

    /** @brief Path sources are always seekable. */
    [[nodiscard]] bool seekable() const noexcept override;

    /** @brief Opens the path with FileReader. */
    [[nodiscard]] kanariv::utils::Expected<std::unique_ptr<Reader>> open() override;

    /** @brief Returns the stored filesystem path. */
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

  private:
    std::filesystem::path path_;
    std::string displayName_;
};

/**
 * @class DescriptorSource
 * @brief Source backed by a caller-supplied native file descriptor.
 * @ingroup kanariv_media
 *
 * The first successful open() transfers ownership of the descriptor to a
 * DescriptorReader and makes subsequent open() calls fail with
 * `StatusCode::State`. This prevents accidental double-close and makes the
 * per-operation lifetime explicit: persist a stable host identifier, not the
 * descriptor.
 */
class KANARIV_API DescriptorSource final : public Source {
  public:
    DescriptorSource() = delete;

    /**
     * @brief Constructs a descriptor source.
     * @param descriptor Descriptor with ownership and seekability metadata.
     * @param displayName Optional diagnostic name. Defaults to
     *        `descriptor:<value>`.
     */
    explicit DescriptorSource(FileDescriptor descriptor, std::string displayName = {});

    DescriptorSource(const DescriptorSource&) = delete;
    DescriptorSource& operator=(const DescriptorSource&) = delete;
    DescriptorSource(DescriptorSource&& other) noexcept;
    DescriptorSource& operator=(DescriptorSource&& other) noexcept;

    /**
     * @brief Releases an owned descriptor that was never handed to a reader.
     * @details Borrowed descriptors (`closeOnDestroy == false`) are never
     * closed by this destructor.
     */
    ~DescriptorSource() override;

    /** @brief Returns SourceKind::descriptor. */
    [[nodiscard]] SourceKind kind() const noexcept override;

    /** @brief Returns the supplied display name. */
    [[nodiscard]] std::string_view displayName() const noexcept override;

    /** @brief Returns the descriptor's declared seekability. */
    [[nodiscard]] bool seekable() const noexcept override;

    /**
     * @brief Transfers the descriptor to a DescriptorReader.
     * @retval StatusCode::State The source has already been opened.
     * @retval StatusCode::InvalidArg The descriptor is invalid/not readable.
     */
    [[nodiscard]] kanariv::utils::Expected<std::unique_ptr<Reader>> open() override;

    /** @brief Reports whether open() has already succeeded. */
    [[nodiscard]] bool consumed() const noexcept;

  private:
    FileDescriptor descriptor_;
    std::string displayName_;
    bool consumed_{false};
};

} // namespace kanariv::media
