#pragma once

/**
 * @file engine_access.hpp
 * @brief Single friendship surface for `Engine`'s service-only operations.
 * @ingroup kanariv_engine
 * @details The tag-write and queue bookkeeping entry points stay off the
 * public class: they exist for the daemon, not for playback clients.
 * `EngineAccess` is the sole `friend` of `Engine` and reaches its `Impl`
 * directly, so none of those operations appear in the installed header.
 *
 * The forwarders are defined out-of-line in the engine component because the
 * return values must cross the DLL boundary in shared builds where
 * `Engine::Impl` is not exported. Every forwarder delegates one-for-one; the
 * locking contract stays with `Engine::Impl`.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <cstddef>
#include <cstdint>
#include <kanariv/engine/engine.hpp>
#include <vector>

namespace kanariv::engine::detail {

/**
 * @brief Static forwarders to the service-only operations of `Engine`.
 * @ingroup kanariv_engine
 * @details Sole friend of `Engine`. Each function forwards to the matching
 * `Engine::Impl` member one-for-one.
 */
struct EngineAccess {
    /**
     * @brief Suspends the loaded track and releases its file handles.
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @return Success or Error State/Busy.
     * @see Engine::Impl::suspendCurrentTrack
     */
    KANARIV_API static Engine::ExpectedVoid suspendCurrentTrack(Engine& eng);

    /**
     * @brief Reopens the suspended track at a captured position.
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param t TrackRow row to open (usually re-registered after a tag write).
     * @param pos Seconds to resume from (clamped to duration).
     * @param wasPaused Restore Paused instead of Playing when true.
     * @return Success or Error Busy/NotFound/Device/Internal.
     * @see Engine::Impl::resumeAfterTagWrite
     */
    KANARIV_API static Engine::ExpectedVoid
    resumeAfterTagWrite(Engine& eng, const kanariv::types::TrackRow& t, double pos, bool wasPaused);

    /**
     * @brief Returns the unmute restore level.
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @return Last non-zero level before mute, in (0,1].
     */
    KANARIV_API static float preMuteVolume(const Engine& eng) noexcept;

    /**
     * @brief Sets the unmute restore level (sanitized; persisted on next save).
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param g Level in (0,1]; anything else becomes 0.5.
     * @return Success or Error.
     */
    KANARIV_API static Engine::ExpectedVoid setPreMuteVolume(Engine& eng, float g);

    /**
     * @brief Returns the shuffle permutation (positions) for a queue.
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param qid Queue id.
     * @return Perm copy when shuffle applies to qid, else empty.
     */
    KANARIV_API static std::vector<int64_t> shufflePermFor(Engine& eng, int64_t qid);

    /**
     * @brief Extends the shuffle permutation after tracks were appended.
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param qid Queue id the tracks were appended to.
     * @param count Number of appended tracks (at the queue end).
     */
    KANARIV_API static void noteEnqueued(Engine& eng, int64_t qid, std::size_t count);

    /**
     * @brief Remaps the shuffle permutation after a queue item moved.
     * @ingroup kanariv_engine
     * @param eng Engine instance.
     * @param qid Queue id the item moved in.
     * @param from Old zero-based queue index.
     * @param to New zero-based queue index.
     */
    KANARIV_API static void noteQueueMoved(Engine& eng, int64_t qid, std::size_t from,
                                           std::size_t to);
};

} // namespace kanariv::engine::detail
