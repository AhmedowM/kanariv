#pragma once

/**
 * @file engine_access.hpp
 * @brief Single friendship surface for `Engine`'s service-only members.
 * @ingroup kanariv_engine
 * @details `Engine` keeps `suspendCurrentTrack`, `resumeAfterTagWrite`,
 * `preMuteVolume`, `setPreMuteVolume`, `shufflePermFor` and `noteEnqueued`
 * private: they exist for the daemon's tag-write and queue bookkeeping, not
 * for the public playback API. This struct is the sole `friend` of `Engine`
 * and exposes static forwarders so `src/service` can reach those members
 * without befriending individual functions or classes.
 *
 * Every forwarder only delegates; the locking contract stays with `Engine`.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <kanariv/engine/engine.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kanariv::engine::detail {

/**
 * @brief Static forwarders to the private members of `Engine`.
 * @ingroup kanariv_engine
 * @details Sole friend of `Engine`. Each function forwards to the matching
 * private member one-for-one.
 */
struct ServiceAccess {
    /**
     * @brief Forwards to Engine::suspendCurrentTrack().
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @return Success or Error State/Busy.
     */
    static Engine::ExpectedVoid suspendCurrentTrack(Engine& eng) {
        return eng.suspendCurrentTrack();
    }

    /**
     * @brief Forwards to Engine::resumeAfterTagWrite().
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param t Track row to open (usually re-registered after a tag write).
     * @param pos Seconds to resume from (clamped to duration).
     * @param wasPaused Restore Paused instead of Playing when true.
     * @return Success or Error Busy/NotFound/Device/Internal.
     */
    static Engine::ExpectedVoid resumeAfterTagWrite(Engine& eng, const kanariv::types::Track& t,
                                                    double pos, bool wasPaused) {
        return eng.resumeAfterTagWrite(t, pos, wasPaused);
    }

    /**
     * @brief Forwards to Engine::preMuteVolume().
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @return Restore level (atomic load, relaxed).
     */
    static float preMuteVolume(Engine& eng) noexcept {
        return eng.preMuteVolume();
    }

    /**
     * @brief Forwards to Engine::setPreMuteVolume().
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param g Level in (0,1]; anything else becomes 0.5.
     * @return Success (always).
     */
    static Engine::ExpectedVoid setPreMuteVolume(Engine& eng, float g) {
        return eng.setPreMuteVolume(g);
    }

    /**
     * @brief Forwards to Engine::shufflePermFor().
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param qid Queue id.
     * @return Perm copy when shuffle applies to qid, else empty.
     */
    static std::vector<int64_t> shufflePermFor(Engine& eng, int64_t qid) noexcept {
        return eng.shufflePermFor(qid);
    }

    /**
     * @brief Forwards to Engine::noteEnqueued().
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param qid Queue id the tracks were appended to.
     * @param count Number of appended tracks (at the queue end).
     */
    static void noteEnqueued(Engine& eng, int64_t qid, std::size_t count) noexcept {
        eng.noteEnqueued(qid, count);
    }
};

} // namespace kanariv::engine::detail
