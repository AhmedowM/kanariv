module;
#include <kanariv/media/scan.hpp>

export module kanariv.media:scan;

export namespace kanariv::media {
using ::kanariv::media::ScanIssue;
using ::kanariv::media::ScanIssueKind;
using ::kanariv::media::ScanIssueSink;
using ::kanariv::media::ScanMode;
using ::kanariv::media::computeLoudnessGain;
using ::kanariv::media::scanTracks;
using ::kanariv::media::scanTracksToVector;
} // namespace kanariv::media
