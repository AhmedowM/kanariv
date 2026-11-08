/**
 * @file test_app_render.cpp
 * @brief Regression coverage for app-side human rendering of IPC results.
 */

#include <app/detail.hpp>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/record.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

kanariv::types::TopEntity entity(std::string name, std::size_t tracks, std::int64_t plays) {
    kanariv::types::TopEntity e;
    e.name = std::move(name);
    e.track_count = tracks;
    e.total_plays = plays;
    return e;
}

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
