#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using namespace kanariv::player;
using namespace kanariv::db;
using namespace kanariv::utils;
using kanariv::media::Decoder;
using kanariv::media::FileReader;

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

std::expected<std::unique_ptr<Database>, Error> openDbWithFallback(const std::string& path,
                                                                   std::string& usedPath) {
    auto r = Database::open(path);
    if (r) {
        usedPath = path;
        return r;
    }
    std::cerr << "[demo] Database::open('" << path << "') failed: " << r.error().message
              << ", fallback :memory:\n";
    auto m = Database::open(":memory:");
    if (m) {
        usedPath = ":memory:";
        std::cout << "[demo] using :memory: DB\n";
    }
    return m;
}

void ensureQueueHasTracks(Database& db, const std::string& samplePath, const std::string& prefix,
                          uint8_t fpBase) {
    auto q = db.listQueueItems(1);
    if (q && !q->empty())
        return;
    std::cout << "[demo] queue empty, populating 2 demo tracks sample='" << samplePath << "'\n";
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(fpBase + i * 32 + b);
        t.path = samplePath;
        t.size = 176444;
        t.mtime = 1700000000 + i;
        t.duration = 1.0;
        t.sample_rate = 44100;
        t.channels = 1;
        t.bitrate = 128;
        t.title = prefix + " Track " + std::to_string(i + 1);
        t.artist = "kanariv demo";
        t.album = prefix;
        t.genre = "Demo";
        t.year = 2026;
        t.track_num = i + 1;
        t.library_id = 1;
        auto ins = db.insertTrack(t);
        int64_t tid = 0;
        if (!ins) {
            if (ins.error().code == StatusCode::AlreadyExists) {
                auto ex = db.findByFingerprint(t.fingerprint);
                if (ex)
                    tid = ex->id;
                else {
                    std::cerr << "[demo] duplicate but find failed\n";
                    continue;
                }
            } else {
                std::cerr << "[demo] insertTrack " << i << " failed: " << ins.error().message
                          << "\n";
                continue;
            }
        } else {
            tid = *ins;
        }
        auto eq = db.queueEnqueue(1, tid, -1);
        if (!eq)
            std::cerr << "[demo] queueEnqueue tid=" << tid << " failed: " << eq.error().message
                      << "\n";
        else
            std::cout << "[demo] enqueued tid=" << tid << "\n";
    }
}

bool playTrackViaPlayer(const kanariv::types::Track& track) {
    std::cout << "[demo] queue track " << track.id << ": '"
              << (track.title.empty() ? "(untitled)" : track.title) << "' by '"
              << (track.artist.empty() ? "(unknown)" : track.artist) << "' path='" << track.path
              << "'\n";

    auto readerResult = FileReader::open(track.path);
    if (!readerResult) {
        std::cerr << "[demo] FileReader::open('" << track.path
                  << "') failed: " << readerResult.error().message
                  << " -- using synthetic fallback\n";
        // Fallback: still exercise decoder with MemoryReader empty data via synthetic path
        // Create a tiny in-memory wav header synthetic fallback: just use decoder fallback directly
        // For demo, skip if file missing but count as error
        return false;
    }
    auto reader = std::move(readerResult.value());
    auto decResult = Decoder::open(*reader);
    if (!decResult) {
        std::cerr << "[demo] Decoder::open failed: " << decResult.error().message << "\n";
        return false;
    }
    auto decoder = std::move(decResult.value());
    std::cout << "  decoder: " << decoder->sampleRate() << " Hz, " << decoder->channels()
              << " ch, totalFrames=" << decoder->totalFrames() << "\n";

    SpscRing<float> ring(8192 * decoder->channels(), decoder->channels());
    AudioOutput::Config cfg;
    cfg.sampleRate = decoder->sampleRate();
    cfg.channels = decoder->channels();
    cfg.ring = &ring;
    auto outRes = AudioOutput::create(cfg);
    if (!outRes) {
        std::cerr << "[demo] AudioOutput::create failed: " << outRes.error().message << "\n";
        return false;
    }
    auto output = std::move(outRes.value());
    output->start();

    std::vector<float> tmp(1024 * decoder->channels());
    int ticks = 0;
    const int maxTicks = 500;
    bool decodingDone = false;
    int framesPlayed = 0;
    while (ticks < maxTicks) {
        if (!decodingDone && ring.availableWrite() * decoder->channels() >= tmp.size()) {
            std::size_t frames = decoder->decode(std::span<float>(tmp.data(), tmp.size()));
            if (frames == 0)
                decodingDone = true;
            else {
                ring.write(std::span<const float>(tmp.data(), frames * decoder->channels()));
                framesPlayed += static_cast<int>(frames);
            }
        }
        // If decoding done and ring drained, we're finished
        if (decodingDone && ring.availableRead() == 0)
            break;
        // If output stopped? ring empty but decoder reached EOF; AudioOutput will be draining
        // fallback sine
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++ticks;
    }
    if (ticks >= maxTicks) {
        std::cerr << "[demo] playback timeout for '" << track.path << "', stopping\n";
    } else {
        std::cout << "[demo] finished '" << (track.title.empty() ? track.path : track.title)
                  << "' in " << ticks << " ticks\n";
    }
    output->stop();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string dbPath = "library.db";
    if (argc > 1 && argv[1] && argv[1][0] != '\0')
        dbPath = argv[1];

    std::string usedPath;
    auto dbRes = openDbWithFallback(dbPath, usedPath);
    if (!dbRes) {
        std::cerr << "[demo] open failed: " << dbRes.error().message
                  << " code=" << static_cast<int>(dbRes.error().code) << "\n";
        return 1;
    }
    auto db = std::move(dbRes.value());
    std::cout << "[demo] opening DB: " << usedPath << "\n";

    auto countRes = db->listQueueItems(1);
    int count = countRes ? static_cast<int>(countRes->size()) : 0;
    std::cout << "[demo] queue 1 has " << count << " item(s)\n";

    if (count == 0) {
        ensureQueueHasTracks(*db, findSample(), "player_db_demo", 0xA0);
        auto c2 = db->listQueueItems(1);
        count = c2 ? static_cast<int>(c2->size()) : 0;
        std::cout << "[demo] after populate queue has " << count << " item(s)\n";
    }

    if (count == 0) {
        std::cout << "[demo] queue empty, nothing to play (graceful exit)\n";
        return 0;
    }

    auto itemsRes = db->listQueueItems(1);
    if (!itemsRes) {
        std::cerr << "[demo] listQueueItems failed: " << itemsRes.error().message << "\n";
        return 1;
    }

    int played = 0, errors = 0;
    for (auto& qi : *itemsRes) {
        auto tr = db->getTrack(qi.track_id);
        if (!tr) {
            std::cerr << "[demo] getTrack(" << qi.track_id << ") failed: " << tr.error().message
                      << "\n";
            ++errors;
            continue;
        }
        kanariv::types::Track track = tr.value();
        std::cout << "[demo] queue pos " << qi.position << " -> track " << track.id << "\n";
        if (playTrackViaPlayer(track))
            ++played;
        else
            ++errors;
    }
    std::cout << "[demo] done: played=" << played << " errors=" << errors << "\n";
    return 0;
}
