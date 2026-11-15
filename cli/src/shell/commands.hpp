#pragma once

/**
 * @file commands.hpp
 * @brief CLI command declarations and per-domain dispatch (internal).
 * @details One Options aggregate owns every parsed value; each domain
 * declares its own subcommands and dispatches its own handlers, so Shell::run
 * stays a short linear chain instead of one 1 200-line function. Adding a
 * command means touching one domain file in two places (declare + dispatch),
 * and the declaration-vocabulary tripwire test fails when those drift.
 */

#include <CLI/CLI.hpp>
#include <cstddef>
#include <cstdint>
#include <kanariv/app/app.hpp>
#include <kanariv/config.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace kanariv::app::cli::detail {

/**
 * @brief Root options parsed before any subcommand dispatches.
 * @ingroup kanariv_app
 */
struct GlobalOptions {
    std::string configPath;
    std::string logLevel;
    std::string device;
    std::vector<std::string> paths;
    bool save = false;
    bool daemon = false;
    bool foreground = false;
};

/** @brief `start`, `shutdown` and the early `version` exit. @ingroup kanariv_app */
struct LifecycleOptions {
    CLI::App* start = nullptr;
    bool startForeground = false;
    CLI::App* shutdown = nullptr;
    CLI::App* version = nullptr;
    bool versionJson = false;
};

/** @brief Transport commands, seek, status (incl. watch) and volume. @ingroup kanariv_app */
struct PlaybackOptions {
    CLI::App* play = nullptr;
    bool playJson = false;
    CLI::App* pause = nullptr;
    bool pauseJson = false;
    CLI::App* resume = nullptr;
    bool resumeJson = false;
    CLI::App* restart = nullptr;
    bool restartJson = false;
    CLI::App* stop = nullptr;
    bool stopJson = false;
    CLI::App* next = nullptr;
    bool nextJson = false;
    CLI::App* prev = nullptr;
    bool prevJson = false;
    CLI::App* seek = nullptr;
    std::string seekStr;
    bool seekJson = false;
    CLI::App* status = nullptr;
    bool statusJson = false;
    bool statusWatch = false;
    bool statusFollow = false;
    int statusInterval = 1000;
    CLI::App* volume = nullptr;
    std::string volumeArg;
    bool volumeJson = false;
};

/** @brief `queue` plus its twelve subcommands. @ingroup kanariv_app */
struct QueueOptions {
    CLI::App* queue = nullptr;
    CLI::App* list = nullptr;
    bool listJson = false;
    CLI::App* queuesAlias = nullptr;
    bool queuesAliasJson = false;
    CLI::App* tracks = nullptr;
    std::string tracksOrder = "playback";
    bool tracksJson = false;
    CLI::App* switchTo = nullptr;
    std::int64_t switchId = 0;
    bool switchJson = false;
    CLI::App* add = nullptr;
    std::vector<std::string> addPaths;
    std::string addId;
    bool addSearch = false;
    bool addRecursive = false;
    std::int64_t addPlaylist = 0;
    bool addReplace = false;
    bool addJson = false;
    CLI::App* remove = nullptr;
    std::vector<std::string> removePaths;
    std::string removeId;
    std::string removePos;
    bool removeRecursive = false;
    bool removeJson = false;
    CLI::App* create = nullptr;
    std::string createName;
    bool createJson = false;
    CLI::App* deleteQueue = nullptr;
    std::int64_t deleteQid = 0;
    bool deleteJson = false;
    CLI::App* move = nullptr;
    std::size_t from = 0;
    std::size_t to = 0;
    bool moveJson = false;
    CLI::App* clear = nullptr;
    bool clearJson = false;
    CLI::App* shuffle = nullptr;
    std::string shuffleArg;
    bool shuffleJson = false;
    CLI::App* repeat = nullptr;
    std::string repeatArg;
    bool repeatJson = false;
};

/** @brief `playlist` plus its ten subcommands. @ingroup kanariv_app */
struct PlaylistOptions {
    CLI::App* playlist = nullptr;
    CLI::App* list = nullptr;
    bool listJson = false;
    CLI::App* tracks = nullptr;
    std::int64_t tracksPid = 0;
    bool tracksJson = false;
    CLI::App* create = nullptr;
    std::string createName;
    std::string createSmart;
    bool createJson = false;
    CLI::App* add = nullptr;
    std::int64_t addPid = 0;
    std::vector<std::string> addIds;
    std::vector<std::string> addPaths;
    bool addRecursive = false;
    bool addJson = false;
    CLI::App* load = nullptr;
    std::int64_t loadPid = 0;
    bool loadPlay = false;
    bool loadReplace = false;
    bool loadJson = false;
    CLI::App* save = nullptr;
    std::string saveName;
    std::int64_t saveQid = 0;
    bool saveJson = false;
    CLI::App* remove = nullptr;
    std::int64_t removePid = 0;
    bool removeJson = false;
    CLI::App* rename = nullptr;
    std::int64_t renamePid = 0;
    std::string renameName;
    bool renameJson = false;
    CLI::App* exportFile = nullptr;
    std::int64_t exportPid = 0;
    std::string exportPath;
    std::string exportFormat = "m3u";
    CLI::App* importFile = nullptr;
    std::string importPath;
    std::string importName;
    bool importJson = false;
};

/** @brief `library` plus its eight subcommands. @ingroup kanariv_app */
struct LibraryOptions {
    CLI::App* library = nullptr;
    CLI::App* scan = nullptr;
    std::string scanPath;
    bool scanFullHash = false;
    bool scanJson = false;
    CLI::App* search = nullptr;
    std::string searchQuery;
    int searchLimit = 50;
    bool searchJson = false;
    CLI::App* stats = nullptr;
    bool statsJson = false;
    int statsTopTracks = -1;
    int statsTopAlbums = 0;
    int statsTopGenres = 0;
    int statsTopArtists = 0;
    std::vector<std::string> statsQueues;
    std::vector<std::int64_t> statsPlaylists;
    CLI::App* add = nullptr;
    std::string addPath;
    bool addRecursive = false;
    bool addJson = false;
    CLI::App* remove = nullptr;
    std::string removeQuery;
    bool removeJson = false;
    CLI::App* list = nullptr;
    int listLimit = 50;
    int listOffset = 0;
    std::string listQuery;
    std::string listSort;
    std::string listOrder;
    std::string listColumns;
    std::string listArtist;
    std::string listAlbum;
    std::string listGenre;
    int listRating = 0;
    bool listJson = false;
    CLI::App* favorites = nullptr;
    int favoritesMinRating = 1;
    int favoritesLimit = 50;
    bool favoritesJson = false;
    CLI::App* maintenance = nullptr;
    std::string maintenanceAction = "report";
    bool maintenanceDryRun = true;
    std::int64_t maintenanceLibraryId = 1;
    bool maintenanceJson = false;
};

/** @brief `tag` and `track` subcommands. @ingroup kanariv_app */
struct TagOptions {
    CLI::App* tag = nullptr;
    CLI::App* edit = nullptr;
    std::string editId;
    std::string editField;
    std::string editValue;
    bool editJson = false;
    CLI::App* get = nullptr;
    std::string getId;
    std::string getField;
    bool getJson = false;
    std::string getOut;
    CLI::App* batch = nullptr;
    std::string batchFile;
    bool batchDryRun = false;
    bool batchJson = false;
    CLI::App* track = nullptr;
    CLI::App* rate = nullptr;
    std::string rateId;
    int rateRating = 0;
    bool rateJson = false;
};

/** @brief `history` and `bookmark` subcommands. @ingroup kanariv_app */
struct HistoryOptions {
    CLI::App* history = nullptr;
    CLI::App* list = nullptr;
    bool listJson = false;
    int limit = 50;
    CLI::App* clear = nullptr;
    bool clearJson = false;
    CLI::App* bookmark = nullptr;
    CLI::App* add = nullptr;
    std::string addTime;
    std::string addNote;
    std::int64_t addTrack = 0;
    bool addJson = false;
    CLI::App* bookmarks = nullptr;
    std::int64_t bookmarksTrack = 0;
    bool bookmarksJson = false;
    CLI::App* remove = nullptr;
    std::int64_t removeId = 0;
    bool removeJson = false;
    CLI::App* gotoBookmark = nullptr;
    std::int64_t gotoId = 0;
    bool gotoJson = false;
};

/** @brief `lyrics` subcommands (incl. the live display). @ingroup kanariv_app */
struct LyricsOptions {
    CLI::App* lyrics = nullptr;
    CLI::App* set = nullptr;
    std::string setText;
    bool setSynced = false;
    std::string setSource = "local";
    std::string setTrack;
    std::string setFile;
    bool setJson = false;
    CLI::App* get = nullptr;
    std::string getTrack;
    std::string getOut;
    bool getLive = false;
    bool getJson = false;
    CLI::App* remove = nullptr;
    std::string removeTrack;
    bool removeJson = false;
};

/** @brief `preview` (daemon-less local audition). @ingroup kanariv_app */
struct PreviewOptions {
    CLI::App* preview = nullptr;
    std::string file;
    bool keepPlaying = false;
};

/** @brief `config`, `device` and `info` subcommands. @ingroup kanariv_app */
struct ConfigOptions {
    CLI::App* config = nullptr;
    CLI::App* get = nullptr;
    std::string getKey;
    bool getJson = false;
    CLI::App* set = nullptr;
    std::string setKey;
    std::string setValue;
    CLI::App* list = nullptr;
    bool listJson = false;
    CLI::App* exportFile = nullptr;
    std::string exportPath;
    bool exportJson = false;
    CLI::App* importFile = nullptr;
    std::string importPath;
    bool importJson = false;
    CLI::App* reset = nullptr;
    std::string resetKey;
    bool resetJson = false;
    CLI::App* device = nullptr;
    CLI::App* devList = nullptr;
    bool devListJson = false;
    CLI::App* devSet = nullptr;
    std::string devSetId;
    bool devSetJson = false;
    CLI::App* devTest = nullptr;
    std::string devTestId;
    bool devTestTone = false;
    bool devTestJson = false;
    CLI::App* info = nullptr;
    bool infoJson = false;
};

/** @brief Every parsed CLI value, grouped by domain. @ingroup kanariv_app */
struct Options {
    GlobalOptions global;
    LifecycleOptions lifecycle;
    PlaybackOptions playback;
    QueueOptions queue;
    PlaylistOptions playlist;
    LibraryOptions library;
    TagOptions tag;
    HistoryOptions history;
    LyricsOptions lyrics;
    PreviewOptions preview;
    ConfigOptions config;
};

/**
 * @brief Adds the shared `--json` flag to a subcommand.
 * @param cmd Subcommand receiving the flag.
 * @param out Receives the parsed value (must outlive the CLI app).
 * @param desc Flag description; the default covers the common case.
 */
inline void addJsonFlag(CLI::App* cmd, bool& out, const char* desc = "JSON output") {
    cmd->add_flag("--json", out, desc);
}

void declareGlobal(CLI::App& cli, Options& o);
void declareLifecycle(CLI::App& cli, Options& o);
void declarePlayback(CLI::App& cli, Options& o);
void declareQueue(CLI::App& cli, Options& o);
void declarePlaylist(CLI::App& cli, Options& o);
void declareLibrary(CLI::App& cli, Options& o);
void declareTags(CLI::App& cli, Options& o);
void declareHistory(CLI::App& cli, Options& o);
void declareLyrics(CLI::App& cli, Options& o);
void declarePreview(CLI::App& cli, Options& o);
void declareConfig(CLI::App& cli, Options& o);

/**
 * @brief Declares every option and subcommand in help-display order.
 * @param cli Root CLI app.
 * @param o Options aggregate receiving every parsed value.
 */
void declareAll(CLI::App& cli, Options& o);

/**
 * @brief Reports whether the invocation changes persisted state.
 * @details First-run defaults are only persisted for these commands: a
 * read-only invocation must not create a config file.
 */
bool isMutating(const Options& o);

/** @brief `kanariv PATH... [--save]` direct play; nullopt when not used. */
std::optional<int> runDirectPlay(App& app, const CLI::App& cli, const Options& o);
/** @brief `start` / `shutdown`; nullopt when neither was invoked. */
std::optional<int> runLifecycle(App& app, std::stop_token stop, const Options& o);
/** @brief Transport, status and volume commands. */
std::optional<int> runPlayback(App& app, const config::Config& cfg, std::stop_token stop,
                               const Options& o);
/** @brief Queue commands. */
std::optional<int> runQueue(App& app, const Options& o);
/** @brief Playlist commands. */
std::optional<int> runPlaylist(App& app, const Options& o);
/** @brief Library commands. */
std::optional<int> runLibrary(App& app, const Options& o);
/** @brief Tag and track commands. */
std::optional<int> runTags(App& app, const Options& o);
/** @brief History and bookmark commands. */
std::optional<int> runHistory(App& app, const Options& o);
/** @brief Lyrics commands (incl. `lyrics get --live`). */
std::optional<int> runLyrics(App& app, const config::Config& cfg, std::stop_token stop,
                             const Options& o);
/** @brief Local preview command. */
std::optional<int> runPreview(App& app, const config::Config& cfg, std::stop_token stop,
                              const Options& o);
/** @brief Config, device and info commands. */
std::optional<int> runConfig(App& app, const Options& o);

} // namespace kanariv::app::cli::detail
