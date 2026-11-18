#include <blake3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/fingerprint.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/generator.hpp>
#include <media/scan_walker.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

namespace kanariv::media::internal {

namespace {

using kanariv::utils::Error;
using kanariv::utils::makeError;
using kanariv::utils::StatusCode;

/// Full-file BLAKE3: the `ScanMode::Full` counterpart of `computeFingerprint`.
std::expected<std::array<uint8_t, 32>, Error>
fullFileFingerprint(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::unexpected{makeError(StatusCode::Io, "cannot open file for hashing")};
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<std::byte, 8192> buf{};
    while (f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size())) ||
           f.gcount())
        blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(f.gcount()));
    // A read error mid-file would hash a prefix and yield a digest that looks
    // valid; report it instead of persisting a wrong fingerprint.
    if (f.bad())
        return std::unexpected{makeError(StatusCode::Io, "read error while hashing")};
    std::array<uint8_t, 32> out{};
    blake3_hasher_finalize(&hasher, out.data(), out.size());
    return out;
}

FingerprintProvider defaultFingerprintProvider(ScanMode mode) {
    if (mode == ScanMode::Full)
        return
            [](const std::filesystem::path& path, ScanMode) { return fullFileFingerprint(path); };
    return [](const std::filesystem::path& path, ScanMode) { return computeFingerprint(path); };
}

/// Copies the tags FFmpeg can supply into `t`; a metadata failure is not fatal
/// (the file is still usable without tags), so it leaves them empty.
void applyMetadata(kanariv::types::TrackRow& t) {
    auto meta = kanariv::media::extractMetadata(t.path);
    if (!meta)
        return;
    t.title = std::move(meta->title);
    t.artist = std::move(meta->artist);
    t.album = std::move(meta->album);
    t.album_artist = std::move(meta->album_artist);
    t.genre = std::move(meta->genre);
    t.year = meta->year;
    t.track_num = meta->track_num;
    t.disc_num = meta->disc_num;
    t.duration = meta->duration;
    t.sample_rate = static_cast<uint32_t>(meta->sample_rate);
    t.channels = static_cast<uint32_t>(meta->channels);
    t.bitrate = meta->bitrate;
}

/// Builds a TrackRow for one audio file. Never returns a TrackRow without a usable
/// fingerprint: a hashing failure (or the all-zero sentinel) is an error.
std::expected<kanariv::types::TrackRow, Error> buildTrack(const std::filesystem::path& path,
                                                       ScanMode mode,
                                                       const FingerprintProvider& fingerprint) {
    kanariv::types::TrackRow t;
    // UTF-8, not generic_string(): the latter throws on Windows for characters
    // outside the ANSI code page, which aborted the whole scan at the first
    // such file (and surfaced as a broken pipe).
    t.path = kanariv::utils::detail::pathToUtf8(path);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec)
        t.size = static_cast<int64_t>(size);
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec)
        t.mtime = static_cast<int64_t>(mtime.time_since_epoch().count());

    auto fp = fingerprint(path, mode);
    if (!fp) {
        return std::unexpected{
            makeError(StatusCode::Io, "fingerprint failed: " + fp.error().message)};
    }
    if (isZeroFingerprint(*fp))
        return std::unexpected{makeError(StatusCode::Io, "fingerprint is the all-zero sentinel")};
    t.fingerprint = *fp;

    applyMetadata(t);
    return t;
}

} // namespace

struct ScanWalker::Impl {
    std::filesystem::path root;
    ScanMode mode{ScanMode::Sampled};
    ScanIssueSink onIssue;
    FingerprintProvider fingerprint;
    std::filesystem::recursive_directory_iterator it;
    std::filesystem::recursive_directory_iterator end;
    bool opened{false};
    std::optional<kanariv::utils::Error> walkError;

    void report(ScanIssueKind kind, std::string path, std::string message) {
        if (onIssue)
            onIssue(ScanIssue{kind, std::move(path), std::move(message)});
    }

    void failWalk(const std::filesystem::path& where, const std::string& cause) {
        const std::string whereUtf8 = kanariv::utils::detail::pathToUtf8(where);
        auto error =
            makeError(StatusCode::Io, "directory walk failed at '" + whereUtf8 + "': " + cause);
        report(ScanIssueKind::WalkError, whereUtf8, error.message);
        walkError = std::move(error);
        // Standard-library contract: an iterator whose increment failed is
        // end, but be explicit so the loop cannot resume on stale state.
        it = end;
    }
};

ScanWalker::ScanWalker(std::filesystem::path root, ScanMode mode, ScanIssueSink onIssue,
                       FingerprintProvider fingerprint)
    : impl_(std::make_unique<Impl>()) {
    impl_->root = std::move(root);
    impl_->mode = mode;
    impl_->onIssue = std::move(onIssue);
    impl_->fingerprint = fingerprint ? std::move(fingerprint) : defaultFingerprintProvider(mode);
}

ScanWalker::~ScanWalker() = default;

std::expected<void, kanariv::utils::Error> ScanWalker::open() {
    if (!impl_)
        return std::unexpected{makeError(StatusCode::Internal, "moved-from walker")};
    std::error_code ec;
    if (!std::filesystem::exists(impl_->root, ec)) {
        impl_->report(ScanIssueKind::WalkError, kanariv::utils::detail::pathToUtf8(impl_->root),
                      "scan root does not exist");
        return std::unexpected{
            makeError(StatusCode::NotFound, "scan root does not exist: " +
                                                kanariv::utils::detail::pathToUtf8(impl_->root))};
    }
    // The constructor's error_code is checked explicitly: without it a
    // permission or I/O problem would look like an empty directory.
    impl_->it = std::filesystem::recursive_directory_iterator(
        impl_->root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        const std::string cause = ec.message();
        impl_->failWalk(impl_->root, cause);
        return std::unexpected{makeError(
            StatusCode::Io, "cannot open scan root '" +
                                kanariv::utils::detail::pathToUtf8(impl_->root) + "': " + cause)};
    }
    impl_->opened = true;
    return {};
}

std::optional<kanariv::types::TrackRow> ScanWalker::next() {
    if (!impl_ || !impl_->opened)
        return std::nullopt;
    while (impl_->it != impl_->end) {
        // Copy the path: the iterator is invalidated by increment() below,
        // which must happen before any return so the next call continues past
        // this entry instead of re-processing it forever.
        const auto path = impl_->it->path();
        std::error_code entryEc;
        const bool regular = impl_->it->is_regular_file(entryEc);
        std::optional<kanariv::types::TrackRow> result;
        if (entryEc) {
            impl_->report(ScanIssueKind::FileSkipped, kanariv::utils::detail::pathToUtf8(path),
                          entryEc.message());
        } else if (regular && kanariv::media::hasAudioExt(path)) {
            if (auto track = buildTrack(path, impl_->mode, impl_->fingerprint))
                result = std::move(*track);
            else
                impl_->report(ScanIssueKind::FileSkipped, kanariv::utils::detail::pathToUtf8(path),
                              track.error().message);
        }
        std::error_code stepEc;
        impl_->it.increment(stepEc);
        if (stepEc) {
            impl_->failWalk(path, stepEc.message());
            return std::nullopt;
        }
        if (result)
            return result;
    }
    return std::nullopt;
}

const std::optional<kanariv::utils::Error>& ScanWalker::walkError() const noexcept {
    static const std::optional<kanariv::utils::Error> none{};
    return impl_ ? impl_->walkError : none;
}

} // namespace kanariv::media::internal

namespace kanariv::media {

kanariv::utils::Generator<kanariv::types::TrackRow> scanTracks(const std::filesystem::path& root,
                                                               ScanMode mode) {
    return scanTracks(root, mode, ScanIssueSink{});
}

kanariv::utils::Generator<kanariv::types::TrackRow>
scanTracks(const std::filesystem::path& root, ScanMode mode, ScanIssueSink onIssue) {
    // The sink is taken by value: a coroutine body runs only once iteration
    // starts, so a reference to a caller temporary could dangle by then.
    internal::ScanWalker walker(root, mode, std::move(onIssue));
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        co_return;
    if (auto opened = walker.open(); !opened)
        co_return;
    while (auto track = walker.next())
        co_yield std::move(*track);
}

std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode) {
    return scanTracksToVector(root, mode, ScanIssueSink{});
}

std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode, ScanIssueSink onIssue) {
    internal::ScanWalker walker(root, mode, std::move(onIssue));
    if (auto opened = walker.open(); !opened)
        return std::unexpected{opened.error()};
    std::vector<kanariv::types::TrackRow> out;
    while (auto track = walker.next())
        out.push_back(std::move(*track));
    // Discard the partial vector: a truncated walk must not look like a
    // complete (smaller) library.
    if (const auto& walkError = walker.walkError())
        return std::unexpected{*walkError};
    return out;
}

} // namespace kanariv::media
