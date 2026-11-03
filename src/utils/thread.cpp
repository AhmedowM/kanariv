#include <cstddef>
#include <cstring>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/thread.hpp>
#include <kanariv/windows_fwd.hpp>
#include <string>
#include <string_view>

#ifdef _WIN32
inline constexpr unsigned utf8CodePage = 65001;
#else
#include <pthread.h>
#include <time.h>

#include <cerrno>
#endif

namespace kanariv::utils::detail {

#ifdef _WIN32
auto setNativeHandleName(void* nativeHandle, std::string_view name) noexcept -> Expected<void> {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32 != nullptr) {
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        auto pSetDesc = reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(
            k32, "SetThreadDescription")); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif
        if (pSetDesc != nullptr) {
            int wlen = MultiByteToWideChar(utf8CodePage, 0, name.data(),
                                           static_cast<int>(name.size()), nullptr, 0);
            if (wlen > 0) {
                std::wstring wbuf(static_cast<std::size_t>(wlen), L'\0');
                MultiByteToWideChar(utf8CodePage, 0, name.data(), static_cast<int>(name.size()),
                                    wbuf.data(), wlen);
                HRESULT hr =
                    pSetDesc(reinterpret_cast<HANDLE>(nativeHandle),
                             wbuf.c_str()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                (void)hr;
                return {};
            }
        }
    }
    return {};
}

auto setCurrentThreadNameImpl(std::string_view name) noexcept -> Expected<void> {
    return setNativeHandleName(GetCurrentThread(), name);
}
#else
int setPthreadName(pthread_t th, std::string_view name) noexcept {
    std::string_view t = truncate15(name);
    char buf[16]{};
    if (!t.empty())
        std::memcpy(buf, t.data(), t.size());
    buf[t.size()] = '\0';
#ifdef __APPLE__
    // macOS can only name the calling thread (no thread-targeted variant).
    if (!pthread_equal(th, pthread_self()))
        return 0;
    return pthread_setname_np(buf);
#else
    return pthread_setname_np(th, buf);
#endif
}
#endif

} // namespace kanariv::utils::detail

namespace kanariv::utils {

[[nodiscard]] auto setThreadName(std::string_view name) noexcept -> Expected<void> {
    if (name.empty()) {
        return std::unexpected(Error{StatusCode::InvalidArg, std::string_view("empty name")});
    }
#ifdef _WIN32
    return detail::setCurrentThreadNameImpl(name);
#else
#if defined(__APPLE__) && defined(__MACH__)
    std::string_view t = detail::truncate15(name);
    char buf[16]{};
    if (!t.empty())
        std::memcpy(buf, t.data(), t.size());
    buf[t.size()] = '\0';
    int rc = pthread_setname_np(buf);
    if (rc != 0) {
        return std::unexpected(
            Error{StatusCode::Unsupported, std::string_view("pthread_setname_np failed")});
    }
    return {};
#elif defined(__linux__) || defined(_GNU_SOURCE) || defined(__GLIBC__)
    int rc = detail::setPthreadName(pthread_self(), name);
    if (rc != 0) {
        return std::unexpected(
            Error{StatusCode::Unsupported, std::string_view("pthread_setname_np failed")});
    }
    return {};
#else
    (void)name;
    return std::unexpected(
        Error{StatusCode::Unsupported, std::string_view("setThreadName not supported")});
#endif
#endif
}

[[nodiscard]] auto setThreadName(std::jthread& jt, std::string_view name) noexcept
    -> Expected<void> {
    if (!jt.joinable()) {
        return std::unexpected(Error{StatusCode::State, std::string_view("thread not joinable")});
    }
    if (name.empty()) {
        return std::unexpected(Error{StatusCode::InvalidArg, std::string_view("empty name")});
    }
#ifdef _WIN32
    auto *h = reinterpret_cast<HANDLE>(
        jt.native_handle()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return detail::setNativeHandleName(h, name);
#else
    pthread_t th = jt.native_handle();
#if defined(__APPLE__) && defined(__MACH__)
    // macOS pthread_setname_np only supports naming the current thread (no handle arg).
    // std::jthread native_handle is not the current thread, so we cannot name it.
    (void)th;
    (void)name;
    return std::unexpected(
        Error{StatusCode::Unsupported,
              std::string_view("setThreadName with jthread not supported on macOS")});
#elif defined(__linux__)
    int rc = detail::setPthreadName(th, name);
    if (rc != 0) {
        return std::unexpected(
            Error{StatusCode::Unsupported, std::string_view("pthread_setname_np failed")});
    }
    return {};
#else
    (void)th;
    (void)name;
    return std::unexpected(
        Error{StatusCode::Unsupported, std::string_view("setThreadName not supported")});
#endif
#endif
}

[[nodiscard]] auto setThreadName(std::thread& t, std::string_view name) noexcept -> Expected<void> {
    if (!t.joinable()) {
        return std::unexpected(Error{StatusCode::State, std::string_view("thread not joinable")});
    }
    if (name.empty()) {
        return std::unexpected(Error{StatusCode::InvalidArg, std::string_view("empty name")});
    }
#ifdef _WIN32
    auto *h = reinterpret_cast<HANDLE>(
        t.native_handle()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return detail::setNativeHandleName(h, name);
#else
#if defined(__linux__) && !defined(__APPLE__)
    pthread_t th = t.native_handle();
    int rc = detail::setPthreadName(th, name);
    if (rc != 0)
        return std::unexpected(
            Error{StatusCode::Unsupported, std::string_view("pthread_setname_np failed")});
    return {};
#else
    (void)t;
    (void)name;
    return std::unexpected(Error{StatusCode::Unsupported, std::string_view("not supported")});
#endif
#endif
}

} // namespace kanariv::utils
