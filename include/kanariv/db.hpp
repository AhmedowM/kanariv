#pragma once

/**
 * @file db.hpp
 * @brief Aggregate include for the kanariv.db module.
 * @ingroup kanariv_db
 * @defgroup kanariv_db kanariv database
 * @details Includes all public headers: types, core, scan, search, json.
 * Statement/transaction/queue/schema/detail internals and the background
 * writer thread live in src/db/ and are NOT installed.
 */

#include <kanariv/db/database.hpp>
#include <kanariv/db/json.hpp>
#include <kanariv/db/maintenance.hpp>
#include <kanariv/db/scan.hpp>
#include <kanariv/db/search.hpp>
#include <kanariv/types/db.hpp>
