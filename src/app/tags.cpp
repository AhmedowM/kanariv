/**
 * @file tags.cpp
 * @brief Track tag command handlers.
 * @ingroup kanariv_app
 * @details Phase 1: returns outcome data; the frontend renders.
 */

#include <algorithm>
#include <app/app_impl.hpp>
#include <kanariv/app/app.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <variant>

namespace kanariv::app {

bool isTagField(std::string_view field) {
    return std::find(tagFields.begin(), tagFields.end(), field) != tagFields.end();
}

// Returns true if s consists entirely of decimal digits (a numeric track id).
bool isNumericId(std::string_view s) {
    if (s.empty())
        return false;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

AppResult App::tagEdit(std::string idOrPath, const std::string& field,
                       const std::string& value) {
    int64_t id = 0;
    if (isNumericId(idOrPath)) {
        id = std::stoll(idOrPath);
    } else {
        // Resolve path to track id via TagGet{path}.
        kanariv::ipc::Command tgCmd{kanariv::ipc::TagGet{.path = idOrPath}};
        auto tgRes = impl_->sendRaw(tgCmd);
        if (!tgRes)
            return std::unexpected{tgRes.error()};
        if (auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*tgRes))
            id = st->track.id;
        else
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::NotFound,
                "track not found for path '" + idOrPath + "'")};
    }
    kanariv::ipc::Command cmd{kanariv::ipc::TagEdit{.id=id, .field=field, .value=value}};
    return impl_->confirm(impl_->sendRaw(cmd),
                          std::format("Updated {} for track {}", field, id));
}

AppResult App::tagGet(std::string idOrPath, bool cover) {
    kanariv::ipc::TagGet tg{.cover = cover};
    if (isNumericId(idOrPath)) {
        tg.id = std::stoll(idOrPath);
    } else {
        tg.path = idOrPath;
    }
    kanariv::ipc::Command cmd{std::move(tg)};
    return impl_->confirm(impl_->sendRaw(cmd));
}

std::expected<TagValue, kanariv::utils::Error> App::tagValue(std::string idOrPath,
                                                            const std::string& field) {
    if (!isTagField(field)) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "unknown tag field '" + field + "'")};
    }

    int64_t id = 0;
    if (isNumericId(idOrPath)) {
        id = std::stoll(idOrPath);
    } else {
        kanariv::ipc::Command tgCmd{kanariv::ipc::TagGet{.path = idOrPath}};
        auto tgRes = impl_->sendRaw(tgCmd);
        if (!tgRes)
            return std::unexpected{tgRes.error()};
        if (auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*tgRes))
            id = st->track.id;
        else
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::NotFound,
                "track not found for path '" + idOrPath + "'")};
    }

    // Cover art needs its own IPC round-trip with cover=true; for all other
    // fields a single TagGet suffices.
    if (field == "cover") {
        kanariv::ipc::Command cmd{kanariv::ipc::TagGet{.id = id, .cover = true}};
        auto res = impl_->sendRaw(cmd);
        if (!res)
            return std::unexpected{res.error()};
        auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*res);
        if (!st || st->cover_data.empty())
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::NotFound,
                "tag get cover: no cover art for track " + std::to_string(id))};
        kanariv::utils::Json j = kanariv::utils::Json::object();
        j["cover"] = st->cover_data;
        return TagValue{.value=st->cover_data, .json=j.dump()};
    }

    kanariv::ipc::Command cmd{kanariv::ipc::TagGet{.id = id}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*res);
    if (!st)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        "tag get: no track in result")};
    const auto& t = st->track;
    bool numeric = false;
    std::string value;
    if (field == "title")
        value = t.title;
    else if (field == "artist")
        value = t.artist;
    else if (field == "album")
        value = t.album;
    else if (field == "album_artist")
        value = t.album_artist;
    else if (field == "genre")
        value = t.genre;
    else if (field == "year") {
        numeric = true;
        value = std::to_string(t.year);
    } else if (field == "track_number") {
        numeric = true;
        value = std::to_string(t.track_num);
    } else if (field == "disc_number") {
        numeric = true;
        value = std::to_string(t.disc_num);
    }
    kanariv::utils::Json j = kanariv::utils::Json::object();
    if (numeric) {
        try {
            j[field] = std::stoll(value);
        } catch (...) {
            j[field] = value;
        }
    } else {
        j[field] = value;
    }
    return TagValue{.value=value, .json=j.dump()};
}

} // namespace kanariv::app
