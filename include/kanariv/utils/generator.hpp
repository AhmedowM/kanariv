#pragma once

/**
 * @file generator.hpp
 * @brief Portable coroutine generator.
 * @ingroup kanariv_utils
 * @details `std::generator` (C++23 P2502) has no libc++ implementation,
 * so a macOS/brew-LLVM build cannot include `<generator>`. This header
 * provides `kanariv::utils::Generator<T>`, a minimal coroutine-based
 * single-pass range supporting `co_yield`/`co_return` and range-for.
 * It uses only `<coroutine>` (available everywhere) and matches the
 * `std::generator` iteration subset used here. Always use this type
 * instead of `std::generator` directly.
 * @note This header is fully inline/template; no .cpp is required.
 */

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace kanariv::utils {

/**
 * @brief Lazy single-pass coroutine range.
 * @ingroup kanariv_utils
 * @tparam T Yielded value type.
 */
template <typename T>
class Generator {
  public:
    /// @brief Coroutine promise. @ingroup kanariv_utils
    struct promise_type {
        std::optional<T> value_; ///< Most recently yielded value.
        std::exception_ptr ex_;  ///< Exception captured from the coroutine body.

        /** @brief Creates the owning Generator for this promise. */
        Generator get_return_object() {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        /** @brief Suspends before the first iteration. */
        std::suspend_always initial_suspend() noexcept {
            return {};
        }
        /** @brief Suspends after completion so the owner can destroy the frame. */
        std::suspend_always final_suspend() noexcept {
            return {};
        }
        /** @brief Stores a yielded value and suspends the coroutine. */
        std::suspend_always yield_value(T v) {
            value_.emplace(std::move(v));
            return {};
        }
        /** @brief Completes the coroutine without yielding another value. */
        void return_void() {}
        /** @brief Captures an exception for rethrow during iteration. */
        void unhandled_exception() {
            ex_ = std::current_exception();
        }
    };

    /** @brief Handle type owned by a Generator instance. */
    using Handle = std::coroutine_handle<promise_type>;

    /// @brief Wraps a coroutine handle. @ingroup kanariv_utils
    /// @param h Live promise handle.
    explicit Generator(Handle h) noexcept : handle_(h) {}
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    /// @brief Transfers ownership. @ingroup kanariv_utils
    /// @param o Source; left empty.
    Generator(Generator&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    Generator& operator=(Generator&&) = delete;
    /// @brief Destroys the coroutine. @ingroup kanariv_utils
    ~Generator() {
        if (handle_)
            handle_.destroy();
    }

    /// @brief End sentinel. @ingroup kanariv_utils
    struct sentinel {};

    /// @brief Input iterator over yielded values. @ingroup kanariv_utils
    struct iterator {
        Handle handle_ = {};

        /** @brief Resumes the coroutine and advances to the next value. */
        iterator& operator++() {
            handle_.promise().value_.reset();
            handle_.resume();
            if (handle_.promise().ex_)
                std::rethrow_exception(handle_.promise().ex_);
            return *this;
        }
        /** @brief Returns the current yielded value. */
        T& operator*() const {
            return *handle_.promise().value_;
        }
        /** @brief Compares this iterator with the end sentinel. */
        bool operator==(sentinel) const {
            return handle_.done();
        }
    };

    /**
     * @brief Starts iteration (resumes to first yield).
     * @ingroup kanariv_utils
     * @return Iterator positioned at the first value (or end).
     */
    iterator begin() {
        if (handle_) {
            handle_.resume();
            if (handle_.promise().ex_)
                std::rethrow_exception(handle_.promise().ex_);
        }
        return iterator{handle_};
    }

    /// @brief End of range. @ingroup kanariv_utils
    /// @return Sentinel compared against `done()`.
    sentinel end() const {
        return {};
    }

  private:
    Handle handle_ = {};
};

} // namespace kanariv::utils
