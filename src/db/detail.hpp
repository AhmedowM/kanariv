#pragma once
/**
 * @file detail.hpp
 * @brief Internal re-export hub for db helpers.
 * @ingroup kanariv_db
 * @details Re-exports fts and stmt_helpers into the `kanariv::db::internal`
 * namespace, plus the media fingerprint helper into
 * `kanariv::media::internal`. Private to src/ (NOT installed);
 * included by db .cpps and src-private db headers.
 */

#include <db/fts.hpp>
#include <db/stmt_helpers.hpp>
#include <media/fingerprint.hpp>
