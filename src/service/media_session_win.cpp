#include <service/media_session.hpp>

#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/log.hpp>

#include <windows.h>
#include <SystemMediaTransportControlsInterop.h>
#include <shcore.h>

#include <winrt/windows.foundation.h>
#include <winrt/windows.media.h>
#include <winrt/windows.storage.streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kanariv::media_session {

using winrt::Windows::Media::MediaPlaybackStatus;
using winrt::Windows::Media::MediaPlaybackType;
using winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
using winrt::Windows::Media::ISystemMediaTransportControls;
using winrt::Windows::Media::AutoRepeatModeChangeRequestedEventArgs;
using winrt::Windows::Media::PlaybackPositionChangeRequestedEventArgs;
using winrt::Windows::Media::ShuffleEnabledChangeRequestedEventArgs;
using winrt::Windows::Media::SystemMediaTransportControls;
using winrt::Windows::Media::SystemMediaTransportControlsButton;
using winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties;

class WinImpl final : public Impl {
  public:
    WinImpl(std::string appName, std::string appIcon,
            bool enableNotifications, bool enableMediaKeys)
        : appName_(std::move(appName)),
          appIcon_(std::move(appIcon)),
          enableNotifications_(enableNotifications),
          enableMediaKeys_(enableMediaKeys) {}

    ~WinImpl() override { (void)deactivate(); }

    kanariv::utils::Expected<void> activate() override {
        std::lock_guard lock(mtx_);
        if (active_)
            return {};

        stopRequested_.store(false, std::memory_order_release);
        hotkeyThread_ = std::thread([this] { threadProc(); });
        active_ = true;
        return {};
    }

    kanariv::utils::Expected<void> deactivate() override {
        {
            std::lock_guard lock(mtx_);
            if (!active_)
                return {};

            stopRequested_.store(true, std::memory_order_release);
            if (hwnd_)
                PostMessageW(hwnd_, WM_QUIT, 0, 0);
        }

        if (hotkeyThread_.joinable())
            hotkeyThread_.join();

        std::lock_guard lock(mtx_);
        active_ = false;
        hwnd_ = nullptr;
        return {};
    }

    kanariv::utils::Expected<void> setMetadata(
        const Metadata& metadata) override {
        {
            std::lock_guard lock(mtx_);
            metadata_ = metadata;
        }
        requestUpdate();
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState state, double position,
        double duration) override {
        {
            std::lock_guard lock(mtx_);
            state_ = state;
            position_ = position;
            duration_ = duration;
            positionStamp_ = std::chrono::steady_clock::now();
        }
        requestUpdate();
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackMode(
        bool shuffle, kanariv::types::RepeatMode repeat) override {
        {
            std::lock_guard lock(mtx_);
            shuffle_ = shuffle;
            repeat_ = repeat;
        }
        requestUpdate();
        return {};
    }

    void setLogger(kanariv::utils::Logger* logger) noexcept override {
        logger_ = logger;
    }

    kanariv::utils::Expected<void> setVolume(float, bool) override {
        // SMTC has no per-application volume; the OS mixer controls volume.
        return {};
    }

    bool active() const noexcept override {
        std::lock_guard lock(mtx_);
        return active_;
    }

  private:
    static constexpr UINT WM_UPDATE_SMTC = WM_APP + 1;
    static constexpr UINT_PTR kTimelineTimer = 1;
    // Microsoft recommends refreshing the SMTC timeline about every 5
    // seconds while playing so the seek bar (and PositionChangeRequested)
    // stays in sync with the real playback position.
    static constexpr UINT kTimelineIntervalMs = 5'000;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam,
                                    LPARAM lParam) {
        auto* self = reinterpret_cast<WinImpl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<WinImpl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }

        if (!self)
            return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
            case WM_UPDATE_SMTC:
                self->publishToSmtc();
                return 0;
            case WM_TIMER:
                if (wParam == kTimelineTimer) {
                    self->updateTimeline();
                    return 0;
                }
                return DefWindowProcW(hwnd, message, wParam, lParam);
            case WM_HOTKEY:
                self->handleHotkey(static_cast<int>(wParam));
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void requestUpdate() {
        std::lock_guard lock(mtx_);
        if (hwnd_)
            PostMessageW(hwnd_, WM_UPDATE_SMTC, 0, 0);
    }

    void threadProc() {
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);

            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = &WinImpl::wndProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"KanarivMediaSessionWindow";
            RegisterClassW(&windowClass);

            HWND hwnd = CreateWindowExW(
                WS_EX_TOOLWINDOW, windowClass.lpszClassName,
                L"Kanariv Media Session", WS_OVERLAPPED,
                0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, this);
            if (!hwnd) {
                UnregisterClassW(windowClass.lpszClassName,
                                 windowClass.hInstance);
                winrt::uninit_apartment();
                return;
            }

            {
                std::lock_guard lock(mtx_);
                hwnd_ = hwnd;
            }

            if (enableMediaKeys_) {
                RegisterHotKey(hwnd, 1, 0, VK_MEDIA_PLAY_PAUSE);
                RegisterHotKey(hwnd, 2, 0, VK_MEDIA_STOP);
                RegisterHotKey(hwnd, 3, 0, VK_MEDIA_NEXT_TRACK);
                RegisterHotKey(hwnd, 4, 0, VK_MEDIA_PREV_TRACK);
            }

            try {
                initializeSmtc();
            } catch (const winrt::hresult_error& error) {
                if (logger_)
                    logger_->warn("SMTC initialization failed: {}",
                                  winrt::to_string(error.message()));
            } catch (const std::exception& error) {
                if (logger_)
                    logger_->warn("SMTC initialization failed: {}", error.what());
            }

            if (smtc_)
                SetTimer(hwnd, kTimelineTimer, kTimelineIntervalMs, nullptr);

            MSG message{};
            while (!stopRequested_.load(std::memory_order_acquire)) {
                const BOOL result = GetMessageW(&message, nullptr, 0, 0);
                if (result <= 0)
                    break;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            KillTimer(hwnd, kTimelineTimer);

            if (enableMediaKeys_) {
                UnregisterHotKey(hwnd, 1);
                UnregisterHotKey(hwnd, 2);
                UnregisterHotKey(hwnd, 3);
                UnregisterHotKey(hwnd, 4);
            }

            releaseSmtc();
            DestroyWindow(hwnd);
            UnregisterClassW(windowClass.lpszClassName,
                             windowClass.hInstance);

            {
                std::lock_guard lock(mtx_);
                hwnd_ = nullptr;
            }
            winrt::uninit_apartment();
        } catch (const winrt::hresult_error& error) {
            if (logger_)
                logger_->error("media-session thread failed: {}",
                               winrt::to_string(error.message()));
            std::lock_guard lock(mtx_);
            hwnd_ = nullptr;
        } catch (const std::exception& error) {
            if (logger_)
                logger_->error("media-session thread failed: {}", error.what());
            std::lock_guard lock(mtx_);
            hwnd_ = nullptr;
        } catch (...) {
            std::lock_guard lock(mtx_);
            hwnd_ = nullptr;
        }
    }

    void initializeSmtc() {
        if (logger_)
            logger_->debug("SMTC initialization started");
        auto interop = winrt::get_activation_factory<
            SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();

        SystemMediaTransportControls smtc{nullptr};
        winrt::check_hresult(interop->GetForWindow(
            hwnd_, winrt::guid_of<ISystemMediaTransportControls>(),
            reinterpret_cast<void**>(winrt::put_abi(smtc))));

        smtc_ = std::move(smtc);
        smtc_.IsEnabled(true);
        smtc_.IsPlayEnabled(true);
        smtc_.IsPauseEnabled(true);
        smtc_.IsStopEnabled(true);
        smtc_.IsNextEnabled(true);
        smtc_.IsPreviousEnabled(true);
        buttonPressedToken_ = smtc_.ButtonPressed(
            {this, &WinImpl::onButtonPressed});
        repeatModeToken_ = smtc_.AutoRepeatModeChangeRequested(
            {this, &WinImpl::onRepeatModeChangeRequested});
        shuffleToken_ = smtc_.ShuffleEnabledChangeRequested(
            {this, &WinImpl::onShuffleChangeRequested});
        positionToken_ = smtc_.PlaybackPositionChangeRequested(
            {this, &WinImpl::onPositionChangeRequested});
        if (logger_)
            logger_->debug("SMTC controls and request handlers registered");
        publishToSmtc();
    }

    void releaseSmtc() {
        if (smtc_) {
            smtc_.ButtonPressed(buttonPressedToken_);
            smtc_.AutoRepeatModeChangeRequested(repeatModeToken_);
            smtc_.ShuffleEnabledChangeRequested(shuffleToken_);
            smtc_.PlaybackPositionChangeRequested(positionToken_);
            smtc_ = nullptr;
        }
        removeArtworkFile();
    }

    void removeArtworkFile() {
        if (!artworkFile_.empty()) {
            std::error_code error;
            std::filesystem::remove(artworkFile_, error);
            artworkFile_.clear();
            artworkKey_.clear();
        }
    }

    bool prepareArtworkFile(const Metadata& metadata) {
        if (!metadata.artworkBase64 || metadata.artworkBase64->empty()) {
            removeArtworkFile();
            return false;
        }

        if (*metadata.artworkBase64 == artworkKey_ &&
            !artworkFile_.empty())
            return true;

        auto decoded = kanariv::utils::Base64::decode(
            *metadata.artworkBase64);
        if (!decoded || decoded->empty())
            return false;

        const auto key = std::hash<std::string>{}(*metadata.artworkBase64);
        std::string extension = ".img";
        if (metadata.artworkMime == "image/jpeg")
            extension = ".jpg";
        else if (metadata.artworkMime == "image/png")
            extension = ".png";
        else if (metadata.artworkMime == "image/webp")
            extension = ".webp";
        auto path = std::filesystem::temp_directory_path() /
                    ("kanariv-smtc-" + std::to_string(key) + extension);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;

        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
        if (!output)
            return false;

        removeArtworkFile();
        artworkFile_ = std::move(path);
        artworkKey_ = *metadata.artworkBase64;
        return true;
    }

    void publishToSmtc() {
        if (!smtc_)
            return;

        if (publishing_) {
            publishPending_ = true;
            if (logger_)
                logger_->debug("SMTC publish coalesced while artwork is pending");
            return;
        }

        publishing_ = true;
        struct PublishScope final {
            WinImpl& owner;
            ~PublishScope() {
                owner.publishing_ = false;
                if (owner.publishPending_) {
                    owner.publishPending_ = false;
                    if (owner.hwnd_)
                        PostMessageW(owner.hwnd_, WM_UPDATE_SMTC, 0, 0);
                }
            }
        } publishScope{*this};

        if (logger_)
            logger_->debug("SMTC publishing metadata and artwork");

        Metadata metadata;
        kanariv::types::PlaybackState state;
        kanariv::types::RepeatMode repeat;
        bool shuffle;
        double position;
        double duration;
        std::chrono::steady_clock::time_point stamp;
        {
            std::lock_guard lock(mtx_);
            metadata = metadata_;
            state = state_;
            repeat = repeat_;
            shuffle = shuffle_;
            position = position_;
            duration = duration_;
            stamp = positionStamp_;
        }

        switch (state) {
            case kanariv::types::PlaybackState::Playing:
                smtc_.PlaybackStatus(MediaPlaybackStatus::Playing);
                break;
            case kanariv::types::PlaybackState::Paused:
                smtc_.PlaybackStatus(MediaPlaybackStatus::Paused);
                break;
            default:
                smtc_.PlaybackStatus(MediaPlaybackStatus::Stopped);
                break;
        }

        switch (repeat) {
            case kanariv::types::RepeatMode::One:
                smtc_.AutoRepeatMode(MediaPlaybackAutoRepeatMode::Track);
                break;
            case kanariv::types::RepeatMode::All:
                smtc_.AutoRepeatMode(MediaPlaybackAutoRepeatMode::List);
                break;
            default:
                smtc_.AutoRepeatMode(MediaPlaybackAutoRepeatMode::None);
                break;
        }
        smtc_.ShuffleEnabled(shuffle);

        publishTimeline(state, position, duration, stamp);

        // The display updater drives the media flyout / lock screen
        // notification. When notifications are disabled the timeline and
        // transport buttons still work, but no media card is published.
        if (!enableNotifications_) {
            if (logger_)
                logger_->debug("SMTC notifications disabled; skipping metadata");
            return;
        }

        auto updater = smtc_.DisplayUpdater();
        updater.ClearAll();
        updater.Type(MediaPlaybackType::Music);

        auto properties = updater.MusicProperties();
        properties.Title(winrt::to_hstring(metadata.title));
        properties.Artist(winrt::to_hstring(metadata.artist));
        if (metadata.album)
            properties.AlbumTitle(winrt::to_hstring(*metadata.album));
        if (metadata.trackId > 0)
            properties.TrackNumber(static_cast<uint32_t>(metadata.trackId));

        if (metadata.artworkBase64 && !metadata.artworkBase64->empty()) {
            try {
                if (logger_)
                    logger_->debug("SMTC decoding artwork ({} chars)",
                                   metadata.artworkBase64->size());
                if (prepareArtworkFile(metadata)) {
                    winrt::Windows::Storage::Streams::IRandomAccessStream stream{
                        nullptr};
                    winrt::check_hresult(CreateRandomAccessStreamOnFile(
                        artworkFile_.c_str(), STGM_READ,
                        winrt::guid_of<decltype(stream)>(),
                        reinterpret_cast<void**>(winrt::put_abi(stream))));
                    updater.Thumbnail(
                        winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromStream(stream));
                    if (logger_)
                        logger_->debug("SMTC artwork file stream attached");
                }
            } catch (const winrt::hresult_error& error) {
                if (logger_)
                    logger_->warn("SMTC artwork update failed: {}",
                                  winrt::to_string(error.message()));
            } catch (const std::exception& error) {
                if (logger_)
                    logger_->warn("SMTC artwork update failed: {}", error.what());
            }
        }
        updater.Update();
        if (logger_)
            logger_->debug("SMTC metadata update complete");
    }

    static winrt::Windows::Foundation::TimeSpan toTimeSpan(double seconds) {
        return winrt::Windows::Foundation::TimeSpan{
            static_cast<int64_t>((seconds > 0.0 ? seconds : 0.0) * 10'000'000.0)};
    }

    // The daemon only pushes a position when playback state changes, so the
    // timeline has to extrapolate from the last update while playing.
    static double effectivePosition(kanariv::types::PlaybackState state,
                                    double position, double duration,
                                    std::chrono::steady_clock::time_point stamp) {
        if (state == kanariv::types::PlaybackState::Playing) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - stamp).count();
            position += elapsed;
            if (duration > 0.0 && position > duration)
                position = duration;
        }
        return position > 0.0 ? position : 0.0;
    }

    void publishTimeline(kanariv::types::PlaybackState state, double position,
                         double duration,
                         std::chrono::steady_clock::time_point stamp) {
        if (!smtc_)
            return;

        const double current = effectivePosition(state, position, duration, stamp);
        SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(winrt::Windows::Foundation::TimeSpan{0});
        // Microsoft: PositionChangeRequested is only raised when both
        // MinSeekTime and MaxSeekTime are set.
        timeline.MinSeekTime(winrt::Windows::Foundation::TimeSpan{0});
        timeline.Position(toTimeSpan(current));
        timeline.EndTime(toTimeSpan(duration));
        timeline.MaxSeekTime(toTimeSpan(duration));
        smtc_.UpdateTimelineProperties(timeline);
    }

    // Called every kTimelineIntervalMs from the message-loop WM_TIMER so the
    // system seek bar tracks the real position (and keeps PositionChangeRequested
    // enabled) without waiting for the next daemon state sync.
    void updateTimeline() {
        kanariv::types::PlaybackState state;
        double position = 0.0;
        double duration = 0.0;
        std::chrono::steady_clock::time_point stamp;
        {
            std::lock_guard lock(mtx_);
            state = state_;
            position = position_;
            duration = duration_;
            stamp = positionStamp_;
        }
        publishTimeline(state, position, duration, stamp);
    }

    void onRepeatModeChangeRequested(
        SystemMediaTransportControls const&,
        AutoRepeatModeChangeRequestedEventArgs const& args) {
        kanariv::types::RepeatMode repeat =
            kanariv::types::RepeatMode::Off;
        switch (args.RequestedAutoRepeatMode()) {
            case MediaPlaybackAutoRepeatMode::Track:
                repeat = kanariv::types::RepeatMode::One;
                break;
            case MediaPlaybackAutoRepeatMode::List:
                repeat = kanariv::types::RepeatMode::All;
                break;
            default:
                break;
        }

        Callbacks callbacks;
        {
            std::lock_guard lock(mtx_);
            callbacks = callbacks_;
        }
        if (callbacks.onRepeat)
            callbacks.onRepeat(repeat);
    }

    void onShuffleChangeRequested(
        SystemMediaTransportControls const&,
        ShuffleEnabledChangeRequestedEventArgs const& args) {
        Callbacks callbacks;
        {
            std::lock_guard lock(mtx_);
            callbacks = callbacks_;
        }
        if (callbacks.onShuffle)
            callbacks.onShuffle(args.RequestedShuffleEnabled());
    }

    void onPositionChangeRequested(
        SystemMediaTransportControls const&,
        PlaybackPositionChangeRequestedEventArgs const& args) {
        Callbacks callbacks;
        {
            std::lock_guard lock(mtx_);
            callbacks = callbacks_;
        }
        if (callbacks.onSeekTo) {
            const auto position = args.RequestedPlaybackPosition();
            callbacks.onSeekTo(std::chrono::duration<double>(position).count());
        }
    }

    void onButtonPressed(
        SystemMediaTransportControls const&,
        winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const& args) {
        Callbacks callbacks;
        {
            std::lock_guard lock(mtx_);
            callbacks = callbacks_;
        }

        switch (args.Button()) {
            case SystemMediaTransportControlsButton::Play:
                if (callbacks.onPlay) callbacks.onPlay();
                break;
            case SystemMediaTransportControlsButton::Pause:
                if (callbacks.onPause) callbacks.onPause();
                break;
            case SystemMediaTransportControlsButton::Stop:
                if (callbacks.onStop) callbacks.onStop();
                break;
            case SystemMediaTransportControlsButton::Next:
                if (callbacks.onNext) callbacks.onNext();
                break;
            case SystemMediaTransportControlsButton::Previous:
                if (callbacks.onPrev) callbacks.onPrev();
                break;
            default:
                break;
        }
    }

    void handleHotkey(int id) {
        Callbacks callbacks;
        bool playing = false;
        {
            std::lock_guard lock(mtx_);
            callbacks = callbacks_;
            playing = state_ == kanariv::types::PlaybackState::Playing;
        }

        switch (id) {
            case 1:
                if (playing) {
                    if (callbacks.onPause) callbacks.onPause();
                } else if (callbacks.onPlay) {
                    callbacks.onPlay();
                }
                break;
            case 2:
                if (callbacks.onStop) callbacks.onStop();
                break;
            case 3:
                if (callbacks.onNext) callbacks.onNext();
                break;
            case 4:
                if (callbacks.onPrev) callbacks.onPrev();
                break;
            default:
                break;
        }
    }

    [[maybe_unused]] std::string appName_;
    [[maybe_unused]] std::string appIcon_;
    bool enableNotifications_{true};
    bool enableMediaKeys_{true};

    mutable std::mutex mtx_;
    bool active_{false};
    HWND hwnd_{nullptr};
    std::thread hotkeyThread_;
    std::atomic<bool> stopRequested_{false};

    SystemMediaTransportControls smtc_{nullptr};
    winrt::event_token buttonPressedToken_{};
    winrt::event_token repeatModeToken_{};
    winrt::event_token shuffleToken_{};
    winrt::event_token positionToken_{};

    Metadata metadata_;
    kanariv::types::PlaybackState state_{};
    double position_{0.0};
    double duration_{0.0};
    // Timestamp of the last position pushed by the daemon, used to
    // extrapolate the timeline while playing.
    std::chrono::steady_clock::time_point positionStamp_{};
    bool shuffle_{false};
    kanariv::types::RepeatMode repeat_{kanariv::types::RepeatMode::Off};
    kanariv::utils::Logger* logger_{nullptr};
    std::filesystem::path artworkFile_;
    std::string artworkKey_;
    bool publishing_{false};
    bool publishPending_{false};
};

MediaSession::ExpectedSession createWindows(
    std::string appName, std::string appIcon, bool enableNotifications,
    bool enableMediaKeys) {
    return MediaSession::wrap(std::make_unique<WinImpl>(
        std::move(appName), std::move(appIcon), enableNotifications,
        enableMediaKeys));
}

} // namespace kanariv::media_session
