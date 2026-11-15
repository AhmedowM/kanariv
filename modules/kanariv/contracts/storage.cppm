module;
#include <kanariv/contracts/storage.hpp>

export module kanariv.contracts:storage;

export namespace kanariv::contracts {
using ::kanariv::contracts::IHistoryStore;
using ::kanariv::contracts::ILibraryStore;
using ::kanariv::contracts::IPlaybackStateStore;
using ::kanariv::contracts::IPlaylistStore;
using ::kanariv::contracts::IQueueStore;
using ::kanariv::contracts::IStore;
using ::kanariv::contracts::NullHistoryStore;
using ::kanariv::contracts::ITrackStore;
using ::kanariv::contracts::Storage;
} // namespace kanariv::contracts
