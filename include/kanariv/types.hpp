#pragma once

/**
 * @file types.hpp
 * @brief Main header for kanariv.types -- dependency-free shared value types.
 * @ingroup kanariv_types
 * @details Bottom layer of the component graph. Provides the wire enums
 * (RepeatMode, PlaybackState) and the plain-data records (Track, Playlist,
 * QueueItem, HistoryEntry, Bookmark) shared by engine, ipc, db and app, so no
 * component has to include another just to name them. The wire text of the
 * enums (`types/format.hpp`) and its inverse (`types/parse.hpp`) live here
 * too. No dependencies.
 */

#include <kanariv/types/format.hpp>
#include <kanariv/types/parse.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/wire.hpp>
