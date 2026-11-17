#include <kanariv/config.hpp>
#include <kanariv/utils/status_format.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <utility>

#include "shell.hpp"
#include "signals.hpp"

// Debug-only abort tripwire (all platforms): abort()/terminate is exactly when
// evidence is hardest to obtain, so record pid + exception type in a log file
// before dying. Release builds compile this out (zero cost). On the Windows
// Debug CRT this also replaces the modal dialog that would otherwise block the
// process and look like a deaf daemon or a hung CLI.
#if !defined(NDEBUG)
#if defined(_WIN32)
#include <crtdbg.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
FILE* g_abortLog() {
    static FILE* f = []() -> FILE* {
        try {
            auto p = std::filesystem::temp_directory_path() / "kanariv-abort.log";
            FILE* fh = nullptr;
#ifdef _WIN32
            _wfopen_s(&fh, p.c_str(), L"a");
#else
            fh = std::fopen(p.string().c_str(), "a");
#endif
            return fh;
        } catch (...) {
            return nullptr;
        }
    }();
    return f;
}
void g_logAbort(const char* what) {
    if (FILE* f = g_abortLog()) {
#ifdef _WIN32
        std::fprintf(f, "pid=%lu %s\n", static_cast<unsigned long>(::_getpid()), what);
#else
        std::fprintf(f, "pid=%ld %s\n", static_cast<long>(::getpid()), what);
#endif
        std::fflush(f);
    }
}
void g_terminateDiag() {
    try {
        throw;
    } catch (const std::exception& e) {
        std::string m = std::string("terminate: std::exception: ") + e.what();
        g_logAbort(m.c_str());
    } catch (...) {
        g_logAbort("terminate: unknown non-std exception");
    }
    std::abort();
}
} // namespace
#endif

int main(int argc, char** argv) {
#if defined(_WIN32) && defined(_DEBUG) && !defined(NDEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, g_abortLog());
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
#if !defined(NDEBUG)
    std::set_terminate(g_terminateDiag);
#endif
    // Install SIGINT/SIGTERM (POSIX) or a console control handler (Windows)
    // before anything can block; every long-running loop observes the token.
    kanariv::app::cli::detail::ProcessStop processStop;

    // Load default config (XDG or temp). If path empty, use default.
    auto cfgExp = kanariv::config::loadConfig(std::filesystem::path{});
    kanariv::config::Config cfg;
    if (cfgExp) {
        cfg = std::move(*cfgExp);
    } else {
        std::cerr << std::format("warning: loadConfig failed ({}): {}\n",
                                 kanariv::utils::toString(cfgExp.error().code),
                                 cfgExp.error().message);
        cfg.dbPath = std::filesystem::path("library.db");
        cfg.configPath = std::filesystem::path{};
        cfg.device = "auto";
        cfg.logLevel = 2;
    }

    kanariv::app::cli::Shell shell{std::move(cfg), processStop.token()};
    return shell.run(argc, argv);
}
