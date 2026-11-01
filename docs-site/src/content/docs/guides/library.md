---
title: Library Management
description: Scan, search, rate, and manage your music library
---

# Library Management

caudio's library is stored in SQLite with FTS5 full-text search. Tracks are
discovered by scanning filesystem paths and can be rated, bookmarked, and
organized into queues and playlists.

## Scanning

Scan a directory to add tracks to the library:

```sh
caudio library scan --path ~/Music
caudio library scan --path ~/Music --mode full
```

Scan modes:

| Mode | Description |
|---|---|
| `sampled` | Fast; samples audio for fingerprinting (default) |
| `full` | Complete decode for accurate metadata |

## Searching

Search tracks using FTS5:

```sh
caudio library search "miles davis" --limit 10
caudio library search "kind of blue" --limit 20 --offset 0
```

## Listing

List all tracks in the library:

```sh
caudio library list --limit 50
```

## Ratings

Rate tracks on a scale:

```sh
caudio library rate --track-id 42 --rating 5
caudio library rate --track-id 42 --rating 0   # remove rating
```

## Playlists

Create and manage playlists:

```sh
# Create a playlist
caudio playlist create "My Favorites"

# Add a track
caudio playlist add --playlist-id 1 --track-id 42

# List playlist tracks
caudio playlist list --playlist-id 1
```

## Smart playlists

Smart playlists are defined by a query and dynamically updated:

```sh
caudio playlist create "High Rated" --type smart --smart-query "rating >= 4"
```

## Bookmarks

Add bookmarks at positions within tracks:

```sh
caudio bookmark add --track-id 42 --position 120000 --note "Great solo"
caudio bookmark list --track-id 42
```

## Statistics

View library and playback statistics:

```sh
caudio library stats
caudio library stats --top-tracks 10 --top-artists 5
```

## Next steps

- [Command-Line Client](/guides/cli) — full CLI reference
- [API Reference](/reference/api/) — database and engine API
