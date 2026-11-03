#include <atomic>
#include <chrono>
#include <cstddef>
#include <kanariv/service/host.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace kanariv::service {

/**
 * @brief Host state: owned Service, worker thread, stop source, and terminal error.
 * @ingroup kanariv_service
 */
struct HostImpl {
    /** @brief Service owned by the host. Declared first so it outlives the worker. */
    std::unique_ptr<Service> service;
    /** @brief Cooperative cancellation source handed to `Service::run`. */
    std::stop_source stopSource;
    /** @brief Worker running the service loop. */
    std::jthread worker;
    /** @brief Set once the service loop is observed running. */
    std::atomic<bool> started{false};
    /** @brief Set when the worker function returns (success or failure). */
    std::atomic<bool> finished{false};
    /** @brief Guards `terminalError`. */
    mutable std::mutex mutex;
    /** @brief Serializes `wait()` callers without being held during `join()`. */
    std::mutex joinMutex;
    /** @brief Error returned by `Service::run`, when it failed. */
    std::optional<kanariv::utils::Error> terminalError;

    /** @brief Worker body: signal readiness, run, record error, signal completion. */
    void run() {
        auto result = service->run(stopSource.get_token());
        {
            std::scoped_lock lock(mutex);
            if (!result) {
                terminalError = result.error();
            }
        }
        finished.store(true, std::memory_order_release);
    }
};

namespace {

/**
 * @brief Copies the recorded terminal error, if any.
 * @param impl Host state.
 * @return Error copy or nullopt.
 */
auto copyError(const HostImpl& impl) -> std::optional<kanariv::utils::Error> {
    std::scoped_lock lock(impl.mutex);
    return impl.terminalError;
}

} // namespace

Host::Host(std::unique_ptr<HostImpl> impl) noexcept : impl_(std::move(impl)) {}

Host::~Host() {
    stop();
}

auto Host::create(ServiceConfig cfg, std::shared_ptr<kanariv::contracts::IDatabase> db)
    -> Host::ExpectedHost {
    // The host contract is in-process by definition; ignore any desktop mode
    // inherited from a shared configuration object.
    cfg.inProcess = true;

    auto serviceResult = Service::create(cfg, std::move(db));
    if (!serviceResult) {
        return std::unexpected{serviceResult.error()};
    }

    auto impl = std::make_unique<HostImpl>();
    impl->service = std::move(*serviceResult);
    impl->worker = std::jthread([state = impl.get()] -> void { state->run(); });

    // Wait until run() has entered its loop (or failed immediately) so callers
    // can safely use the service as soon as create() returns.
    for (;;) {
        if (impl->service->running()) {
            impl->started.store(true, std::memory_order_release);
            break;
        }
        if (impl->finished.load(std::memory_order_acquire)) {
            auto err = copyError(*impl);
            if (impl->worker.joinable()) {
                impl->worker.join();
            }
            if (err) {
                return std::unexpected{*err};
            }
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                             "service loop exited during startup")};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return std::unique_ptr<Host>(new Host(std::move(impl)));
}

void Host::requestStop() noexcept {
    impl_->stopSource.request_stop();
}

auto Host::wait() -> bool {
    std::scoped_lock joinLock(impl_->joinMutex);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    auto err = copyError(*impl_);
    return !err.has_value();
}

void Host::stop() {
    requestStop();
    (void)wait();
}

auto Host::running() const noexcept -> bool {
    return impl_->service->running();
}

auto Host::ready() const noexcept -> bool {
    return impl_->started.load(std::memory_order_acquire);
}

auto Host::error() const -> std::optional<kanariv::utils::Error> {
    return copyError(*impl_);
}

auto Host::service() noexcept -> Service& {
    return *impl_->service;
}

auto Host::service() const noexcept -> const Service& {
    return *impl_->service;
}

} // namespace kanariv::service
