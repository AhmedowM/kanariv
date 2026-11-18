module;
#include <kanariv/contracts/storage.hpp>

export module kanariv.contracts:storage;

export namespace kanariv::contracts {
using ::kanariv::contracts::IHistoryStore;
using ::kanariv::contracts::ILibraryAnalyzer;
using ::kanariv::contracts::ILibraryScanner;
using ::kanariv::contracts::ILibraryStore;
using ::kanariv::contracts::IPlaybackStateStore;
using ::kanariv::contracts::IPlaylistStore;
using ::kanariv::contracts::IQueueStore;
using ::kanariv::contracts::IStore;
using ::kanariv::contracts::ITrackStore;
using ::kanariv::contracts::LibraryAnalyzeOptions;
using ::kanariv::contracts::LibraryAnalyzeReport;
using ::kanariv::contracts::ScanMode;
using ::kanariv::contracts::Storage;
} // namespace kanariv::contracts
