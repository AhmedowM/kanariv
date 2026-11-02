#include <stdio.h>

#include <algorithm>
#include <kanariv/media/reader.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kanariv::media {

namespace detail {
// 64-bit file helpers (TU-local; were inline in reader.hpp).
int64_t ftell64(FILE* f) noexcept {
#if defined(_WIN32)
    return _ftelli64(f);
#else
    off_t o = ftello(f);
    if (o == (off_t)-1)
        return -1;
    return static_cast<int64_t>(o);
#endif
}

int fseek64(FILE* f, int64_t off, int whence) noexcept {
#if defined(_WIN32)
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, static_cast<off_t>(off), whence);
#endif
}

int64_t fileSizeInner(FILE* f) noexcept {
    int64_t cur = ftell64(f);
    if (cur < 0)
        return -1;
    if (fseek64(f, 0, SEEK_END) != 0)
        return -1;
    int64_t sz = ftell64(f);
    if (sz < 0)
        return -1;
    if (fseek64(f, cur, SEEK_SET) != 0)
        return -1;
    return sz;
}

// Descriptor helpers (TU-local; POSIX/Windows CRT only, no Android headers).
int64_t fdTell64(int fd) noexcept {
#if defined(_WIN32)
    return _lseeki64(fd, 0, SEEK_CUR);
#else
    off_t o = lseek(fd, 0, SEEK_CUR);
    if (o == (off_t)-1)
        return -1;
    return static_cast<int64_t>(o);
#endif
}

int64_t fdSeek64(int fd, int64_t off, int whence) noexcept {
#if defined(_WIN32)
    return _lseeki64(fd, off, whence);
#else
    off_t o = lseek(fd, static_cast<off_t>(off), whence);
    if (o == (off_t)-1)
        return -1;
    return static_cast<int64_t>(o);
#endif
}

int64_t fdSize(int fd) noexcept {
#if defined(_WIN32)
    struct _stat64 st{};
    if (_fstat64(fd, &st) != 0)
        return -1;
    return static_cast<int64_t>(st.st_size);
#else
    struct stat st{};
    if (fstat(fd, &st) != 0)
        return -1;
    return static_cast<int64_t>(st.st_size);
#endif
}

int64_t fdRead(int fd, std::byte* dst, std::size_t len) noexcept {
#if defined(_WIN32)
    const unsigned int chunk =
        static_cast<unsigned int>(std::min<std::size_t>(len, std::numeric_limits<int>::max()));
    return static_cast<int64_t>(_read(fd, dst, chunk));
#else
    return static_cast<int64_t>(::read(fd, dst, len));
#endif
}

void fdClose(int fd) noexcept {
#if defined(_WIN32)
    (void)_close(fd);
#else
    (void)::close(fd);
#endif
}
} // namespace detail

/// File handle, cached size and seek lock, hidden from the public header.
struct FileReader::Impl {
    FILE* file = nullptr;
    int64_t fileSize = -1;
    mutable std::mutex seekMutex;
};

kanariv::utils::Expected<std::unique_ptr<Reader>>
FileReader::open(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("empty path")});
    }
    FILE* f = nullptr;
#if defined(_WIN32)
    f = _wfopen(path.wstring().c_str(), L"rb");
    if (!f) {
        // fallback to narrow
        std::string s = path.string();
        f = std::fopen(s.c_str(), "rb");
    }
#else
    std::string s = path.string();
    f = std::fopen(s.c_str(), "rb");
#endif
    if (!f) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::NotFound,
                                                    std::string_view("cannot open file")});
    }
    if (detail::fseek64(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Internal,
                                                    std::string_view("fseek failed")});
    }
    int64_t sz = detail::ftell64(f);
    if (sz < 0) {
        std::fclose(f);
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Internal,
                                                    std::string_view("ftell failed")});
    }
    if (detail::fseek64(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Internal,
                                                    std::string_view("fseek failed")});
    }
    // Wrap immediately in unique_ptr so FILE* is owned even if Expected construction throws
    auto holder = std::unique_ptr<FileReader>(new FileReader(FileToken{}));
    holder->impl_->file = f;
    holder->impl_->fileSize = sz;
    std::unique_ptr<Reader> base = std::move(holder);
    return kanariv::utils::Expected<std::unique_ptr<Reader>>{std::move(base)};
}

FileReader::~FileReader() {
    if (impl_->file)
        std::fclose(impl_->file);
}

std::size_t FileReader::read(std::span<std::byte> dst) {
    if (!impl_->file || dst.empty())
        return 0;
    return std::fread(dst.data(), 1, dst.size(), impl_->file);
}

kanariv::utils::Expected<void> FileReader::seek(int64_t offset, int whence) {
    if (!impl_->file)
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("null file")});
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad whence")});
    }
    std::lock_guard<std::mutex> lock(impl_->seekMutex);
    int64_t cur = detail::ftell64(impl_->file);
    if (cur < 0)
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Internal,
                                                    std::string_view("ftell failed")});
    int64_t sz = impl_->fileSize;
    if (sz < 0)
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Internal,
                                                    std::string_view("size not cached")});
    int64_t base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = cur;
        break;
    case SEEK_END:
        base = sz;
        break;
    default:
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad whence")});
    }
    int64_t newPos = base + offset;
    if (newPos < 0 || newPos > sz) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("seek out of range")});
    }
    if (detail::fseek64(impl_->file, newPos, SEEK_SET) != 0) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Internal,
                                                    std::string_view("fseek failed")});
    }
    return {};
}

int64_t FileReader::tell() noexcept {
    if (!impl_->file)
        return -1;
    std::lock_guard<std::mutex> lock(impl_->seekMutex);
    return detail::ftell64(impl_->file);
}

int64_t FileReader::size() noexcept {
    return impl_->fileSize;
}

bool FileReader::seekable() const noexcept {
    return true;
}

FileReader::FileReader(FileToken) : impl_(std::make_unique<Impl>()) {}

/// Owned/borrowed descriptor, tracked position, cached size and seek lock.
struct DescriptorReader::Impl {
    int fd{-1};
    bool seekable{false};
    bool owned{false};
    int64_t size{-1};
    int64_t pos{0};
    mutable std::mutex mutex;
};

kanariv::utils::Expected<std::unique_ptr<Reader>> DescriptorReader::open(FileDescriptor descriptor) {
    if (descriptor.value < 0) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("invalid descriptor")});
    }
    if (!descriptor.readable) {
        return std::unexpected(kanariv::utils::Error{
            kanariv::utils::StatusCode::InvalidArg, std::string_view("descriptor is not readable")});
    }

    int64_t size = -1;
    int64_t pos = 0;
    if (descriptor.seekable) {
        pos = detail::fdTell64(descriptor.value);
        if (pos < 0) {
            // The caller asserted seekability; fail without taking ownership.
            return std::unexpected(
                kanariv::utils::Error{kanariv::utils::StatusCode::Unsupported,
                                     std::string_view("descriptor position unavailable")});
        }
        size = detail::fdSize(descriptor.value);
    }

    auto holder =
        std::unique_ptr<DescriptorReader>(new DescriptorReader(DescriptorToken{}, descriptor));
    holder->impl_->pos = pos;
    holder->impl_->size = size;
    std::unique_ptr<Reader> base = std::move(holder);
    return kanariv::utils::Expected<std::unique_ptr<Reader>>{std::move(base)};
}

DescriptorReader::DescriptorReader(DescriptorToken, FileDescriptor descriptor)
    : impl_(std::make_unique<Impl>()) {
    impl_->fd = descriptor.value;
    impl_->seekable = descriptor.seekable;
    impl_->owned = descriptor.closeOnDestroy;
}

DescriptorReader::~DescriptorReader() {
    if (impl_ && impl_->owned && impl_->fd >= 0)
        detail::fdClose(impl_->fd);
    if (impl_)
        impl_->fd = -1;
}

std::size_t DescriptorReader::read(std::span<std::byte> dst) {
    if (!impl_ || impl_->fd < 0 || dst.empty())
        return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (;;) {
        int64_t n = detail::fdRead(impl_->fd, dst.data(), dst.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        impl_->pos += n;
        return static_cast<std::size_t>(n);
    }
}

kanariv::utils::Expected<void> DescriptorReader::seek(int64_t offset, int whence) {
    if (!impl_ || impl_->fd < 0) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("invalid descriptor")});
    }
    if (!impl_->seekable) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Unsupported,
                                                    std::string_view("source is not seekable")});
    }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad whence")});
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    int64_t base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = impl_->pos;
        break;
    case SEEK_END:
        if (impl_->size < 0) {
            return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Unsupported,
                                                        std::string_view("size is unknown")});
        }
        base = impl_->size;
        break;
    default:
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad whence")});
    }
    int64_t newPos = base + offset;
    if (newPos < 0 || (impl_->size >= 0 && newPos > impl_->size)) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("seek out of range")});
    }
    int64_t landed = detail::fdSeek64(impl_->fd, newPos, SEEK_SET);
    if (landed < 0) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::Io,
                                                    std::string_view("descriptor seek failed")});
    }
    impl_->pos = landed;
    return {};
}

int64_t DescriptorReader::tell() noexcept {
    if (!impl_ || impl_->fd < 0)
        return -1;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->pos;
}

int64_t DescriptorReader::size() noexcept {
    return impl_ ? impl_->size : -1;
}

bool DescriptorReader::seekable() const noexcept {
    return impl_ && impl_->seekable;
}

kanariv::utils::Expected<std::unique_ptr<Reader>>
MemoryReader::open(std::span<const std::byte> data) {
    // copy data to owned buffer -- wrap immediately for exception safety
    auto holder = std::unique_ptr<MemoryReader>(new MemoryReader(data));
    std::unique_ptr<Reader> base = std::move(holder);
    return kanariv::utils::Expected<std::unique_ptr<Reader>>{std::move(base)};
}

kanariv::utils::Expected<std::unique_ptr<Reader>>
MemoryReader::open(const std::vector<std::byte>& data) {
    return open(std::span<const std::byte>(data.data(), data.size()));
}

kanariv::utils::Expected<std::unique_ptr<Reader>> MemoryReader::open(const std::byte* data,
                                                                    std::size_t len) {
    if (len > 0 && data == nullptr) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("null data")});
    }
    return open(std::span<const std::byte>(data, len));
}

std::size_t MemoryReader::read(std::span<std::byte> dst) {
    if (dst.empty())
        return 0;
    std::size_t avail = buf_.size() > pos_ ? buf_.size() - pos_ : 0;
    std::size_t n = dst.size() < avail ? dst.size() : avail;
    if (n > 0) {
        std::memcpy(dst.data(), buf_.data() + pos_, n);
        pos_ += n;
    }
    return n;
}

kanariv::utils::Expected<void> MemoryReader::seek(int64_t offset, int whence) {
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad whence")});
    }
    int64_t base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = static_cast<int64_t>(pos_);
        break;
    case SEEK_END:
        base = static_cast<int64_t>(buf_.size());
        break;
    default:
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("bad whence")});
    }
    int64_t newPos = base + offset;
    if (newPos < 0 || newPos > static_cast<int64_t>(buf_.size())) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::InvalidArg,
                                                    std::string_view("seek out of range")});
    }
    pos_ = static_cast<std::size_t>(newPos);
    return {};
}

int64_t MemoryReader::tell() noexcept {
    return static_cast<int64_t>(pos_);
}

int64_t MemoryReader::size() noexcept {
    return static_cast<int64_t>(buf_.size());
}

bool MemoryReader::seekable() const noexcept {
    return true;
}

MemoryReader::MemoryReader(std::span<const std::byte> src)
    : buf_(src.begin(), src.end()), pos_(0) {}

} // namespace kanariv::media
