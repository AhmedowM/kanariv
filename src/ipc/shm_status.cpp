#include <algorithm>
#include <atomic>
#include <kanariv/ipc/shm_status.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/status_format.hpp>
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

namespace {

// Raw SHM handle fields filled by the open/create helper.
struct ShmRaw {
    AtomicShmStatus* map{nullptr};
    std::size_t size{0};
    std::string name;
    std::intptr_t handle{-1};
    bool reset{false}; ///< True when the mapping must be re-initialized.
};

// Maximum number of seqlock retries before a reader declares the writer
// crashed/wedged. A live publish() only holds an odd sequence for a few
// hundred byte stores, so this budget is generous for real contention while
// keeping a dead writer from pinning a client core.
constexpr std::uint32_t snapshotAttempts = 1024;

// Publishes `src` into a fixed seqlock-protected byte buffer. Always rewrites
// the whole buffer: the payload, a terminator, and zero padding. Byte-wise
// atomic stores keep the concurrent access defined (no data race) while the
// surrounding seqlock rejects torn snapshots.
void publishFixedString(char* dest, std::size_t capacity, std::string_view src) noexcept {
    const std::size_t n = std::min(src.size(), capacity - 1);
    for (std::size_t i = 0; i < n; ++i)
        std::atomic_ref<char>(dest[i]).store(src[i], std::memory_order_relaxed);
    for (std::size_t i = n; i < capacity; ++i)
        std::atomic_ref<char>(dest[i]).store('\0', std::memory_order_relaxed);
}

// Copies a seqlock-protected byte buffer with atomic loads (race-free).
void copyFixedString(char* dest, char* src, std::size_t capacity) noexcept {
    for (std::size_t i = 0; i < capacity; ++i)
        dest[i] = std::atomic_ref<char>(src[i]).load(std::memory_order_relaxed);
}

// Re-initializes a freshly created/replaced segment. Uses an odd/even sequence
// so a lingering reader from a crashed daemon cannot adopt half-reset fields.
void resetStatus(AtomicShmStatus* s) noexcept {
    s->seq.store(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    s->position.store(0, std::memory_order_relaxed);
    s->duration.store(0, std::memory_order_relaxed);
    s->volume.store(0, std::memory_order_relaxed);
    s->muted.store(false, std::memory_order_relaxed);
    s->state.store(0, std::memory_order_relaxed);
    s->track_id.store(0, std::memory_order_relaxed);
    s->queueSize.store(0, std::memory_order_relaxed);
    publishFixedString(s->title, sizeof(s->title), {});
    publishFixedString(s->artist, sizeof(s->artist), {});

    std::atomic_thread_fence(std::memory_order_release);
    s->seq.store(0, std::memory_order_release);
}

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
        flags |= O_CREAT | O_EXCL;
    int fd = ::shm_open(fullName.c_str(), flags, 0600);
    if (fd < 0 && create && errno == EEXIST) {
        // Stale segment from a crashed daemon. The single-instance lock already
        // guarantees no live writer owns it, so replace it with a fresh one.
        if (::shm_unlink(fullName.c_str()) != 0 && errno != ENOENT) {
            return "shm_unlink failed: " + std::string(std::strerror(errno));
        }
        fd = ::shm_open(fullName.c_str(), flags, 0600);
    }
    if (fd < 0) {
        return "shm_open failed: " + std::string(std::strerror(errno));
    }

    if (create) {
        if (::ftruncate(fd, sizeof(AtomicShmStatus)) != 0) {
            ::close(fd);
            ::shm_unlink(fullName.c_str());
            return "ftruncate failed: " + std::string(std::strerror(errno));
        }
        raw.reset = true;
    } else {
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            return "fstat failed: " + std::string(std::strerror(errno));
        }
        if (static_cast<std::size_t>(st.st_size) < sizeof(AtomicShmStatus)) {
            ::close(fd);
            return "shared memory status segment has an incompatible size";
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
        // 183 == ERROR_ALREADY_EXISTS. A stale/lingering mapping survives when
        // a reader still holds it after a daemon crash; a fresh one is already
        // zero-filled, so only the pre-existing mapping needs an explicit reset.
        constexpr DWORD errorAlreadyExists = 183UL;
        hMap = ::CreateFileMappingW(invalidHandleValue, nullptr, protect, 0,
                                    sizeof(AtomicShmStatus), wname.c_str());
        if (hMap && ::GetLastError() == errorAlreadyExists)
            raw.reset = true;
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
    // A create must never inherit a crashed writer's odd sequence or stale
    // metadata: reset the header before the writer becomes usable.
    if (raw.reset)
        resetStatus(raw.map);
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
    for (uint32_t attempt = 0; attempt < snapshotAttempts; ++attempt) {
        const uint64_t seq = s->seq.load(std::memory_order_acquire);
        if (seq & 1) {
            // Writer busy (or crashed mid-publish): yield occasionally, retry.
            if ((attempt & 7U) == 7U)
                std::this_thread::yield();
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        out.seq = seq;
        out.position = atomicLoadDouble(s->position);
        out.duration = atomicLoadDouble(s->duration);
        out.volume = atomicLoadFloat(s->volume);
        out.muted = s->muted.load(std::memory_order_relaxed);
        out.state = s->state.load(std::memory_order_relaxed);
        out.track_id = s->track_id.load(std::memory_order_relaxed);
        out.queueSize = s->queueSize.load(std::memory_order_relaxed);
        copyFixedString(out.title, s->title, sizeof(out.title));
        copyFixedString(out.artist, s->artist, sizeof(out.artist));
        std::atomic_thread_fence(std::memory_order_acquire);
        // Verify no writer intervened.
        if (s->seq.load(std::memory_order_relaxed) == seq)
            return out;
    }
    // Bounded retry budget exhausted: a crashed writer leaves `seq` odd
    // forever, so report a zeroed snapshot with `stale` set instead of
    // spinning at 100% CPU.
    out = ShmStatus{};
    out.stale = true;
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

// Seqlock writer: publishes the complete plain update under an odd/even sequence.
void ShmStatusWriter::publish(const ShmStatusUpdate& u) noexcept {
    if (!map_)
        return;
    AtomicShmStatus* s = map_;
    uint64_t old = s->seq.load(std::memory_order_relaxed);
    // Ensure old is even (recover from a prior torn/crashed publish).
    if (old & 1)
        ++old;

    // Mark as writing (odd). The full fence keeps the data stores below from
    // becoming visible before readers observe the odd sequence.
    s->seq.store(old + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    atomicStoreDouble(s->position, u.position);
    atomicStoreDouble(s->duration, u.duration);
    atomicStoreFloat(s->volume, u.volume);
    s->state.store(u.state, std::memory_order_relaxed);
    s->track_id.store(u.track_id, std::memory_order_relaxed);
    s->queueSize.store(u.queueSize, std::memory_order_relaxed);
    s->muted.store(u.muted, std::memory_order_relaxed);
    // Always rewrite both strings: an empty view clears the previous value.
    publishFixedString(s->title, sizeof(s->title), u.title);
    publishFixedString(s->artist, sizeof(s->artist), u.artist);

    // Mark as done (even). The release fence makes every store above visible
    // to readers that observe this even sequence.
    std::atomic_thread_fence(std::memory_order_release);
    s->seq.store(old + 2, std::memory_order_release);
}

void ShmStatusWriter::close() noexcept {
    closeShmRaw(map_, size_, handle_, create_, name_);
}

} // namespace kanariv::ipc
