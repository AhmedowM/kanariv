#pragma once

/**
 * @file service_paths.hpp
 * @brief Daemon path, lock and liveness helpers for kanariv.service (internal).
 * @ingroup kanariv_service
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace kanariv::service::detail {

/**
 * @brief Get PID file path for a database path.
 * Delegates to cli::pidPathFor; falls back to dbPath parent / "kanariv.pid".
 * @param dbPath Database path.
 * @param socketPath Unused (kept for signature compatibility).
 * @return PID file path.
 */
std::filesystem::path pidPathForSocket(const std::filesystem::path& dbPath,
                                       const std::string& /*socketPath*/);

/**
 * @brief Get lock file path for a database path.
 * Delegates to cli::lockPathFor; falls back to PID dir / "kanariv-<hash>.lock".
 * @param dbPath Database path.
 * @param socketPath Unused (kept for signature compatibility).
 * @return Lock file path.
 */
std::filesystem::path lockPathForSocket(const std::filesystem::path& dbPath,
                                        const std::string& /*socketPath*/);

/**
 * @brief Get socket path for a database path.
 * Delegates to config::socketPathFor; falls back to platform-specific default.
 * Windows: Named pipe \\.\pipe\kanariv-<hash>
 * POSIX: $XDG_RUNTIME_DIR/kanariv/kanariv-<hash>.sock (created if needed)
 * @param dbPath Database path.
 * @return Socket path string.
 */
std::string socketPathForDb(const std::filesystem::path& dbPath);

/**
 * @brief Probe if a socket/named pipe is alive (connectable).
 * Windows: Try CreateFileW + WaitNamedPipeW.
 * POSIX: Try connect() to AF_UNIX socket.
 * @param sp Socket path string.
 * @return true if connection succeeds (service alive), false otherwise.
 */
bool probeSocketAlive(const std::string& sp);

/**
 * @brief Try to acquire exclusive lock on a file (single-instance enforcement).
 * Windows: CreateFileW + LockFileEx on first byte.
 * POSIX: open() + flock(LOCK_EX | LOCK_NB).
 * Creates parent directories if needed.
 * @param lockPath Lock file path.
 * @param outFd Output file descriptor/handle (set on success).
 * @return true if lock acquired, false if already held or error.
 */
bool tryAcquireLock(const std::filesystem::path& lockPath, std::intptr_t& outFd);

/**
 * @brief Release a previously acquired lock.
 * Windows: UnlockFileEx + CloseHandle.
 * POSIX: flock(LOCK_UN) + close().
 * @param fd File descriptor/handle returned by tryAcquireLock.
 */
void releaseLock(std::intptr_t fd);

/**
 * @brief Check if a process ID is alive.
 * POSIX: kill(pid, 0) == 0.
 * Windows: OpenProcess(SYNCHRONIZE) + WaitForSingleObject(0) == WAIT_TIMEOUT.
 * @param pid Process ID to check.
 * @return true if process exists, false otherwise.
 */
bool checkPidAlive(int pid);

/**
 * @brief Read PID from a PID file.
 * @param pidPath Path to PID file.
 * @return PID if file exists and contains valid integer, nullopt otherwise.
 */
std::optional<int> readPidFile(const std::filesystem::path& pidPath);

/**
 * @brief Resolve config file path.
 * Priority: explicit configPath > dbPath parent / "config.json" > temp/kanariv/config.json.
 * @param configPath Explicit config path (may be empty).
 * @param dbPath Database path.
 * @return Resolved config file path.
 */
std::filesystem::path resolveConfigPath(const std::filesystem::path& configPath,
                                        const std::filesystem::path& dbPath);

} // namespace kanariv::service::detail
