/**
 * @file analyze.cpp
 * @brief Explicit ReplayGain 2.0 analysis pass over a library's tracks.
 * @ingroup kanariv_library
 */

#include <cstddef>
#include <db/database_access.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/dsp/loudness.hpp>
#include <kanariv/library/analyze.hpp>
#include <kanariv/media/loudness.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::library {

namespace {

/// Album identity for grouping: album artist when present, otherwise artist.
/// Tracks without an album tag get a key of their own, so their album gain is
/// measured from their own blocks (== their track gain).
std::string groupKey(const kanariv::types::TrackRow& t) {
    if (t.album.empty())
        return "track:" + std::to_string(t.id);
    const std::string& artist = t.album_artist.empty() ? t.artist : t.album_artist;
    std::string key = artist;
    key.push_back('\x1f');
    key.append(t.album);
    return key;
}

bool alreadyAnalyzed(const kanariv::types::TrackRow& t) {
    return t.replayGainTrack != 0.0 && t.replayGainAlbum != 0.0;
}

} // namespace

std::expected<AnalyzeReport, kanariv::utils::Error>
analyzeLibrary(kanariv::db::Database& db, int64_t libraryId, AnalyzeOptions options,
               std::function<void(int64_t, int64_t, std::string_view)> progress) {
    std::optional<kanariv::types::TrackQuery> query;
    if (libraryId != 0) {
        kanariv::types::TrackQuery q;
        q.library_id = libraryId;
        query = std::move(q);
    }
    auto listed = db.listTracks(query);
    if (!listed)
        return std::unexpected{listed.error()};

    AnalyzeReport report;
    report.tracks = static_cast<int64_t>(listed->size());

    // Group by album; std::map keeps the pass order deterministic.
    std::map<std::string, std::vector<const kanariv::types::TrackRow*>> groups;
    for (const auto& t : *listed)
        groups[groupKey(t)].push_back(&t);

    int64_t total = 0;
    for (const auto& [key, members] : groups) {
        bool needsWork = options.reanalyze;
        for (const auto* t : members) {
            if (!alreadyAnalyzed(*t))
                needsWork = true;
        }
        if (needsWork)
            total += static_cast<int64_t>(members.size());
    }

    int64_t done = 0;
    auto* lg = db.logger();
    for (const auto& [key, members] : groups) {
        bool needsWork = options.reanalyze;
        for (const auto* t : members) {
            if (!alreadyAnalyzed(*t))
                needsWork = true;
        }
        if (!needsWork) {
            report.skipped += static_cast<int64_t>(members.size());
            continue;
        }

        struct Measured {
            const kanariv::types::TrackRow* track;
            double trackGainDb{};
        };
        std::vector<Measured> measured;
        std::vector<double> albumEnergies;
        double albumTruePeak = -std::numeric_limits<double>::infinity();
        bool groupComplete = true;

        for (const auto* t : members) {
            auto analysis = kanariv::media::analyzeLoudness(t->path);
            if (!analysis) {
                groupComplete = false;
                report.failed++;
                if (lg)
                    lg->warn("analyze failed track={} err={}", t->path, analysis.error().message);
            } else {
                measured.push_back(Measured{t, analysis->gainDb});
                albumEnergies.insert(albumEnergies.end(), analysis->blockEnergies.begin(),
                                     analysis->blockEnergies.end());
                if (analysis->truePeakDbtp > albumTruePeak)
                    albumTruePeak = analysis->truePeakDbtp;
            }
            ++done;
            if (progress)
                progress(done, total, t->path);
        }

        double albumGainDb = 0.0;
        if (groupComplete) {
            const double albumLufs = kanariv::dsp::loudness::integratedLoudness(albumEnergies);
            albumGainDb = kanariv::dsp::loudness::replayGainDb({albumLufs, albumTruePeak});
        }

        std::unique_lock<std::mutex> lk(kanariv::db::DatabaseAccess::mutex(db));
        for (const auto& m : measured) {
            // An incomplete group keeps whatever album gain its members already
            // had: it will be recomputed once every track of the album decodes.
            const double albumValue = groupComplete ? albumGainDb : m.track->replayGainAlbum;
            auto stored = kanariv::db::DatabaseAccess::setReplayGainLocked(
                db, m.track->id, m.trackGainDb, albumValue);
            if (!stored) {
                report.failed++;
                if (lg)
                    lg->warn("analyze: storing gain failed track={} err={}", m.track->path,
                             stored.error().message);
            } else {
                report.analyzed++;
            }
        }
    }

    if (lg)
        lg->info("analyze finished tracks={} analyzed={} failed={} skipped={}", report.tracks,
                 report.analyzed, report.failed, report.skipped);
    return report;
}

Analyzer::Analyzer(std::shared_ptr<kanariv::db::Database> db) : db_(std::move(db)) {}

std::expected<contracts::LibraryAnalyzeReport, kanariv::utils::Error>
Analyzer::analyzeLibrary(int64_t libraryId, contracts::LibraryAnalyzeOptions options,
                         std::function<void(int64_t, int64_t, std::string_view)> progress) {
    auto report = kanariv::library::analyzeLibrary(
        *db_, libraryId, AnalyzeOptions{options.reanalyze}, std::move(progress));
    if (!report)
        return std::unexpected{report.error()};
    return contracts::LibraryAnalyzeReport{report->tracks, report->analyzed, report->failed,
                                           report->skipped};
}

} // namespace kanariv::library
