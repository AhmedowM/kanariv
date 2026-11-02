#include "service_status.hpp"

#include <blake3.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#endif

#include <kanariv/config.hpp>
#include <kanariv/contracts/database.hpp>
#include <kanariv/engine.hpp>
#include <kanariv/ipc.hpp>
#include <kanariv/player.hpp>
#include <kanariv/utils.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/version.hpp>
#include <kanariv/windows_fwd.hpp>

namespace kanariv::service::detail {
std::expected<kanariv::ipc::Status, kanariv::utils::Error>
buildStatus(kanariv::engine::Engine& eng, kanariv::contracts::IDatabase& db) {
    kanariv::ipc::Status s{};
    s.version = std::string(kanariv::version::version());
    s.state = eng.state();
    s.pos = eng.position();
    s.dur = eng.duration();
    s.vol = eng.volume();
    s.muted = eng.volume() == 0.0f;
    s.shuffle = eng.shuffle();
    s.repeat = eng.repeat();
    s.track_id = eng.currentTrackId();
    if (s.track_id != 0) {
        auto tr = db.getTrack(s.track_id);
        if (tr) {
            s.title = tr->title;
            s.artist = tr->artist;
            s.path = tr->path;
        }
    }
    try {
        int64_t activeQ = eng.activeQueueId();
        // O(1) size via COUNT(*) + O(log n) position lookup (C-3): the old
        // code materialized the full queue vector and scanned it linearly
        // on every status call (including --watch each second).
        s.q_size = db.queueCount(activeQ);
        s.q_idx = 0;
        if (s.track_id != 0) {
            if (auto idx = db.queueIndexOf(activeQ, s.track_id); idx)
                s.q_idx = *idx;
        }
    } catch (...) {
    }
    return s;
}

} // namespace kanariv::service::detail
