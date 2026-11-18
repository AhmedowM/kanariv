#pragma once

/**
 * @file library_scan.hpp
 * @brief Library scan port: discovery plus batched persistence of a root.
 * @ingroup kanariv_contracts
 * @details Scanning a library spans two components -- `kanariv::media` walks
 * the tree and fingerprints files, and the backing store upserts the result --
 * so the orchestration lives behind this narrow port. The port itself is
 * media-free: consumers (the daemon) depend on the contract, not on the FFmpeg
 * decoder stack, and only the composition root wires a concrete scanner.
 *
 * @par Error behavior
 * Implementations use `StatusCode::InvalidArg` for a zero or unknown library
 * id, `NotFound` for a missing root, `Busy` when the store cannot start a
 * write batch, and `Io`/`Internal` for backend failures.
 */

#include <cstdint>
#include <expected>
#include <functional>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <string_view>

namespace kanariv::contracts {

/**
 * @brief Scan fingerprinting mode.
 * @ingroup kanariv_contracts
 * @details Mirrors `kanariv::media::ScanMode` so this contract stays
 * decoder-free; the scanner implementation maps one onto the other.
 */
enum class ScanMode {
    Sampled, ///< Sampled BLAKE3 (head+tail+size) -- fast, default.
    Full     ///< Full-file BLAKE3 -- slower, more collision-resistant.
};

/**
 * @brief Scans a configured library root and upserts the discovered tracks.
 * @ingroup kanariv_contracts
 */
class ILibraryScanner {
  public:
    /**
     * @brief Destroys the scanner view.
     * @ingroup kanariv_contracts
     */
    virtual ~ILibraryScanner() = default;

    /**
     * @brief Scans one library root and persists the discovered tracks.
     * @param library_id Library identifier whose path is the scan root.
     * @param progress Optional callback `progress(scanned, total, path)`.
     * @param mode Discovery mode.
     * @return Success, or `InvalidArg`/`NotFound`/`Busy`/`Io`/`Internal`.
     * @details Discovery runs outside the store lock; persistence proceeds in
     * bounded batches. Callers must not hold the store lock across the call.
     */
    virtual kanariv::utils::Expected<void>
    scanLibrary(int64_t library_id,
                std::function<void(int64_t, int64_t, std::string_view)> progress = {},
                ScanMode mode = ScanMode::Sampled) = 0;
};

} // namespace kanariv::contracts
