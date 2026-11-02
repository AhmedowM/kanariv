module;
#include <kanariv/db/search.hpp>

export module kanariv.db:search;

export namespace kanariv::db {
using ::kanariv::db::sanitizeFtsTerm;
using ::kanariv::db::search;
using ::kanariv::db::searchFts;
using ::kanariv::db::searchLike;
} // namespace kanariv::db
