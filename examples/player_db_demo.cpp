#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <kanariv/db/database.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
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

constexpr int demoTracksCount = 2;
constexpr int fingerprintSize = 32;
constexpr int demoTrackSize = 176444;
constexpr int demoMtimeBase = 1700000000;
constexpr int demoSampleRate = 44100;
constexpr int demoChannels = 1;
constexpr int demoBitrate = 128;
constexpr int demoYear = 2026;
constexpr int demoLibraryId = 1;
constexpr int defaultQueueId = 1;
constexpr int defaultPosition = -1;
constexpr int defaultRingFramesPerChannel = 8192;
constexpr int decodeTmpMultiplier = 1024;
constexpr int decodeMaxTicks = 500;
constexpr int pollSleepMs = 10;
constexpr uint8_t defaultFpBase = 0xA0;
constexpr int exitCodeSuccess = 0;
constexpr int exitCodeFailure = 1;

auto findSample() -> std::string {
    auto toAbs = [](std::string s) -> std::string {
        try {
            return std::filesystem::absolute(s).string();
        } catch (...) {
            return s;
        }
    };
    if (auto* env = std::getenv("KANARIV_SAMPLE")) {
        if (std::filesystem::exists(env)) {
            return toAbs(env);
        }
    }
    if (auto* env = std::getenv("KANARIV_FIXTURE")) {
        if (std::filesystem::exists(env)) {
            return toAbs(env);
        }
    }
    for (const auto* c : {"tests/fixtures/sample.wav", "./tests/fixtures/sample.wav",
                          "../tests/fixtures/sample.wav",
                          "C:/Users/Secondary/Projects/kanariv-cpp/tests/fixtures/sample.wav",
                          "C:/Users/Secondary/Projects/kanariv/tests/fixtures/sample.wav"}) {
        if (std::filesystem::exists(c)) {
            return toAbs(c);
        }
    }
    if (auto* env = std::getenv("KANARIV_SAMPLE")) {
        return toAbs(env);
    }
    if (std::filesystem::exists(
            "C:/Users/Secondary/Projects/kanariv-cpp/tests/fixtures/sample.wav")) {
        return "C:/Users/Secondary/Projects/kanariv-cpp/tests/fixtures/sample.wav";
    }
    return "tests/fixtures/sample.wav";
}

auto openDbWithFallback(const std::string& path, std::string& usedPath)
    -> std::expected<std::unique_ptr<Database>, Error> {
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
    if (q && !q->empty()) {
        return;
    }
    std::cout << "[demo] queue empty, populating 2 demo tracks sample='" << samplePath << "'\n";
    for (int i = 0; i < demoTracksCount; ++i) {
        kanariv::types::Track t;
        for (int b = 0; b < fingerprintSize; ++b) {
            t.fingerprint.at(b) = static_cast<uint8_t>(fpBase + i * fingerprintSize + b);
        }
        t.path = samplePath;
        t.size = demoTrackSize;
        t.mtime = demoMtimeBase + i;
        t.duration = 1.0;
        t.sample_rate = demoSampleRate;
        t.channels = demoChannels;
        t.bitrate = demoBitrate;
        t.title = prefix + " Track " + std::to_string(i + 1);
        t.artist = "kanariv demo";
        t.album = prefix;
        t.genre = "Demo";
        t.year = demoYear;
        t.track_num = i + 1;
        t.library_id = demoLibraryId;
        auto ins = db.insertTrack(t);
        int64_t tid = 0;
        if (!ins) {
            if (ins.error().code == StatusCode::AlreadyExists) {
                auto ex = db.findByFingerprint(t.fingerprint);
                if (ex) {
                    tid = ex->id;
                } else {
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
        auto eq = db.queueEnqueue(defaultQueueId, tid, defaultPosition);
        if (!eq) {
            std::cerr << "[demo] queueEnqueue tid=" << tid << " failed: " << eq.error().message
                      << "\n";
        } else {
            std::cout << "[demo] enqueued tid=" << tid << "\n";
        }
    }
}

auto playTrackViaPlayer(const kanariv::types::Track& track) -> bool {
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

    SpscRing<float> ring(defaultRingFramesPerChannel * decoder->channels(), decoder->channels());
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

    std::vector<float> tmp(decodeTmpMultiplier * decoder->channels());
    int ticks = 0;
    const int maxTicks = decodeMaxTicks;
    bool decodingDone = false;
    int framesPlayed = 0;
    while (ticks < maxTicks) {
        if (!decodingDone && ring.availableWrite() * decoder->channels() >= tmp.size()) {
            std::size_t frames = decoder->decode(std::span<float>(tmp.data(), tmp.size()));
            if (frames == 0) {
                decodingDone = true;
            } else {
                ring.write(std::span<const float>(tmp.data(), frames * decoder->channels()));
                framesPlayed += static_cast<int>(frames);
            }
        }
        // If decoding done and ring drained, we're finished
        if (decodingDone && ring.availableRead() == 0) {
            break;
        }
        // If output stopped? ring empty but decoder reached EOF; AudioOutput will be draining
        // fallback sine
        std::this_thread::sleep_for(std::chrono::milliseconds(pollSleepMs));
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

auto main(int argc, char** argv) -> int {
    std::string dbPath = "library.db";
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        dbPath = argv[1];
    }

    std::string usedPath;
    auto dbRes = openDbWithFallback(dbPath, usedPath);
    if (!dbRes) {
        std::cerr << "[demo] open failed: " << dbRes.error().message
                  << " code=" << static_cast<int>(dbRes.error().code) << "\n";
        return exitCodeFailure;
    }
    auto db = std::move(dbRes.value());
    std::cout << "[demo] opening DB: " << usedPath << "\n";

    auto countRes = db->listQueueItems(defaultQueueId);
    int count = countRes ? static_cast<int>(countRes->size()) : 0;
    std::cout << "[demo] queue " << defaultQueueId << " has " << count << " item(s)\n";

    if (count == 0) {
        ensureQueueHasTracks(*db, findSample(), "player_db_demo", defaultFpBase);
        auto c2 = db->listQueueItems(defaultQueueId);
        count = c2 ? static_cast<int>(c2->size()) : 0;
        std::cout << "[demo] after populate queue has " << count << " item(s)\n";
    }

    if (count == 0) {
        std::cout << "[demo] queue empty, nothing to play (graceful exit)\n";
        return exitCodeSuccess;
    }

    auto itemsRes = db->listQueueItems(defaultQueueId);
    if (!itemsRes) {
        std::cerr << "[demo] listQueueItems failed: " << itemsRes.error().message << "\n";
        return exitCodeFailure;
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
        const kanariv::types::Track& track = tr.value();
        std::cout << "[demo] queue pos " << qi.position << " -> track " << track.id << "\n";
        if (playTrackViaPlayer(track)) {
            ++played;
        } else {
            ++errors;
        }
    }
    std::cout << "[demo] done: played=" << played << " errors=" << errors << "\n";
    return exitCodeSuccess;
}
