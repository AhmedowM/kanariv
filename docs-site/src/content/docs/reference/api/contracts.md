---
title: Contracts — caudio::contracts
description: Backend-neutral persistence interface (IDatabase)
---

# Contracts — `caudio::contracts`

The `caudio::contracts` component defines `IDatabase`, a backend-neutral
persistence interface used by the engine and service layers.

## IDatabase

```cpp
#include <caudio/contracts/database.hpp>

class IDatabase {
public:
    virtual ~IDatabase() = default;

    // Connection
    virtual bool isOpen() const noexcept = 0;
    virtual caudio::utils::Logger* logger() const noexcept = 0;

    // Tracks
    virtual std::expected<Track, Error> getTrack(int64_t id) = 0;
    virtual std::expected<int64_t, Error> insertTrack(const Track& t) = 0;
    virtual std::expected<Track, Error> findByFingerprint(
        const std::array<uint8_t, 32>& fp) = 0;
    virtual std::expected<Track, Error> findByPath(
        std::string_view path) = 0;
    virtual std::expected<Track, Error> findBySource(
        const SourceRef& source) = 0;
    virtual std::expected<void, Error> updateTrack(const Track& t) = 0;
    virtual std::expected<void, Error> deleteTrack(int64_t id) = 0;

    // Queue
    virtual std::expected<Queue, Error> getQueue(int64_t qid) = 0;
    virtual std::expected<bool, Error> queueExists(int64_t qid) = 0;
    virtual std::expected<std::vector<Queue>, Error> listQueues() = 0;
    virtual std::expected<void, Error> queueEnqueueBatch(
        int64_t qid, std::span<const int64_t> tids) = 0;
    virtual std::expected<void, Error> queueRemove(int64_t qid, int64_t pos) = 0;
    // ... more queue operations

    // Engine state
    virtual std::expected<EngineState, Error> loadEngineState() = 0;
    virtual std::expected<void, Error> saveEngineState(
        const EngineState& state) = 0;
    virtual std::expected<void, Error> saveEngineShuffle(
        bool enabled, std::span<const int64_t> permutation,
        int64_t cursor_position) = 0;

    // Libraries
    virtual std::expected<std::vector<Library>, Error> listLibraries() = 0;
    virtual std::expected<void, Error> libraryUpdate(
        const Library& l) = 0;
    virtual std::expected<void, Error> libraryDelete(int64_t id) = 0;

    // Search
    virtual std::expected<std::vector<Track>, Error> listTracks(
        const TrackQuery* q = nullptr) = 0;

    // Playlists, bookmarks, history, statistics...
};
```

## Thread safety

Implementations must document their concurrency guarantees. The engine may
call this interface from monitor/decode and service paths, so implementations
used with `Engine` must serialize their own database access.

## Error behavior

Use `StatusCode::NotFound` for absent entities, `InvalidArg` for invalid caller
input, `State` for invalid lifecycle state, and `Io`, `Corrupt`, or `Internal`
for backend failures.

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/contracts/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/contracts)
