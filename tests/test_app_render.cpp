/**
 * @file test_app_render.cpp
 * @brief Regression coverage for app-side human rendering of IPC results.
 */

#include <app/detail.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <kanariv/app/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <string>
#include <utility>
#include <variant>

namespace {

kanariv::types::TopEntity entity(std::string name, std::size_t tracks, std::int64_t plays) {
    kanariv::types::TopEntity e;
    e.name = std::move(name);
    e.track_count = tracks;
    e.total_plays = plays;
    return e;
}

/// @brief Removes a scratch directory on scope exit (best effort).
struct TempDir {
    std::filesystem::path path;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("library stats summary when no top report is requested", "[app_render]") {
    kanariv::types::LibraryStats v{};
    v.tracks = 3;
    v.queues = 1;
    v.playlists = 2;

    std::string out;
    kanariv::app::detail::renderInto(out, kanariv::ipc::Result{v});

    REQUIRE(out.find("Tracks: 3 Queues: 1 Playlists: 2") != std::string::npos);
    REQUIRE(out.find("Library Stats (Detailed):") == std::string::npos);
}

TEST_CASE("library stats renders every requested top report", "[app_render]") {
    kanariv::types::LibraryStats v{};
    v.tracks = 3;
    v.queues = 1;
    v.playlists = 2;
    v.total_duration_ms = 600000;
    v.total_play_time_ms = 120000;
    // Only the non-track reports are requested: before the fix this printed
    // the one-line summary and dropped albums/genres/artists entirely.
    v.top_albums.emplace();
    v.top_albums->push_back(entity("Album A", 2, 7));
    v.top_genres.emplace();
    v.top_genres->push_back(entity("Genre G", 3, 9));
    v.top_artists.emplace();
    v.top_artists->push_back(entity("Artist X", 3, 9));

    std::string out;
    kanariv::app::detail::renderInto(out, kanariv::ipc::Result{v});

    REQUIRE(out.find("Library Stats (Detailed):") != std::string::npos);
    REQUIRE(out.find("Top Albums by Plays:") != std::string::npos);
    REQUIRE(out.find("Album A") != std::string::npos);
    REQUIRE(out.find("Top Genres by Plays:") != std::string::npos);
    REQUIRE(out.find("Genre G") != std::string::npos);
    REQUIRE(out.find("Top Artists by Plays:") != std::string::npos);
    REQUIRE(out.find("Artist X") != std::string::npos);
    REQUIRE(out.find("Most Played Tracks:") == std::string::npos);
}

TEST_CASE("readiness attempts fit one absolute 5 s budget", "[app_render]") {
    using kanariv::app::detail::daemonReadyAttempt;
    using kanariv::app::detail::daemonReadyBudget;
    using kanariv::app::detail::readinessAttemptTimeout;
    using std::chrono::milliseconds;

    REQUIRE(daemonReadyBudget == milliseconds{5000});
    // A full attempt is bounded by the per-attempt cap...
    REQUIRE(readinessAttemptTimeout(milliseconds{5000}) == daemonReadyAttempt);
    // ...and truncated to whatever budget remains.
    REQUIRE(readinessAttemptTimeout(milliseconds{250}) == milliseconds{250});
    // An exhausted budget yields no attempt at all (the loop stops).
    REQUIRE(readinessAttemptTimeout(milliseconds{0}) == milliseconds{0});
    REQUIRE(readinessAttemptTimeout(milliseconds{-50}) == milliseconds{0});
}

TEST_CASE("daemon-down error renders the start hint", "[app_render]") {
    const auto e = kanariv::app::detail::daemonDownError();
    REQUIRE(e.code == kanariv::utils::StatusCode::State);

    std::string err;
    kanariv::app::detail::renderErrorInto(err, e);

    REQUIRE(err == "Error: State daemon not running\n"
                   "hint: run `kanariv start` to start the daemon");
}

TEST_CASE("a failed daemon start is an unexpected, not a failed outcome", "[app_render]") {
    namespace fs = std::filesystem;
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    TempDir dir{fs::temp_directory_path() / ("kanariv-app-start-" + stamp)};

    kanariv::config::Config cfg;
    cfg.dbPath = dir.path / "library.db";
    cfg.socketPath = (dir.path / "kanariv.sock").string();
    kanariv::app::App app{cfg};

    // No daemon host attached: the foreground start must fail through the
    // error channel, which is what sendPlay() checks after a failed start.
    auto r = app.startDaemon(true);
    REQUIRE(!r);
    REQUIRE(r.error().code == kanariv::utils::StatusCode::State);
    REQUIRE(r.error().message == "daemon host not configured");
}
