#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <kanariv/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace CLI {
class App;
}

namespace kanariv::app::cli {

namespace detail {

// CLI-internal helpers (adapt parse.hpp string-errors into utils::Error).

/// @brief Parses `s`/`mm:ss`/`hh:mm:ss` into seconds (see parse.hpp).
auto parseTime(std::string_view s) -> std::expected<double, kanariv::utils::Error>;
/// @brief Parses absolute or `+`/`-` relative seeks into seconds-or-delta.
auto parseSeek(std::string_view s) -> std::expected<double, kanariv::utils::Error>;
/// @brief Parses `0-100`/`+n`/`-n`/`mute`/`unmute`/empty(show) into VolumeSet.
auto parseVolume(std::string_view s)
    -> std::expected<kanariv::ipc::VolumeSet, kanariv::utils::Error>;
/// @brief parseTime result as a duration.
auto parseDuration(std::string_view s) -> std::chrono::duration<double>;

} // namespace detail

using detail::parseSeek;
using detail::parseTime;
using detail::parseVolume;

class Shell {
  public:
    explicit Shell(kanariv::config::Config cfg);
    ~Shell();
    auto run(int argc, char** argv) -> int;

  private:
    kanariv::config::Config config_;
    kanariv::app::App app_;
    std::unique_ptr<CLI::App> cli_;

    // CLI subcommand pointers (set during construction; read during dispatch).
    CLI::App* startCmd = nullptr;
    CLI::App* shutdownCmd = nullptr;
    CLI::App* playCmd = nullptr;
    CLI::App* pauseCmd = nullptr;
    CLI::App* resumeCmd = nullptr;
    CLI::App* restartCmd = nullptr;
    CLI::App* stopCmd = nullptr;
    CLI::App* nextCmd = nullptr;
    CLI::App* prevCmd = nullptr;
    CLI::App* seekCmd = nullptr;
    CLI::App* statusCmd = nullptr;
    CLI::App* volumeCmd = nullptr;
    CLI::App* queueCmd = nullptr;
    CLI::App* qList = nullptr;
    CLI::App* qQueuesAlias = nullptr;
    CLI::App* qTracks = nullptr;
    CLI::App* qSwitch = nullptr;
    CLI::App* qAdd = nullptr;
    CLI::App* qRemove = nullptr;
    CLI::App* qCreate = nullptr;
    CLI::App* qDelete = nullptr;
    CLI::App* qMove = nullptr;
    CLI::App* qClear = nullptr;
    CLI::App* qShuffle = nullptr;
    CLI::App* qRepeat = nullptr;
    CLI::App* plCmd = nullptr;
    CLI::App* plList = nullptr;
    CLI::App* plTracks = nullptr;
    CLI::App* plCreate = nullptr;
    CLI::App* plAdd = nullptr;
    CLI::App* plLoad = nullptr;
    CLI::App* plSave = nullptr;
    CLI::App* plDelete = nullptr;
    CLI::App* plRename = nullptr;
    CLI::App* plExport = nullptr;
    CLI::App* plImport = nullptr;
    CLI::App* libCmd = nullptr;
    CLI::App* libScan = nullptr;
    CLI::App* libSearch = nullptr;
    CLI::App* libStats = nullptr;
    CLI::App* libList = nullptr;
    CLI::App* libFav = nullptr;
    CLI::App* libMaint = nullptr;
    CLI::App* libAdd = nullptr;
    CLI::App* libRemove = nullptr;
    CLI::App* tagCmd = nullptr;
    CLI::App* tagEdit = nullptr;
    CLI::App* tagGet = nullptr;
    CLI::App* trackCmd = nullptr;
    CLI::App* trRate = nullptr;
    CLI::App* historyCmd = nullptr;
    CLI::App* historyList = nullptr;
    CLI::App* historyClear = nullptr;
    CLI::App* bookmarkCmd = nullptr;
    CLI::App* bmAdd = nullptr;
    CLI::App* bmList = nullptr;
    CLI::App* bmRemove = nullptr;
    CLI::App* bmGoto = nullptr;
    CLI::App* previewCmd = nullptr;
    CLI::App* cfgCmd = nullptr;
    CLI::App* cfgGet = nullptr;
    CLI::App* cfgSet = nullptr;
    CLI::App* cfgList = nullptr;
    CLI::App* cfgExport = nullptr;
    CLI::App* cfgImport = nullptr;
    CLI::App* cfgReset = nullptr;
    CLI::App* deviceCmd = nullptr;
    CLI::App* devList = nullptr;
    CLI::App* devSet = nullptr;
    CLI::App* devTest = nullptr;
    CLI::App* infoCmd = nullptr;
    CLI::App* versionCmd = nullptr;

    // CLI state (bound by CLI::App during construction; read during dispatch).
    std::string configPathStr;
    std::string logLevelStr;
    std::string deviceStr;
    std::vector<std::string> posPaths;
    bool posSave = false;
    bool daemonFlag = false;
    bool globalFg = false;
    bool fg = false;

    // Transport
    bool playJson = false;
    bool pauseJson = false;
    bool resumeJson = false;
    bool restartJson = false;
    bool stopJson = false;
    bool nextJson = false;
    bool prevJson = false;
    std::string seekStr;
    bool seekJson = false;
    bool jsonFlag = false;
    bool statusWatch = false;
    static constexpr int defaultStatusInterval = 1000;
    static constexpr int defaultSearchLimit = 50;
    static constexpr int defaultListLimit = 50;
    static constexpr int defaultFavLimit = 50;
    static constexpr int defaultHistoryLimit = 50;
    int statusInterval = defaultStatusInterval;
    std::string volumeArg;
    bool volumeJson = false;

    // Queue
    bool qJson = false;
    bool qQueuesJson = false;
    std::string qTracksOrder = "playback";
    bool qTracksJson = false;
    std::int64_t qSwitchId = 0;
    bool qSwitchJson = false;
    std::vector<std::string> qAddPaths;
    std::string qAddId;
    bool qAddSearch = false;
    bool qAddRecursive = false;
    std::int64_t qAddPlaylist = 0;
    bool qAddReplace = false;
    bool qAddJson = false;
    std::vector<std::string> qRemovePaths;
    std::string qRemoveId;
    std::string qRemovePos;
    bool qRemoveRecursive = false;
    bool qRemoveJson = false;
    std::string qCreateName;
    bool qCreateJson = false;
    std::int64_t qDeleteQid = 0;
    bool qDeleteJson = false;
    std::size_t qFrom = 0;
    std::size_t qTo = 0;
    bool qMoveJson = false;
    bool qClearJson = false;
    std::string qShuffleArg;
    bool qShuffleJson = false;
    std::string qRepeatArg;
    bool qRepeatJson = false;

    // Playlist
    bool plJson = false;
    std::int64_t plTracksPid = 0;
    bool plTracksJson = false;
    std::string plCreateName;
    std::string plCreateSmart;
    bool plCreateJson = false;
    std::int64_t plAddPid = 0;
    std::vector<std::string> plAddIds;
    std::vector<std::string> plAddPaths;
    bool plAddRecursive = false;
    bool plAddJson = false;
    std::int64_t plLoadPid = 0;
    bool plLoadPlay = false;
    bool plLoadReplace = false;
    bool plLoadJson = false;
    std::string plSaveName;
    std::int64_t plSaveQid = 0;
    bool plSaveJson = false;
    std::int64_t plDeletePid = 0;
    bool plDeleteJson = false;
    std::int64_t plRenamePid = 0;
    std::string plRenameName;
    bool plRenameJson = false;
    std::int64_t plExportPid = 0;
    std::string plExportPath;
    std::string plExportFormat = "m3u";
    std::string plImportPath;
    std::string plImportName;
    bool plImportJson = false;

    // Library
    std::string libScanPath;
    bool libScanFullHash = false;
    bool libScanJson = false;
    std::string libSearchQuery;
    int libSearchLimit = defaultSearchLimit;
    bool libSearchJson = false;
    bool libStatsJson = false;
    int libStatsTopTracks = -1;
    int libStatsTopAlbums = 0;
    int libStatsTopGenres = 0;
    int libStatsTopArtists = 0;
    std::vector<std::string> libStatsQueues;
    std::vector<std::int64_t> libStatsPlaylists;
    std::string libAddPath;
    bool libAddRecursive = false;
    bool libAddJson = false;
    std::string libRemoveQuery;
    bool libRemoveJson = false;
    int libListLimit = defaultListLimit;
    int libListOffset = 0;
    std::string libListQuery;
    std::string libListSort;
    std::string libListOrder;
    std::string libListColumns;
    std::string libListArtist;
    std::string libListAlbum;
    std::string libListGenre;
    int libListRating = 0;
    bool libListJson = false;
    int libFavMinRating = 1;
    int libFavLimit = defaultFavLimit;
    bool libFavJson = false;
    std::string libMaintAction = "report";
    bool libMaintDryRun = true;
    std::int64_t libMaintLibraryId = 1;
    bool libMaintJson = false;

    // Tag
    std::string tagEditId;
    std::string tagEditField;
    std::string tagEditValue;
    bool tagEditJson = false;
    std::string tagGetId;
    std::string tagGetField;
    bool tagGetJson = false;
    std::string tagGetOut;

    // Track
    std::string trRateId;
    int trRateRating = 0;
    bool trRateJson = false;

    // History
    bool historyJson = false;
    bool historyClearJson = false;
    int historyLimit = defaultHistoryLimit;

    // Bookmark
    std::string bmAddTime;
    std::string bmAddNote;
    std::int64_t bmAddTrack = 0;
    bool bmAddJson = false;
    std::int64_t bmListTrack = 0;
    bool bmListJson = false;
    std::int64_t bmRemoveId = 0;
    bool bmRemoveJson = false;
    std::int64_t bmGotoId = 0;
    bool bmGotoJson = false;

    // Preview
    std::string previewFile;
    bool previewKeepPlaying = false;

    // Config
    std::string cfgGetKey;
    bool cfgGetJson = false;
    std::string cfgSetKey;
    std::string cfgSetVal;
    bool cfgListJson = false;
    std::string cfgExportPath;
    bool cfgExportJson = false;
    std::string cfgImportPath;
    bool cfgImportJson = false;
    std::string cfgResetKey;
    bool cfgResetJson = false;

    // Device
    bool devJson = false;
    std::string devSetId;
    bool devSetJson = false;
    std::string devTestId;
    bool devTestJson = false;
    bool devTestTone = false;

    // Info / Version
    bool infoJson = false;
    bool versionJson = false;

    // CLI construction helpers (register options/subcommands into cli_).
    void setupGlobalOptions();
    void setupStartShutdown();
    void setupTransportCommands();
    void setupQueueCommands();
    void setupPlaylistCommands();
    void setupLibraryCommands();
    void setupTagCommands();
    void setupTrackCommands();
    void setupHistoryCommands();
    void setupBookmarkCommands();
    void setupPreviewCommand();
    void setupConfigCommands();
    void setupDeviceCommands();
    void setupInfoCommand();
    void setupVersionCommand();

    // Environment setup helpers.
    auto setupConfig(const std::string& configPathStr) -> int;
    void setupDaemonHost();

    // Command dispatch handlers.
    auto dispatchVersion() const -> int;
    auto dispatchDaemonMode() -> int;
    auto dispatchDirectPlay() -> int;
    auto dispatchStart() -> int;
    auto dispatchShutdown() -> int;
    auto dispatchPlay() -> int;
    auto dispatchPause() -> int;
    auto dispatchResume() -> int;
    auto dispatchRestart() -> int;
    auto dispatchStop() -> int;
    auto dispatchNext() -> int;
    auto dispatchPrev() -> int;
    auto dispatchSeek() -> int;
    auto dispatchStatus() -> int;
    auto dispatchVolume() -> int;
    auto dispatchQueue() -> int;
    auto dispatchPlaylist() -> int;
    auto dispatchLibrary() -> int;
    auto dispatchTag() -> int;
    auto dispatchTrack() -> int;
    auto dispatchHistory() -> int;
    auto dispatchBookmark() -> int;
    auto dispatchPreview() -> int;
    auto dispatchConfig() -> int;
    auto dispatchDevice() -> int;
    auto dispatchInfo() -> int;
    auto dispatchHelp() -> int;
};

} // namespace kanariv::app::cli
