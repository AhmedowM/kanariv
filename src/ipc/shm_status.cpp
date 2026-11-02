#include <algorithm>
#include <atomic>
#include <bit>
#include <kanariv/ipc/shm_status.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/windows_fwd.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#if !defined(_WIN32) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kanariv::ipc {

// Helpers for bit_cast atomic operations
double atomicLoadDouble(const std::atomic<uint64_t>& a) noexcept {
    return std::bit_cast<double>(a.load(std::memory_order_acquire));
}
void atomicStoreDouble(std::atomic<uint64_t>& a, double v) noexcept {
    a.store(std::bit_cast<uint64_t>(v), std::memory_order_relaxed);
}
float atomicLoadFloat(const std::atomic<uint32_t>& a) noexcept {
    return std::bit_cast<float>(a.load(std::memory_order_acquire));
}
void atomicStoreFloat(std::atomic<uint32_t>& a, float v) noexcept {
    a.store(std::bit_cast<uint32_t>(v), std::memory_order_relaxed);
}

namespace {

// Raw SHM handle fields filled by the open/create helper.
struct ShmRaw {
    AtomicShmStatus* map{nullptr};
    std::size_t size{0};
    std::string name;
    std::intptr_t handle{-1};
};

// Opens or creates a shared-memory segment and fills `raw`. Returns an error
// description on failure (empty string on success).
std::string openShmRaw(const std::string& name, bool create, ShmRaw& raw) {
#ifdef _WIN32
    std::string fullName = "kanariv-" + name + "-status";
#elif defined(__ANDROID__)
    (void)name;
    (void)create;
    (void)raw;
    return "shared memory status is unavailable on Android";
#else
    std::string fullName = "/kanariv-" + name + "-status";
    int flags = O_RDWR;
    if (create)
        flags |= O_CREAT;
    int fd = ::shm_open(fullName.c_str(), flags, 0600);
    if (fd < 0) {
        return "shm_open failed: " + std::string(std::strerror(errno));
    }

    if (create) {
        if (::ftruncate(fd, sizeof(AtomicShmStatus)) != 0) {
            ::close(fd);
            ::shm_unlink(fullName.c_str());
            return "ftruncate failed: " + std::string(std::strerror(errno));
        }
    }

    void* ptr = ::mmap(nullptr, sizeof(AtomicShmStatus), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        ::close(fd);
        if (create)
            ::shm_unlink(fullName.c_str());
        return "mmap failed: " + std::string(std::strerror(errno));
    }
    raw.map = static_cast<AtomicShmStatus*>(ptr);
    raw.size = sizeof(AtomicShmStatus);
    raw.handle = fd;
#endif

#ifdef _WIN32
    std::wstring wname;
    wname.reserve(fullName.size());
    for (char c : fullName)
        wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));

    DWORD access = fileMapRead | fileMapWrite;
    DWORD protect = pageReadWrite;
    HANDLE hMap = nullptr;

    if (create) {
        hMap = ::CreateFileMappingW(invalidHandleValue, nullptr, protect, 0,
                                    sizeof(AtomicShmStatus), wname.c_str());
    } else {
        hMap = ::OpenFileMappingW(access, boolFalse, wname.c_str());
    }

    if (!hMap) {
        return "CreateFileMapping/OpenFileMapping failed: " + std::to_string(::GetLastError());
    }

    void* ptr = ::MapViewOfFile(hMap, access, 0, 0, sizeof(AtomicShmStatus));
    if (!ptr) {
        ::CloseHandle(hMap);
        return "MapViewOfFile failed: " + std::to_string(::GetLastError());
    }
    raw.map = static_cast<AtomicShmStatus*>(ptr);
    raw.size = sizeof(AtomicShmStatus);
    raw.handle = reinterpret_cast<std::intptr_t>(hMap);
#endif

#ifndef __ANDROID__
    raw.name = std::move(fullName);
#endif
    return "";
}

// Closes a raw SHM handle.
void closeShmRaw(AtomicShmStatus*& map, std::size_t& size, std::intptr_t& handle, bool created,
                 const std::string& name) noexcept {
    (void)size;
    (void)created;
    (void)name;
    if (map) {
#if !defined(_WIN32) && !defined(__ANDROID__)
        ::munmap(map, size);
        map = nullptr;
        if (handle >= 0) {
            if (created) {
                ::shm_unlink(name.c_str());
            }
            ::close(static_cast<int>(handle));
            handle = -1;
        }
#elif defined(_WIN32)
        ::UnmapViewOfFile(map);
        map = nullptr;
        if (handle != -1) {
            ::CloseHandle(reinterpret_cast<HANDLE>(handle));
            handle = -1;
        }
#else
        map = nullptr;
        size = 0;
        handle = -1;
#endif
    }
}

} // namespace

ShmStatusReader::ExpectedShm ShmStatusReader::openReadOnly(const std::string& name) {
    ShmRaw raw;
    std::string err = openShmRaw(name, false, raw);
    if (!err.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, err)};
    ShmStatusReader reader;
    reader.map_ = raw.map;
    reader.size_ = raw.size;
    reader.name_ = std::move(raw.name);
    reader.handle_ = raw.handle;
    return reader;
}

ShmStatusReader::~ShmStatusReader() {
    closeShmRaw(map_, size_, handle_, false, name_);
}

ShmStatusReader::ShmStatusReader(ShmStatusReader&& other) noexcept
    : map_(other.map_), size_(other.size_), name_(std::move(other.name_)), handle_(other.handle_) {
    other.map_ = nullptr;
    other.size_ = 0;
    other.handle_ = -1;
}

ShmStatusReader& ShmStatusReader::operator=(ShmStatusReader&& other) noexcept {
    if (this != &other) {
        closeShmRaw(map_, size_, handle_, false, name_);
        map_ = other.map_;
        size_ = other.size_;
        name_ = std::move(other.name_);
        handle_ = other.handle_;
        other.map_ = nullptr;
        other.size_ = 0;
        other.handle_ = -1;
    }
    return *this;
}

bool ShmStatusReader::valid() const noexcept {
    return map_ != nullptr;
}

const std::string& ShmStatusReader::name() const noexcept {
    return name_;
}

// Seqlock reader: returns a snapshot copy of the status
ShmStatus ShmStatusReader::snapshot() const {
    ShmStatus out{};
    if (!map_)
        return out;
    AtomicShmStatus* s = map_;
    uint32_t spinCount = 0;
    while (true) {
        uint64_t seq = s->seq.load(std::memory_order_acquire);
        if (seq & 1) {
            // Writer busy, retry with occasional yield
            if (++spinCount >= 3) {
                std::this_thread::yield();
                spinCount = 0;
            }
            continue;
        }
        out.seq = seq;
        out.position = atomicLoadDouble(s->position);
        out.duration = atomicLoadDouble(s->duration);
        out.volume = atomicLoadFloat(s->volume);
        out.muted = s->muted.load(std::memory_order_acquire);
        out.state = s->state.load(std::memory_order_acquire);
        out.track_id = s->track_id.load(std::memory_order_acquire);
        out.queueSize = s->queueSize.load(std::memory_order_acquire);
        // Copy strings (protected by seqlock)
        std::memcpy(out.title, s->title, sizeof(out.title));
        std::memcpy(out.artist, s->artist, sizeof(out.artist));
        // Verify no writer intervened
        if (s->seq.load(std::memory_order_acquire) == seq) {
            break;
        }
    }
    return out;
}

void ShmStatusReader::close() noexcept {
    closeShmRaw(map_, size_, handle_, false, name_);
}

ShmStatusWriter::ExpectedShm ShmStatusWriter::create(const std::string& name) {
    ShmRaw raw;
    std::string err = openShmRaw(name, true, raw);
    if (!err.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, err)};
    ShmStatusWriter writer;
    writer.map_ = raw.map;
    writer.size_ = raw.size;
    writer.name_ = std::move(raw.name);
    writer.create_ = true;
    writer.handle_ = raw.handle;
    return writer;
}

ShmStatusWriter::~ShmStatusWriter() {
    closeShmRaw(map_, size_, handle_, create_, name_);
}

ShmStatusWriter::ShmStatusWriter(ShmStatusWriter&& other) noexcept
    : map_(other.map_), size_(other.size_), name_(std::move(other.name_)), create_(other.create_),
      handle_(other.handle_) {
    other.map_ = nullptr;
    other.size_ = 0;
    other.handle_ = -1;
    other.create_ = false;
}

ShmStatusWriter& ShmStatusWriter::operator=(ShmStatusWriter&& other) noexcept {
    if (this != &other) {
        closeShmRaw(map_, size_, handle_, create_, name_);
        map_ = other.map_;
        size_ = other.size_;
        name_ = std::move(other.name_);
        create_ = other.create_;
        handle_ = other.handle_;
        other.map_ = nullptr;
        other.size_ = 0;
        other.handle_ = -1;
        other.create_ = false;
    }
    return *this;
}

bool ShmStatusWriter::valid() const noexcept {
    return map_ != nullptr;
}

const std::string& ShmStatusWriter::name() const noexcept {
    return name_;
}

// Seqlock writer: publishes the plain update under an odd/even sequence.
void ShmStatusWriter::publish(const ShmStatusUpdate& u) noexcept {
    if (!map_)
        return;
    AtomicShmStatus* s = map_;
    uint64_t old = s->seq.load(std::memory_order_acquire);
    // Ensure old is even
    if (old & 1)
        old++;

    // Mark as writing (odd)
    s->seq.store(old + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    atomicStoreDouble(s->position, u.position);
    atomicStoreFloat(s->volume, u.volume);
    s->state.store(u.state, std::memory_order_relaxed);
    s->track_id.store(u.track_id, std::memory_order_relaxed);
    s->muted.store(u.muted, std::memory_order_relaxed);

    // Copy strings with bounds checking
    if (!u.title.empty()) {
        std::size_t n = std::min(u.title.size(), sizeof(s->title) - 1);
        std::memcpy(s->title, u.title.data(), n);
        s->title[n] = '\0';
    }
    if (!u.artist.empty()) {
        std::size_t n = std::min(u.artist.size(), sizeof(s->artist) - 1);
        std::memcpy(s->artist, u.artist.data(), n);
        s->artist[n] = '\0';
    }

    // Mark as done (even)
    s->seq.store(old + 2, std::memory_order_release);
}

void ShmStatusWriter::setDuration(double dur) noexcept {
    if (!map_)
        return;
    atomicStoreDouble(map_->duration, dur);
}

void ShmStatusWriter::setQueueSize(size_t sz) noexcept {
    if (!map_)
        return;
    map_->queueSize.store(sz, std::memory_order_relaxed);
}

void ShmStatusWriter::close() noexcept {
    closeShmRaw(map_, size_, handle_, create_, name_);
}

} // namespace kanariv::ipc
