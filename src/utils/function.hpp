#pragma once

/**
 * @file function.hpp
 * @brief Portable move-only type-erased callable.
 * @ingroup kanariv_utils
 * @details `std::move_only_function` (C++23 P0288) is Complete in
 * libstdc++ and MSVC STL but has no libc++ implementation (not even
 * experimental), so a macOS/brew-LLVM build cannot name it. This header
 * provides `kanariv::utils::MoveOnlyFunction<R(Args...)>` with the subset
 * used here (default/empty construct, move-only, call, bool check).
 * Always use this type instead of `std::move_only_function` directly.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace kanariv::utils {

/**
 * @brief Move-only type-erased callable.
 * @ingroup kanariv_utils
 * @tparam Sig Function signature `R(Args...)`.
 */
template <typename Sig>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
  public:
    /// @brief Constructs an empty callable. @ingroup kanariv_utils
    MoveOnlyFunction() noexcept = default;
    /// @brief Constructs an empty callable from nullptr. @ingroup kanariv_utils
    MoveOnlyFunction(std::nullptr_t) noexcept {}
    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;

    /**
     * @brief Constructs from any invocable target.
     * @ingroup kanariv_utils
     * @tparam F Callable type (stored by decay, moved in).
     * @param f Target to wrap.
     */
    template <typename F>
        requires(!std::same_as<std::decay_t<F>, MoveOnlyFunction> &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
    MoveOnlyFunction(F&& f) : ptr_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}

    /**
     * @brief Assigns an empty target. @ingroup kanariv_utils
     * @return *this
     */
    MoveOnlyFunction& operator=(std::nullptr_t) noexcept {
        ptr_.reset();
        return *this;
    }

    /**
     * @brief Assigns a new invocable target.
     * @ingroup kanariv_utils
     * @tparam F Callable type.
     * @param f Target to wrap.
     * @return *this
     */
    template <typename F>
        requires(!std::same_as<std::decay_t<F>, MoveOnlyFunction> &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
    MoveOnlyFunction& operator=(F&& f) {
        ptr_ = std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f));
        return *this;
    }

    /**
     * @brief Invokes the target.
     * @ingroup kanariv_utils
     * @param args Arguments forwarded to the target.
     * @return Target result (empty call is UB, as with `std::function`).
     */
    R operator()(Args... args) {
        return ptr_->call(std::forward<Args>(args)...);
    }

    /// @brief Checks for a target. @ingroup kanariv_utils
    /// @return true when non-empty.
    explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

  private:
    struct Concept {
        virtual ~Concept() = default;
        virtual R call(Args&&... args) = 0;
    };

    template <typename F>
    struct Model final : Concept {
        template <typename U>
        explicit Model(U&& u) : f_(std::forward<U>(u)) {}
        R call(Args&&... args) override {
            if constexpr (std::is_void_v<R>) {
                std::invoke(f_, std::forward<Args>(args)...);
            } else {
                return std::invoke(f_, std::forward<Args>(args)...);
            }
        }
        F f_;
    };

    std::unique_ptr<Concept> ptr_;
};

} // namespace kanariv::utils
