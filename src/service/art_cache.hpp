#pragma once

/**
 * @file art_cache.hpp
 * @brief Bounded LRU cache of encoded cover art (internal, service-only).
 * @ingroup kanariv_service
 * @details Publishing used to read and Base64-encode the cover on every
 * transport command. The encoded bytes only change when the source file
 * changes, so entries are validated against the source path plus its mtime and
 * size: a retag, rescan or art swap changes the stat and therefore misses the
 * entry, with no invalidation hook in the write paths. Missing art is cached
 * too (a null `base64`), so a track without embedded art is only demuxed once.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace kanariv::service {

/**
 * @brief One cached artwork result for a track.
 * @ingroup kanariv_service
 */
struct ArtCacheEntry final {
    /// Media-session Base64 payload (absent when the track has no art).
    std::optional<std::string> base64;
    /// MIME type of @ref base64 (empty when absent).
    std::string mime;
};

/**
 * @brief Bounded LRU keyed by track id and validated by the source stat.
 * @ingroup kanariv_service
 * @details Not thread-safe: owned by the media-session worker thread.
 */
class ArtCache final {
  public:
    /**
     * @brief Constructs a cache with entry and byte caps.
     * @param max_entries Maximum number of tracks kept (at least 1).
     * @param max_bytes Maximum summed Base64 payload bytes kept (at least 1).
     */
    explicit ArtCache(std::size_t max_entries = 16,
                      std::size_t max_bytes = 32u * 1024u * 1024u) noexcept;

    /**
     * @brief Looks up art for a track/source pair.
     * @param track_id Track the art belongs to.
     * @param source File the art came from (cover file or audio file).
     * @param mtime Current modification time of @p source.
     * @param size Current size of @p source.
     * @return Cached entry on a validated hit (refreshes LRU order), else nullopt.
     */
    std::optional<ArtCacheEntry> get(std::int64_t track_id, const std::filesystem::path& source,
                                     std::filesystem::file_time_type mtime, std::uintmax_t size);

    /**
     * @brief Inserts or replaces an entry, evicting least-recent entries.
     * @param track_id Track the art belongs to.
     * @param source File the art came from.
     * @param mtime Modification time of @p source at encode time.
     * @param size Size of @p source at encode time.
     * @param entry Encoded art (or a cached miss with no `base64`).
     */
    void put(std::int64_t track_id, const std::filesystem::path& source,
             std::filesystem::file_time_type mtime, std::uintmax_t size, ArtCacheEntry entry);

    /// Number of cached tracks.
    std::size_t size() const noexcept {
        return nodes_.size();
    }

    /// Summed Base64 payload bytes currently held.
    std::size_t bytes() const noexcept {
        return bytes_;
    }

    /// Drops every entry.
    void clear() noexcept;

  private:
    struct Node final {
        std::filesystem::path source;
        std::filesystem::file_time_type mtime{};
        std::uintmax_t size{0};
        ArtCacheEntry entry;
        std::list<std::int64_t>::iterator order;
    };

    void evictToFit();

    std::size_t maxEntries_;
    std::size_t maxBytes_;
    std::size_t bytes_{0};
    /// Least-recently used at the front.
    std::list<std::int64_t> order_;
    std::unordered_map<std::int64_t, Node> nodes_;
};

inline ArtCache::ArtCache(std::size_t max_entries, std::size_t max_bytes) noexcept
    : maxEntries_(max_entries == 0 ? 1 : max_entries), maxBytes_(max_bytes == 0 ? 1 : max_bytes) {}

inline std::optional<ArtCacheEntry> ArtCache::get(std::int64_t track_id,
                                                  const std::filesystem::path& source,
                                                  std::filesystem::file_time_type mtime,
                                                  std::uintmax_t size) {
    auto it = nodes_.find(track_id);
    if (it == nodes_.end())
        return std::nullopt;
    if (it->second.source != source || it->second.mtime != mtime || it->second.size != size)
        return std::nullopt;
    order_.splice(order_.end(), order_, it->second.order);
    return it->second.entry;
}

inline void ArtCache::put(std::int64_t track_id, const std::filesystem::path& source,
                          std::filesystem::file_time_type mtime, std::uintmax_t size,
                          ArtCacheEntry entry) {
    if (auto it = nodes_.find(track_id); it != nodes_.end()) {
        bytes_ -= it->second.entry.base64 ? it->second.entry.base64->size() : 0;
        order_.erase(it->second.order);
        nodes_.erase(it);
    }
    order_.push_back(track_id);
    bytes_ += entry.base64 ? entry.base64->size() : 0;
    nodes_.emplace(track_id, Node{source, mtime, size, std::move(entry), std::prev(order_.end())});
    evictToFit();
}

inline void ArtCache::clear() noexcept {
    order_.clear();
    nodes_.clear();
    bytes_ = 0;
}

inline void ArtCache::evictToFit() {
    while (!order_.empty() &&
           (nodes_.size() > maxEntries_ || (bytes_ > maxBytes_ && nodes_.size() > 1))) {
        const std::int64_t victim = order_.front();
        order_.pop_front();
        if (auto it = nodes_.find(victim); it != nodes_.end()) {
            bytes_ -= it->second.entry.base64 ? it->second.entry.base64->size() : 0;
            nodes_.erase(it);
        }
    }
}

} // namespace kanariv::service
