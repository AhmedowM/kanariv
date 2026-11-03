#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/player/player.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
using namespace kanariv::player;
using namespace kanariv::utils;
using namespace kanariv::db;
using namespace kanariv::engine;
using namespace kanariv::test_helpers;
using kanariv::media::Decoder;
using kanariv::media::FileReader;

static_assert(static_cast<int>(kanariv::player::State::Stopped) == 0);
static_assert(static_cast<int>(kanariv::player::State::Playing) == 1);
static_assert(static_cast<int>(kanariv::player::State::Paused) == 2);
static_assert(static_cast<int>(kanariv::player::State::Error) == 3);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Stopped) == 0);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Ready) == 1);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Playing) == 2);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Paused) == 3);
static_assert(!std::is_same_v<kanariv::player::State, kanariv::types::PlaybackState>);

static std::string fixturePath(const std::string& name) {
    for (auto c : {std::string("tests/fixtures/") + name, std::string("../tests/fixtures/") + name,
                   std::string("C:/Users/Secondary/Projects/kanariv-cpp/tests/fixtures/") + name}) {
        if (std::filesystem::exists(c))
            return c;
    }
#ifdef TEST_DATA_DIR
    std::string td = std::string(TEST_DATA_DIR) + "/" + name;
    if (std::filesystem::exists(td))
        return td;
#endif
    return "tests/fixtures/" + name;
}
TEST_CASE("player create open decode wav", "[player_integration]") {
    auto r = FileReader::open(fixturePath("sample.wav"));
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    REQUIRE((*dec)->sampleRate() > 0);
    REQUIRE((*dec)->channels() > 0);
    std::array<float, 1024> out{};
    size_t got = (*dec)->decode(out);
    REQUIRE(got > 0);
}

TEST_CASE("player open invalid path returns NotFound", "[player_integration]") {
    auto r = FileReader::open("/nonexistent_xyz/sample.wav");
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::NotFound);
}

TEST_CASE("player seek and volume", "[player_integration]") {
    KANARIV_SKIP_IF_NOAUDIO();
    auto r = FileReader::open(fixturePath("sample.wav"));
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    REQUIRE((*dec)->seek(0.0).has_value());
    REQUIRE((*dec)->seek(-1.0).error().code == StatusCode::InvalidArg);
    // audio output volume
    SpscRing<float> ring{8192, (*dec)->channels()};
    AudioOutput::Config cfg;
    cfg.sampleRate = (*dec)->sampleRate();
    cfg.channels = (*dec)->channels();
    cfg.ring = &ring;
    auto outRes = AudioOutput::create(cfg);
    REQUIRE(outRes.has_value());
    (*outRes)->setVolume(0.5f);
    REQUIRE((*outRes)->volume() == 0.5f);
    (*outRes)->setVolume(2.0f);
    REQUIRE((*outRes)->volume() == 1.0f);
    (*outRes)->setVolume(-1.0f);
    REQUIRE((*outRes)->volume() == 0.0f);
}

TEST_CASE("player position time-based via Engine steady_clock", "[player_integration]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("pl_pos").string();
    {
        auto dbRes = Database::open(dbPath);
        REQUIRE(dbRes.has_value());
        auto db = std::move(dbRes.value());
        kanariv::types::Track t;
        t.path = fixturePath("sample.wav");
        t.duration = 2.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0x55 + b);
        auto ins = db->insertTrack(t);
        REQUIRE(ins.has_value());
        REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
        kanariv::types::EngineConfig cfg;
        cfg.enableMonitorThread = false;
        auto eRes = Engine::create(cfg);
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(
            eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
        REQUIRE(eng->play(1).has_value());
        REQUIRE(eng->state() == kanariv::types::PlaybackState::Playing);
        double pos0 = eng->position();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        double pos1 = eng->position();
        REQUIRE(pos1 > pos0);
        REQUIRE(pos1 >= 0.08);
        // pause should freeze position
        REQUIRE(eng->pause().has_value());
        REQUIRE(eng->state() == kanariv::types::PlaybackState::Paused);
        double paused = eng->position();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        REQUIRE(eng->position() == paused);
        // resume should advance
        REQUIRE(eng->resume().has_value());
        REQUIRE(eng->state() == kanariv::types::PlaybackState::Playing);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        REQUIRE(eng->position() > paused);
        REQUIRE(eng->stop().has_value());
        REQUIRE(eng->state() == kanariv::types::PlaybackState::Stopped);
    }
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
}

TEST_CASE("player state transitions invalid", "[player_integration]") {
    kanariv::types::EngineConfig cfg;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    // without db, play should fail State
    REQUIRE(!eng->play(1).has_value());
    REQUIRE(eng->play(1).error().code == StatusCode::State);
    // pause when stopped
    REQUIRE(!eng->pause().has_value());
    REQUIRE(eng->pause().error().code == StatusCode::State);
    // resume when not paused
    REQUIRE(!eng->resume().has_value());
}

TEST_CASE("player decode ogg via ffmpeg", "[player_integration]") {
    auto r = FileReader::open(fixturePath("sample.ogg"));
    REQUIRE(r.has_value());
    auto dec = Decoder::open(**r);
    REQUIRE(dec.has_value());
    std::array<float, 512> out{};
    REQUIRE((*dec)->decode(out) > 0);
    REQUIRE((*dec)->seek(0.0).has_value());
    REQUIRE((*dec)->decode(out) > 0);
}

TEST_CASE("player seek-while-playing race via Engine", "[player_integration][seek]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("seek_race").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = fixturePath("sample.wav");
    t.duration = 5.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0x60 + b);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
    kanariv::types::EngineConfig cfg;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    // Loop the single track: natural end stops on repeat Off, which would
    // end playback mid-test and fail the seeks below (this test races seeks,
    // not end behavior).
    REQUIRE(eng->setRepeat(kanariv::types::RepeatMode::All).has_value());
    // sequential seeks while playing (decode thread races with seek's ring reset)
    // we do small delays to let decode thread interleave but avoid true concurrent seeks which
    // cause data race
    int okCount = 0;
    for (int j = 0; j < 10; ++j) {
        double sec = j * 0.3;
        if (eng->seek(sec).has_value())
            okCount++;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        // also verify position after seek
        double pos = eng->position();
        REQUIRE(pos >= 0.0);
        REQUIRE(pos <= 5.0);
    }
    REQUIRE(okCount > 5);
    // ensure engine still playing and position is valid
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Playing);
    // seek to invalid -> should fail InvalidArg
    REQUIRE(!eng->seek(-1.0).has_value());
    REQUIRE(eng->seek(-1.0).error().code == StatusCode::InvalidArg);
    REQUIRE(!eng->seek(std::numeric_limits<double>::quiet_NaN()).has_value());
    eng.reset();
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
}

TEST_CASE("race player open concurrent", "[player_integration][race]") {
    // concurrent FileReader::open on same file should not crash and at least some succeed
    std::string path = fixturePath("sample.wav");
    const int N = 8;
    std::vector<std::thread> ths;
    std::atomic<int> success{0};
    std::atomic<int> fail{0};
    for (int i = 0; i < N; ++i) {
        ths.emplace_back([&]() {
            auto r = FileReader::open(path);
            if (r.has_value()) {
                auto dec = Decoder::open(**r);
                if (dec.has_value()) {
                    std::array<float, 256> out{};
                    if ((*dec)->decode(out) > 0)
                        success.fetch_add(1);
                    else
                        fail.fetch_add(1);
                } else
                    fail.fetch_add(1);
            } else
                fail.fetch_add(1);
        });
    }
    for (auto& th : ths)
        th.join();
    REQUIRE(success.load() > 0);
    REQUIRE(success.load() + fail.load() == N);
    // concurrent open of nonexistent should all return NotFound without crash
    std::atomic<int> notFound{0};
    ths.clear();
    for (int i = 0; i < 4; ++i) {
        ths.emplace_back([&]() {
            auto r = FileReader::open("/nonexistent_race_xyz.wav");
            if (!r.has_value() && r.error().code == StatusCode::NotFound)
                notFound.fetch_add(1);
        });
    }
    for (auto& th : ths)
        th.join();
    REQUIRE(notFound.load() == 4);
}

TEST_CASE("gapless via Player Engine 300ms lookahead", "[player_integration][gapless]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("gapless_player").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = fixturePath("sample.wav");
        t.duration = 0.9;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0x70 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.historyThresholdPct = 100;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    busyWaitUntil([&] { return eng->currentTrackId() != first; }, std::chrono::milliseconds(3000),
                  std::chrono::milliseconds(20));
    REQUIRE(eng->currentTrackId() != first);
    eng.reset();
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
}
