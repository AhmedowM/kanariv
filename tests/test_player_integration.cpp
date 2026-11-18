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
#include <kanariv/utils/ring.hpp>
#include <kanariv/utils/status_format.hpp>
#include <limits>
#include <memory>
#include <span>
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

static_assert(static_cast<int>(kanariv::types::PlaybackState::Stopped) == 0);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Ready) == 1);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Playing) == 2);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Paused) == 3);
static_assert(static_cast<int>(kanariv::types::PlaybackState::Error) == 4);
static_assert(std::is_same_v<kanariv::player::State, kanariv::types::PlaybackState>);

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

namespace {

/// @brief Reader wrapper that turns every read past a byte budget into an I/O
/// error, so the decoder reports DecodeStatus::Error instead of clean EOS.
class FailingReader final : public kanariv::media::Reader {
  public:
    FailingReader(std::unique_ptr<kanariv::media::Reader> inner, int64_t failAfter)
        : inner_(std::move(inner)), failAfter_(failAfter) {}

    std::size_t read(std::span<std::byte> dst) override {
        if (readBytes_ >= failAfter_) {
            status_ = kanariv::media::ReadStatus::Error;
            return 0;
        }
        int64_t budget = failAfter_ - readBytes_;
        if (static_cast<int64_t>(dst.size()) > budget)
            dst = dst.first(static_cast<std::size_t>(budget));
        std::size_t n = inner_->read(dst);
        if (n == 0) {
            status_ = kanariv::media::ReadStatus::Error;
            return 0;
        }
        readBytes_ += static_cast<int64_t>(n);
        status_ = kanariv::media::ReadStatus::Ok;
        return n;
    }

    [[nodiscard]] kanariv::media::ReadStatus lastReadStatus() const noexcept override {
        return status_;
    }
    [[nodiscard]] kanariv::utils::Expected<void> seek(int64_t offset, int whence) override {
        auto r = inner_->seek(offset, whence);
        if (r) {
            readBytes_ = inner_->tell();
            status_ = kanariv::media::ReadStatus::Ok;
        }
        return r;
    }
    [[nodiscard]] int64_t tell() noexcept override {
        return inner_->tell();
    }
    [[nodiscard]] int64_t size() noexcept override {
        return inner_->size();
    }
    [[nodiscard]] bool seekable() const noexcept override {
        return inner_->seekable();
    }

  private:
    std::unique_ptr<kanariv::media::Reader> inner_;
    int64_t failAfter_{0};
    int64_t readBytes_{0};
    kanariv::media::ReadStatus status_{kanariv::media::ReadStatus::Ok};
};

/// @brief Player whose audio output uses miniaudio's silent null backend, so
/// playback runs (and must not be skipped) under KANARIV_TEST_NOAUDIO=1.
std::unique_ptr<Player> makeSilentPlayer() {
    auto res = Player::create(PlayerOptions{.logger = nullptr, .nullOutput = true});
    REQUIRE(res.has_value());
    return std::move(*res);
}

} // namespace
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
        kanariv::types::TrackRow t;
        t.path = fixturePath("sample.wav");
        t.duration = 2.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0x55 + b);
        auto ins = db->insertTrack(t);
        REQUIRE(ins.has_value());
        REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
        kanariv::types::EngineConfig cfg;
        cfg.enableMonitorThread = false;
        auto eRes = Engine::create(cfg);
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(eng->attachStorage(kanariv::db::makeStorage(
                                       std::shared_ptr<kanariv::db::Database>(std::move(db))))
                    .has_value());
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
    kanariv::types::TrackRow t;
    t.path = fixturePath("sample.wav");
    t.duration = 5.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(0x60 + b);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
    kanariv::types::EngineConfig cfg;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(
                   kanariv::db::makeStorage(std::shared_ptr<kanariv::db::Database>(std::move(db))))
                .has_value());
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

TEST_CASE("control APIs wait for the queue lock", "[player_integration][race]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("control_lock_wait").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::TrackRow t;
    t.path = fixturePath("sample.wav");
    t.duration = 5.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(0xA0 + b);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
    kanariv::types::EngineConfig cfg;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 1;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(
                   kanariv::db::makeStorage(std::shared_ptr<kanariv::db::Database>(std::move(db))))
                .has_value());
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->setRepeat(kanariv::types::RepeatMode::All).has_value());
    // A seeker and the 1 ms monitor thread keep the queue lock hot; control
    // calls must wait for it instead of failing with Busy on a momentary hold.
    std::atomic<bool> run{true};
    std::thread seeker([&eng, &run] {
        while (run.load(std::memory_order_relaxed)) {
            (void)eng->seek(0.5);
            std::this_thread::yield();
        }
    });
    int failed = 0;
    for (int i = 0; i < 100; ++i) {
        const auto repeat =
            i % 2 ? kanariv::types::RepeatMode::All : kanariv::types::RepeatMode::One;
        if (!eng->setRepeat(repeat))
            ++failed;
        if (!eng->setShuffle(i % 2 == 0))
            ++failed;
    }
    run.store(false, std::memory_order_relaxed);
    seeker.join();
    REQUIRE(failed == 0);
    REQUIRE(eng->stop().has_value());
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
        kanariv::types::TrackRow t;
        t.path = fixturePath("sample.wav");
        t.duration = 0.9;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0x70 + i * 16 + b);
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
    REQUIRE(eng->attachStorage(
                   kanariv::db::makeStorage(std::shared_ptr<kanariv::db::Database>(std::move(db))))
                .has_value());
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

TEST_CASE("player stop then play restarts playback", "[player_integration][lifecycle]") {
    auto player = makeSilentPlayer();
    REQUIRE(player->open(fixturePath("sample.wav")).has_value());
    REQUIRE(player->play().has_value());
    REQUIRE(player->state() == State::Playing);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(player->stop().has_value());
    REQUIRE(player->state() == State::Stopped);

    // stop() reset the graph; play() must restart the decode thread instead of
    // leaving a stale Playing state over an idle ring.
    REQUIRE(player->play().has_value());
    REQUIRE(player->state() == State::Playing);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(player->state() == State::Playing);
    REQUIRE(player->position().count() > 0.0);

    REQUIRE(player->stop().has_value());
    REQUIRE(player->state() == State::Stopped);
}

TEST_CASE("player end of stream stops cleanly and play rewinds", "[player_integration][eos]") {
    auto player = makeSilentPlayer();
    REQUIRE(player->open(fixturePath("sample.wav")).has_value());
    REQUIRE(player->seek(0.8).has_value());
    REQUIRE(player->play().has_value());

    busyWaitUntil([&] { return player->state() != State::Playing; },
                  std::chrono::milliseconds(5000), std::chrono::milliseconds(10));
    // A clean end must be Stopped, never Error.
    REQUIRE(player->state() == State::Stopped);

    // End of stream is restartable: play() rewinds to the beginning.
    auto replay = player->play();
    REQUIRE(replay.has_value());
    REQUIRE(player->state() == State::Playing);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(player->state() == State::Playing);

    REQUIRE(player->stop().has_value());
    REQUIRE(player->state() == State::Stopped);
}

TEST_CASE("player decode error reports Error not Stopped", "[player_integration][eos]") {
    auto fileRes = FileReader::open(fixturePath("sample.wav"));
    REQUIRE(fileRes.has_value());
    auto inner = std::move(*fileRes);
    REQUIRE(inner->size() > 160000);

    auto player = makeSilentPlayer();
    // Fail roughly 0.85 s into a 1 s file: after probe + preroll, mid-playback.
    auto reader = std::make_unique<FailingReader>(std::move(inner), 150000);
    REQUIRE(player->openReader(std::move(reader)).has_value());
    REQUIRE(player->play().has_value());

    busyWaitUntil([&] { return player->state() != State::Playing; },
                  std::chrono::milliseconds(5000), std::chrono::milliseconds(10));
    REQUIRE(player->state() == State::Error);

    // Error is not restartable without reopening.
    auto replay = player->play();
    REQUIRE(!replay.has_value());
    REQUIRE(replay.error().code == StatusCode::State);
    REQUIRE(player->state() == State::Error);

    // stop() still resets to a usable state.
    REQUIRE(player->stop().has_value());
    REQUIRE(player->state() == State::Stopped);
}

TEST_CASE("player start seek stop stress", "[player_integration][stress]") {
    // Exercises the control mutex / graph mutex protocol (P1) against the
    // decode thread and the miniaudio (null) callback. Runs headless: the
    // output uses the silent null backend, so no KANARIV_SKIP_IF_NOAUDIO.
    auto player = makeSilentPlayer();
    REQUIRE(player->open(fixturePath("sample.wav")).has_value());
    REQUIRE(player->play().has_value());

    std::atomic<bool> done{false};
    std::thread observer([&] {
        while (!done.load(std::memory_order_relaxed)) {
            (void)player->state();
            (void)player->position();
        }
    });

    for (int i = 0; i < 100; ++i) {
        REQUIRE(player->seek((i % 4) * 0.1).has_value());
        if (i % 3 == 0 && player->state() == State::Playing) {
            REQUIRE(player->pause().has_value());
            REQUIRE(player->resume().has_value());
        }
        if (i % 7 == 0) {
            REQUIRE(player->stop().has_value());
            REQUIRE(player->play().has_value());
        }
        if (i % 20 == 0) {
            // Device swap: replace decoder, ring, and output while the decode
            // thread and the observer are active.
            REQUIRE(player->open(fixturePath("sample.wav")).has_value());
            REQUIRE(player->play().has_value());
        }
    }

    done.store(true, std::memory_order_relaxed);
    observer.join();

    REQUIRE(player->stop().has_value());
    REQUIRE(player->state() == State::Stopped);
}
