#pragma once

/**
 * @file storage.hpp
 * @brief Composition of the persistence ports.
 * @ingroup kanariv_contracts
 * @details The backing store is one object exposed through several narrow
 * ports. This aggregate is the wiring token: constructed once at the
 * composition root (and by tests) from a single backend, then handed to
 * consumers, each of which reads the ports it needs. It is deliberately not an
 * interface: no method is defined here, so it can never grow into the
 * god-interface the ports replaced.
 */

#include <kanariv/contracts/history_store.hpp>
#include <kanariv/contracts/library_store.hpp>
#include <kanariv/contracts/playback_state_store.hpp>
#include <kanariv/contracts/playlist_store.hpp>
#include <kanariv/contracts/queue_store.hpp>
#include <kanariv/contracts/store.hpp>
#include <kanariv/contracts/track_store.hpp>
#include <memory>

namespace kanariv::contracts {

/**
 * @brief The persistence ports a process was configured with.
 * @ingroup kanariv_contracts
 * @details Every populated member shares ownership of the same backend, so a
 * consumer that keeps the aggregate alive keeps the store alive. Wiring code
 * builds it with `kanariv::db::Database`; tests may substitute fakes per port.
 */
struct Storage {
    std::shared_ptr<ITrackStore> tracks;        ///< Track rows and annotations.
    std::shared_ptr<IPlaylistStore> playlists;  ///< Playlist containers.
    std::shared_ptr<IQueueStore> queues;        ///< Queues and their items.
    std::shared_ptr<ILibraryStore> library;     ///< Libraries, queries, stats.
    std::shared_ptr<IHistoryStore> history;     ///< Opt-in play history.
    std::shared_ptr<IPlaybackStateStore> state; ///< Engine state sidecar.

    /// True when every port is populated.
    [[nodiscard]] bool complete() const noexcept {
        return tracks && playlists && queues && library && history && state;
    }
};

} // namespace kanariv::contracts
