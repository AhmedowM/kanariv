#pragma once

/**
 * @file scan_walker.hpp
 * @brief Private resumable directory walk shared by every scan entry point.
 * @ingroup kanariv_media
 * @details Internal header: lives in `src/`, not installed. `ScanWalker` owns
 * the filesystem iteration, so `scanTracks` (lazy generator) and
 * `scanTracksToVector` (eager) cannot disagree about which entries are
 * skipped, which issues are raised, or when a walk error truncates the result.
 */

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <kanariv/fwd.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <optional>

namespace kanariv::media::internal {

/**
 * @brief Digest source for one file.
 * @ingroup kanariv_media
 * @details The default hashes with BLAKE3 (`computeFingerprint` for
 * `ScanMode::Sampled`, the full-file hash for `ScanMode::Full`). Tests inject a
 * failing or zero-returning provider here to exercise the skip path without
 * depending on filesystem permission semantics, which differ per platform and
 * are bypassed when the tests run as root.
 */
using FingerprintProvider =
    std::function<std::expected<std::array<std::uint8_t, 32>, kanariv::utils::Error>(
        const std::filesystem::path&, ScanMode)>;

/**
 * @brief Resumable audio-file walk behind every scan entry point.
 * @ingroup kanariv_media
 * @details Single-pass, move-only and not thread-safe. `next()` skips the files
 * it cannot turn into a usable track and reports each skip through the issue
 * sink; the walk ends on the first filesystem error, which is recorded so
 * callers that promise a complete library can fail instead of returning a
 * partial one.
 * @par Thread safety
 * Not thread-safe; one walker per thread.
 */
class KANARIV_API ScanWalker {
  public:
    /**
     * @brief Prepares a walk; nothing is read until `open()` / `next()`.
     * @param root Directory to traverse recursively.
     * @param mode Fingerprinting mode.
     * @param onIssue Optional issue sink, called as problems are found.
     * @param fingerprint Optional digest provider; empty selects the real
     * BLAKE3 provider for `mode` (override only from tests).
     */
    ScanWalker(std::filesystem::path root, ScanMode mode, ScanIssueSink onIssue = {},
               FingerprintProvider fingerprint = {});
    ~ScanWalker();
    ScanWalker(const ScanWalker&) = delete;
    ScanWalker& operator=(const ScanWalker&) = delete;
    ScanWalker(ScanWalker&&) = delete;
    ScanWalker& operator=(ScanWalker&&) = delete;

    /**
     * @brief Opens the directory walk.
     * @return Success, `NotFound` when the root does not exist, or `Io` when it
     * cannot be opened (permission denied, path is a regular file, ...). A
     * failure is also reported as a `WalkError` issue.
     */
    std::expected<void, kanariv::utils::Error> open();

    /**
     * @brief Returns the next fully fingerprinted track.
     * @return A track, or `std::nullopt` once the walk is exhausted *or* was
     * truncated by an error -- check `walkError()` to tell the two apart.
     */
    std::optional<kanariv::types::Track> next();

    /**
     * @brief Error that ended the walk early, if any.
     * @return The error, or `std::nullopt` for a complete walk. A truncated
     * walk means the scanned library is partial.
     */
    [[nodiscard]] const std::optional<kanariv::utils::Error>& walkError() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::media::internal
