#pragma once

/**
 * @file store.hpp
 * @brief Shared base for the persistence ports.
 * @defgroup kanariv_contracts kanariv persistence contracts
 * @details Every consumer port is served by the same backing store, so health
 * and diagnostics live on one small base instead of being repeated (or, worse,
 * reimplemented) per port. Callers that only need "is storage usable" depend on
 * this type and nothing else.
 */

#include <kanariv/fwd.hpp>

namespace kanariv::contracts {

/**
 * @brief Health and diagnostics shared by every persistence port.
 * @ingroup kanariv_contracts
 * @details A store is one backend (today: one SQLite database) viewed through
 * several narrow ports. The base carries the questions that concern the
 * backend as a whole; operations live on the ports.
 */
class IStore {
  public:
    /**
     * @brief Destroys the store view.
     * @ingroup kanariv_contracts
     */
    virtual ~IStore() = default;

    /**
     * @brief Reports whether the backing store is open and usable.
     * @return `true` when operations may be issued; otherwise `false`.
     * @par Thread safety
     * Implementations must make this query safe to call concurrently with
     * other public operations.
     */
    virtual bool isOpen() const noexcept = 0;

    /**
     * @brief Returns the logger associated with the backing store.
     * @return A non-owning logger pointer, or `nullptr` when logging is
     * disabled.
     * @par Ownership
     * The store retains no ownership of the returned logger. The logger must
     * outlive the store and every call that uses it.
     */
    virtual kanariv::utils::Logger* logger() const noexcept = 0;
};

} // namespace kanariv::contracts
