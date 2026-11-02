#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <array>
#include <thread>
#include <utility>
#include <vector>
using namespace kanariv::engine;
using namespace kanariv::db;

namespace {

std::string findSample() {
    auto toAbs = [](std::string s) -> std::string {
        try {
            return std::filesystem::absolute(s).string();
        } catch (...) {
            return s;
        }
    };
    if (auto* env = std::getenv("KANARIV_SAMPLE")) {
        if (std::filesystem::exists(env))
            return toAbs(env);
    }
    if (auto* env = std::getenv("KANARIV_FIXTURE")) {
        if (std::filesystem::exists(env))
            return toAbs(env);
    }
    for (auto* c : {"tests/fixtures/sample.wav", "./tests/fixtures/sample.wav",
                    "../tests/fixtures/sample.wav",
                    "C:/Users/Secondary/Projects/kanariv-cpp/tests/fixtures/sample.wav",
                    "C:/Users/Secondary/Projects/kanariv/tests/fixtures/sample.wav"}) {
        if (std::filesystem::exists(c))
            return toAbs(c);
    }
    if (auto* env = std::getenv("KANARIV_SAMPLE"))
        return toAbs(env);
    if (std::filesystem::exists("C:/Users/Secondary/Projects/kanariv-cpp/tests/fixtures/sample.wav"))
        return "C:/Users/Secondary/Projects/kanariv-cpp/tests/fixtures/sample.wav";
    return "tests/fixtures/sample.wav";
}

std::expected<std::unique_ptr<Database>, kanariv::utils::Error>
openDbWithFallback(const std::string& path, std::string& used) {
    auto r = Database::open(path);
    if (r) {
        used = path;
        return r;
    }
    std::cerr << "[engine_demo] Database::open('" << path << "') failed: " << r.error().message
              << ", fallback :memory:\n";
    auto m = Database::open(":memory:");
    if (m) {
        used = ":memory:";
        std::cout << "[engine_demo] using :memory: DB\n";
    }
    return m;
}

void ensureQueueHasTracks(Database& db, const std::string& samplePath) {
    auto q = db.listQueueItems(1);
    if (q && !q->empty())
        return;
    std::cout << "[engine_demo] queue empty, populating 2 demo tracks sample='" << samplePath
              << "'\n";
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0xC0 + i * 32 + b);
        t.path = samplePath;
        t.size = 176444;
        t.mtime = 1700000000 + i;
        t.duration = 1.0;
        t.sample_rate = 44100;
        t.channels = 1;
        t.bitrate = 128;
        t.title = "engine_demo Track " + std::to_string(i + 1);
        t.artist = "kanariv demo";
        t.album = "engine_demo";
        t.genre = "Demo";
        t.year = 2026;
        t.track_num = i + 1;
        t.library_id = 1;
        auto ins = db.insertTrack(t);
        int64_t tid = 0;
        if (!ins) {
            if (ins.error().code == kanariv::utils::StatusCode::AlreadyExists) {
                auto ex = db.findByFingerprint(t.fingerprint);
                if (ex)
                    tid = ex->id;
                else {
                    std::cerr << "[engine_demo] duplicate but find failed\n";
                    continue;
                }
            } else {
                std::cerr << "[engine_demo] insert failed: " << ins.error().message << "\n";
                continue;
            }
        } else
            tid = *ins;
        auto eq = db.queueEnqueue(1, tid, -1);
        if (!eq)
            std::cerr << "[engine_demo] enqueue tid=" << tid << " failed: " << eq.error().message
                      << "\n";
        else
            std::cout << "[engine_demo] enqueued tid=" << tid << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string dbPath = "build/engine_demo.db";
    if (argc > 1 && argv[1] && argv[1][0] != '\0')
        dbPath = argv[1];

    std::string used;
    auto dbRes = openDbWithFallback(dbPath, used);
    if (!dbRes) {
        std::cerr << "[engine_demo] open failed: " << dbRes.error().message << "\n";
        return 1;
    }
    // The caller owns the backend and attaches it to the Engine.
    auto dbShared = std::shared_ptr<Database>(std::move(dbRes.value()));
    std::cout << "[engine_demo] opening DB: " << used << "\n";

    auto q1 = dbShared->listQueueItems(1);
    int count = q1 ? static_cast<int>(q1->size()) : 0;
    std::cout << "[engine_demo] queue 1 has " << count << " item(s)\n";
    if (count == 0) {
        ensureQueueHasTracks(*dbShared, findSample());
        auto q2 = dbShared->listQueueItems(1);
        count = q2 ? static_cast<int>(q2->size()) : 0;
        std::cout << "[engine_demo] after populate queue has " << count << " item(s)\n";
    }
    if (count == 0) {
        std::cout << "[engine_demo] queue empty, nothing to play\n";
        return 0;
    }

    kanariv::types::EngineConfig cfg;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.enableMonitorThread = true;
    cfg.historyThresholdPct = 60;
    cfg.historyThresholdSecs = 90;
    kanariv::types::EngineCallbacks cbs;
    cbs.on_track_started = [](int64_t tid) {
        std::cout << "[engine_demo] callback onTrackStarted track " << tid << "\n";
    };
    cbs.on_queue_changed = [](int64_t qid) {
        std::cout << "[engine_demo] callback onQueueChanged queue " << qid << "\n";
    };
    cbs.on_track_ended = [](int64_t tid, double pct) {
        std::cout << "[engine_demo] callback onTrackEnded track " << tid << " pct=" << pct << "\n";
    };
    cbs.on_error = [](kanariv::utils::StatusCode r, std::string_view msg) {
        std::cout << "[engine_demo] callback onError " << static_cast<int>(r) << " " << msg << "\n";
    };
    cfg.callbacks = cbs;

    // Create the Engine independently from the selected database backend.
    auto engineRes = Engine::create(cfg);
    if (!engineRes) {
        std::cerr << "[engine_demo] Engine::create failed: " << engineRes.error().message << "\n";
        return 1;
    }
    auto engine = std::move(engineRes.value());
    auto att = engine->attachDatabase(dbShared);
    if (!att) {
        std::cerr << "[engine_demo] attachDatabase failed: " << att.error().message << "\n";
        return 1;
    }

    auto pr = engine->play(1);
    if (!pr) {
        std::cerr << "[engine_demo] play failed: " << pr.error().message << " ("
                  << static_cast<int>(pr.error().code) << ")\n";
    } else {
        std::cout << "[engine_demo] playing queue 1, track " << engine->currentTrackId() << "\n";
    }

    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (true) {
            auto ev = engine->pollEvent();
            if (!ev)
                break;
            std::cout << "[engine_demo] event type=" << static_cast<int>(ev->type) << " track "
                      << ev->track_id << " queue " << ev->queue_id << " pos=" << ev->position
                      << " dur=" << ev->duration << " msg='" << ev->msg << "'\n";
        }
        if (i == 10) {
            std::array<kanariv::types::EngineEvent, 64> buf;
            size_t n = 0;
            (void)engine->drainEvents(buf.data(), 64, &n);
            if (n > 0) {
                std::cout << "[engine_demo] drain got " << n << " events at tick " << i << "\n";
                for (size_t k = 0; k < n; ++k) {
                    std::cout << "  drain[" << k << "] type=" << static_cast<int>(buf[k].type)
                              << " track " << buf[k].track_id << "\n";
                }
            }
        }
    }
    std::array<kanariv::types::EngineEvent, 64> buf;
    size_t n = 0;
    (void)engine->drainEvents(buf.data(), 64, &n);
    std::cout << "[engine_demo] final drain " << n << " events\n";
    for (size_t k = 0; k < n; ++k) {
        std::cout << "  final[" << k << "] type=" << static_cast<int>(buf[k].type) << " track "
                  << buf[k].track_id << " queue " << buf[k].queue_id << "\n";
    }
    std::cout << "[engine_demo] current track " << engine->currentTrackId() << " state "
              << static_cast<int>(engine->state()) << " pos " << engine->position() << "\n";
    engine->shutdown();
    std::cout << "[engine_demo] done\n";
    return 0;
}
