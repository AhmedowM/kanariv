#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>

/**
 * @file fingerprint.hpp
 * @brief Content fingerprinting (BLAKE3 sampled vs full).
 * @ingroup kanariv_media
 * @details Fingerprints are 32-byte BLAKE3 digests used as the primary
 * deduplication key (`tracks.fingerprint` UNIQUE). Two modes:
 * - Sampled (default, used by scan): `BLAKE3(head 64 KiB || tail 64 KiB || LE64(size) ||
 * LE32(version=1))`. Fast for large files; stable across metadata-only changes because only audio
 * bytes are hashed (but file-size is mixed in to avoid collisions on truncated files).
 * - Full (used when `kanariv::media::ScanMode::Full`): hashes the entire file sequentially.
 * See `kanariv/media/scan.hpp` for the full-file path.
 */

namespace kanariv::media::internal {

/**
 * @brief The all-zero digest that must never be persisted.
 * @ingroup kanariv_media
 * @details `TrackRow::fingerprint` is value-initialised, so a scan that failed to
 * hash a file used to yield 32 zero bytes. `tracks.fingerprint` is
 * `NOT NULL UNIQUE`, so that sentinel makes the second unreadable file collide
 * or overwrite the first. Scan discovery refuses to emit it and the DB scan
 * refuses to persist it; both use this predicate so the sentinel is defined in
 * exactly one place.
 */
inline constexpr std::array<uint8_t, 32> zeroFingerprint{};

/**
 * @brief True when `fp` is the unusable all-zero sentinel.
 * @ingroup kanariv_media
 * @param fp 32-byte digest to test.
 * @return true when every byte is zero.
 * @see zeroFingerprint
 */
inline bool isZeroFingerprint(const std::array<uint8_t, 32>& fp) noexcept {
    return fp == zeroFingerprint;
}

/**
 * @brief Number of bytes sampled from head and tail.
 * @ingroup kanariv_media
 * @details 64 KiB each. Files <= 64 KiB hash only once (head == whole file plus size/version
 * trailer).
 */
inline constexpr size_t sample = 64uz * 1024uz;

/**
 * @brief Computes a sampled BLAKE3 fingerprint for a file.
 * @ingroup kanariv_media
 * @param path Filesystem path to hash.
 * @return 32-byte digest on success, or `Error` with `StatusCode::Io` if the file
 * cannot be sized or opened.
 * @details Algorithm: `BLAKE3( head[0..64K) || tail[size-64K..size) || LE64(size) ||
 * LE32(version=1) )`. Uses a thread-local 64 KiB buffer to avoid per-call allocation. The version
 * field allows future fingerprint upgrades without silent collisions.
 * @par Thread safety
 * Thread-safe: uses `thread_local` buffer; no shared state. File I/O is blocking.
 * @see sample
 * @note KANARIV_API: service.dll fingerprints via media.dll.
 */
KANARIV_API kanariv::utils::Expected<std::array<uint8_t, 32>>
computeFingerprint(const std::filesystem::path& path);

} // namespace kanariv::media::internal
