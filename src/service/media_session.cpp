#include <service/media_session.hpp>

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <atomic>
#include <memory>

namespace kanariv::media_session {

namespace {

/**
 * @brief Android backend factory registered by the JNI shared library.
 * @details The Android backend lives in `android/media_session_android.cpp`,
 * which is linked into the app's JNI library rather than the core service.
 * `JNI_OnLoad` registers it here before the daemon creates its MediaSession.
 */
std::atomic<MediaSession::Factory> g_androidFactory{nullptr};

} // namespace

void MediaSession::setAndroidFactory(Factory factory) noexcept {
    g_androidFactory.store(factory, std::memory_order_release);
}

MediaSession::~MediaSession() = default;

MediaSession::MediaSession(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

std::unique_ptr<MediaSession> MediaSession::wrap(std::unique_ptr<Impl> impl) noexcept {
    return std::unique_ptr<MediaSession>(new MediaSession(std::move(impl)));
}

kanariv::utils::Expected<void> MediaSession::activate() {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "no backend")};
    return impl_->activate();
}

kanariv::utils::Expected<void> MediaSession::deactivate() {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "no backend")};
    return impl_->deactivate();
}

kanariv::utils::Expected<void> MediaSession::setMetadata(const Metadata& meta) {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "no backend")};
    return impl_->setMetadata(meta);
}

kanariv::utils::Expected<void> MediaSession::setPlaybackState(
    kanariv::types::PlaybackState state, double position, double duration) {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "no backend")};
    return impl_->setPlaybackState(state, position, duration);
}

kanariv::utils::Expected<void> MediaSession::setPlaybackMode(
    bool shuffle, kanariv::types::RepeatMode repeat) {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "no backend")};
    return impl_->setPlaybackMode(shuffle, repeat);
}

void MediaSession::setLogger(kanariv::utils::Logger* logger) noexcept {
    if (impl_)
        impl_->setLogger(logger);
}

kanariv::utils::Expected<void> MediaSession::setVolume(float volume, bool muted) {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                      "no backend")};
    return impl_->setVolume(volume, muted);
}

void MediaSession::setCallbacks(Callbacks cb) noexcept {
    if (impl_)
        impl_->setCallbacks(std::move(cb));
}

bool MediaSession::active() const noexcept {
    return impl_ != nullptr && impl_->active();
}

// Forward declarations of platform backend factories, defined in their own TUs.
MediaSession::ExpectedSession createWindows(std::string, std::string, bool, bool);
MediaSession::ExpectedSession createMacOS(std::string, std::string, bool, bool);
MediaSession::ExpectedSession createLinux(std::string, std::string, bool, bool);

namespace {

/**
 * @brief Stub backend for platforms without OS media support.
 * @ingroup kanariv_service
 */
class NoneImpl : public Impl {
  public:
    kanariv::utils::Expected<void> activate() override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "no OS media backend available for this platform")};
    }
    kanariv::utils::Expected<void> deactivate() override { return {}; }
    kanariv::utils::Expected<void> setMetadata(const Metadata&) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "no OS media backend available for this platform")};
    }
    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState, double, double) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "no OS media backend available for this platform")};
    }
    kanariv::utils::Expected<void> setVolume(float, bool) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "no OS media backend available for this platform")};
    }
    bool active() const noexcept override { return false; }
};

[[maybe_unused]] MediaSession::ExpectedSession createNone(std::string appName,
                                                           std::string appIcon,
                                                           bool enableNotifications,
                                                           bool enableMediaKeys) {
    (void)appName;
    (void)appIcon;
    (void)enableNotifications;
    (void)enableMediaKeys;
    return MediaSession::wrap(std::make_unique<NoneImpl>());
}

} // namespace

MediaSession::ExpectedSession MediaSession::create(std::string appName,
                                                    std::string appIcon,
                                                    bool enableNotifications,
                                                    bool enableMediaKeys) {
    (void)appName;
    (void)appIcon;
    (void)enableNotifications;
    (void)enableMediaKeys;

#if defined(_WIN32)
    return createWindows(std::move(appName), std::move(appIcon),
                         enableNotifications, enableMediaKeys);
#elif defined(__APPLE__)
    return createMacOS(std::move(appName), std::move(appIcon),
                       enableNotifications, enableMediaKeys);
#elif defined(__ANDROID__)
    // The real Android backend is compiled into the app's JNI shared library
    // and registers itself from JNI_OnLoad. Fall back to the stub only when no
    // factory is registered (for example in a non-app build).
    if (auto factory = g_androidFactory.load(std::memory_order_acquire))
        return factory(std::move(appName), std::move(appIcon),
                       enableNotifications, enableMediaKeys);
    return createNone(std::move(appName), std::move(appIcon),
                      enableNotifications, enableMediaKeys);
#elif defined(__linux__)
    return createLinux(std::move(appName), std::move(appIcon),
                       enableNotifications, enableMediaKeys);
#else
    return createNone(std::move(appName), std::move(appIcon),
                      enableNotifications, enableMediaKeys);
#endif
}

} // namespace kanariv::media_session
