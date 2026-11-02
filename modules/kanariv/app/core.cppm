module;
#include <kanariv/app/app.hpp>

export module kanariv.app:core;

export namespace kanariv::app {
using ::kanariv::app::App;
using ::kanariv::app::AppResult;
using ::kanariv::app::BatchReport;
using ::kanariv::app::BatchResult;
using ::kanariv::app::isTagField;
using ::kanariv::app::Outcome;
using ::kanariv::app::tagFields;
using ::kanariv::app::TagValue;
using ::kanariv::app::TrackList;
} // namespace kanariv::app
