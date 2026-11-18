/**
 * @file render.cpp
 * @brief Outcome rendering shared by every CLI domain.
 * @details Moved verbatim out of the monolithic shell: the byte-level policy
 * (error prefix, JSON vs human, warning vs failure, batch flush) is unchanged.
 */

#include "render.hpp"

#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <kanariv/config.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/print.hpp>
#include <kanariv/version.hpp>
#include <string>
#include <system_error>
#include <utility>

#include "cli_helpers.hpp"

namespace kanariv::app::cli::detail {

std::string statusLine(const kanariv::ipc::Status& st) {
    std::string s{std::format("{}", st.state)};
    std::string who;
    if (!st.artist.empty() && !st.title.empty())
        who = st.artist + " - " + st.title;
    else
        who = st.artist + st.title;
    if (!who.empty())
        s += " " + who;
    s += " [" + kanariv::utils::formatClock(st.pos) + "/" + kanariv::utils::formatClock(st.dur) +
         "]";
    return s;
}

int renderError(const kanariv::utils::Error& e) {
    // Same renderer as kanariv::client::formatError, so both paths emit the
    // exact same "Error: <Code>: <message>" bytes.
    kanariv::client::OutputFormatter{}.printError(e, std::cerr);
    return exitCodeForError(e);
}

// Prints a result as JSON on stdout. Never returns a status: the caller
// decides it, so JSON mode cannot mask a failed batch with exit 0.
void renderJson(const kanariv::ipc::Result& r) {
    kanariv::client::OutputFormatter fmt{true};
    fmt.print(r, std::cout);
}

// True when stdout is an interactive terminal (input for the ANSI gate).
bool stdoutIsTty() {
#ifdef _WIN32
    return ::_isatty(::_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

// Terminal/ANSI detection for renderers (NO_COLOR honored). Gates colour and
// the carriage-return/erase-line escapes so piped output stays clean.
bool useColor() {
    return ansiEnabled(stdoutIsTty(), std::getenv("NO_COLOR") != nullptr);
}

PersistDefaultsGuard::~PersistDefaultsGuard() {
    if (!enabled_ || cfg_.configPath.empty())
        return;
    std::error_code ec;
    auto parent = cfg_.configPath.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    auto saved = kanariv::config::saveConfig(cfg_);
    if (!saved) {
        kanariv::println(std::cerr, "warning: could not write config file '{}': {}",
                         cfg_.configPath.generic_string(), saved.error().message);
    }
}

// Renders a Phase-1 outcome: errors (with hint), JSON results, human
// lines, bare results, or silent success, in that order.
int render(kanariv::app::AppResult res, bool asJson, std::string_view columns) {
    if (!res)
        return renderError(res.error());
    auto outcome = std::move(*res);
    if (outcome.failed) {
        if (outcome.line)
            kanariv::println(std::cerr, "{}", *outcome.line);
        return exitRuntime;
    }
    if (outcome.quiet && !asJson)
        return exitOk;
    if (asJson && outcome.result) {
        renderJson(*outcome.result);
        return exitOk;
    }
    if (outcome.line) {
        if (outcome.toStderr)
            kanariv::println(std::cerr, "{}", *outcome.line);
        else
            kanariv::println(std::cout, "{}", *outcome.line);
        return exitOk;
    }
    if (outcome.result) {
        kanariv::client::OutputFormatter fmt{false};
        if (!columns.empty())
            fmt.setColumns(columns);
        fmt.print(*outcome.result, std::cout);
    }
    return exitOk;
}

// Flushes a batch report: stderr blob, JSON payload under --json,
// stdout blob in text mode, then the recorded exit code. JSON mode keeps the
// exit code: a failed/partial batch emits JSON on stdout *and* exits non-zero.
int renderBatch(kanariv::app::BatchResult res, bool asJson) {
    if (!res)
        return renderError(res.error());
    auto& rep = *res;
    if (!rep.err.empty())
        kanariv::println(std::cerr, "{}", rep.err);
    if (asJson) {
        if (rep.json)
            renderJson(*rep.json);
        return rep.exitCode;
    }
    if (!rep.out.empty())
        kanariv::println(std::cout, "{}", rep.out);
    return rep.exitCode;
}

// Prints version plus build provenance for the `version` subcommand. Human
// output is a labelled block; --json emits one flat object.
int renderVersion(bool asJson) {
#if defined(__clang__)
    const std::string compiler = std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
    const std::string compiler = std::string("MSVC ") + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    const std::string compiler = std::string("GCC ") + __VERSION__;
#else
    const std::string compiler = "unknown";
#endif
#if defined(_LIBCPP_VERSION)
    constexpr std::string_view runtime = "libc++";
#elif defined(_MSVC_STL_VERSION)
    constexpr std::string_view runtime = "MSVC STL";
#elif defined(__GLIBCXX__)
    constexpr std::string_view runtime = "libstdc++";
#else
    constexpr std::string_view runtime = "unknown";
#endif
#if defined(_WIN32)
    constexpr std::string_view platform = "windows";
#elif defined(__APPLE__)
    constexpr std::string_view platform = "macos";
#elif defined(__linux__)
    constexpr std::string_view platform = "linux";
#else
    constexpr std::string_view platform = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    constexpr std::string_view arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    constexpr std::string_view arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    constexpr std::string_view arch = "x86";
#else
    constexpr std::string_view arch = "unknown";
#endif
#if defined(NDEBUG)
    constexpr std::string_view build = "release";
#else
    constexpr std::string_view build = "debug";
#endif
    const std::string full(kanariv::version::version());
    const std::string shortv(kanariv::version::shortVersion());
    const std::string commit(kanariv::version::versionCommit());
    const std::string stdv = std::to_string(__cplusplus);
    if (asJson) {
        kanariv::println(
            R"({{"version":"{}","short":"{}","commit":"{}","compiler":"{}","std":"{}","runtime":"{}","build":"{}","os":"{}","arch":"{}"}})",
            full, shortv, commit, compiler, stdv, runtime, build, platform, arch);
        return 0;
    }
    kanariv::println("kanariv {}", full);
    kanariv::println("  short:    {}", shortv);
    kanariv::println("  commit:   {}", commit);
    kanariv::println("  compiler: {}", compiler);
    kanariv::println("  std:      {}", stdv);
    kanariv::println("  runtime:  {}", runtime);
    kanariv::println("  build:    {}", build);
    kanariv::println("  os:       {}", platform);
    kanariv::println("  arch:     {}", arch);
    return 0;
}

} // namespace kanariv::app::cli::detail
