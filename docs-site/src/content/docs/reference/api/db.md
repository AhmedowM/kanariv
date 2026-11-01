---
title: Database — caudio::db
description: SQLite storage and FTS5 search (Database, scanLibrary)
---

# Database — `caudio::db`

The `caudio::db` component provides SQLite storage and FTS5 full-text search
for the music library.

## Database

```cpp
#include <caudio/db/database.hpp>

class Database {
public:
    static Expected<Database> open(std::string_view path);
    // Track operations, queue operations, engine state,
    // libraries, playlists, bookmarks, history, statistics
};
```

## Scan

```cpp
#include <caudio/db/scan.hpp>

CAUDIO_API std::expected<void, Error>
scanLibrary(contracts::IDatabase& db, int64_t libraryId,
            std::function<void(int64_t, int64_t, std::string_view)> progress = {},
            caudio::media::ScanMode mode = caudio::media::ScanMode::Sampled);
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/db/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/db)
