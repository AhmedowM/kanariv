#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <kanariv/utils/print.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utils/log_sink.hpp>

namespace kanariv::utils {

namespace {

auto levelTag(LogLevel lvl) noexcept -> std::string_view {
    switch (lvl) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO ";
    case LogLevel::Warn:
        return "WARN ";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "?????";
    }
}

auto formatLine(LogLevel lvl, std::string_view msg) -> std::string {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    // Low 24 bits distinguish the daemon's handful of threads; the full
    // 64-bit hash is visual noise in every line.
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffu;
    return std::format("[{:%FT%TZ}] [{}] [t:{:06x}] {}\n", now, levelTag(lvl), tid, msg);
}

} // namespace

// All sink state lives behind the pimpl so the public header stays free of
// <fstream>/<mutex> and the filesystem backend.
struct FileLogSink::Impl {
    mutable std::mutex mutex;
    std::ofstream out;
    std::filesystem::path path;
    FileSinkOptions opts;
    std::size_t currentBytes{0};
    bool valid{false};

    void writeLine(LogLevel lvl, std::string_view msg);
    void rotate();
    auto indexedPath(int i) const -> std::filesystem::path;
};

FileLogSink::FileLogSink() : impl_(std::make_unique<Impl>()) {}
FileLogSink::~FileLogSink() = default;

auto FileLogSink::open(const std::filesystem::path& path, const FileSinkOptions& opts)
    -> std::shared_ptr<FileLogSink> {
    auto sink = std::shared_ptr<FileLogSink>(new FileLogSink());
    Impl& impl = *sink->impl_;
    impl.path = path;
    impl.opts = opts;
    impl.opts.generations = std::max(impl.opts.generations, 1);
    impl.out.open(path, std::ios::out | std::ios::app);
    if (!impl.out.is_open()) {
        return nullptr;
    }
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    impl.currentBytes = ec ? 0 : static_cast<std::size_t>(size);
    impl.valid = true;
    if (impl.currentBytes > impl.opts.maxBytes) {
        std::scoped_lock lk(impl.mutex);
        impl.rotate();
    }
    return sink;
}

auto FileLogSink::callback() -> Logger::Callback {
    std::shared_ptr<FileLogSink> self = shared_from_this();
    return [self](LogLevel lvl, std::string_view msg) -> void { self->impl_->writeLine(lvl, msg); };
}

auto FileLogSink::valid() const noexcept -> bool {
    std::scoped_lock lk(impl_->mutex);
    return impl_->valid;
}

void FileLogSink::flush() {
    std::scoped_lock lk(impl_->mutex);
    if (impl_->valid) {
        impl_->out.flush();
    }
}

void FileLogSink::Impl::writeLine(LogLevel lvl, std::string_view msg) {
    std::string line;
    try {
        line = formatLine(lvl, msg);
    } catch (...) {
        return;
    }
    std::scoped_lock lk(mutex);
    if (!valid) {
        return;
    }
    try {
        if (currentBytes + line.size() > opts.maxBytes) {
            rotate();
        }
        if (!valid) {
            return;
        }
        out << line;
        out.flush();
        if (!out) {
            valid = false;
        } else {
            currentBytes += line.size();
        }
    } catch (...) {
        valid = false;
    }
    if (opts.teeStderr) {
        try {
            kanariv::println(std::cerr, "[{}] {}", levelTag(lvl), msg);
        } catch (...) {
        }
    }
}

void FileLogSink::Impl::rotate() {
    // Caller holds mutex. Failures degrade to truncate-and-continue so the
    // bound (generations * maxBytes) always holds.
    try {
        out.close();
        const int gens = opts.generations;
        std::error_code ec;
        std::filesystem::remove(indexedPath(gens - 1), ec);
        for (int i = gens - 2; i >= 0; --i) {
            std::filesystem::rename(indexedPath(i), indexedPath(i + 1), ec);
        }
        out.open(path, std::ios::out | std::ios::trunc);
        valid = out.is_open();
        currentBytes = 0;
    } catch (...) {
        valid = false;
    }
}

auto FileLogSink::Impl::indexedPath(int i) const -> std::filesystem::path {
    if (i <= 0) {
        return path;
    }
    return std::filesystem::path(path.string() + "." + std::to_string(i));
}

} // namespace kanariv::utils
