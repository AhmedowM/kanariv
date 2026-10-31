/**
 * @file tags.cpp
 * @brief Track tag command handlers.
 * @ingroup caudio_app
 * @details Phase 1: returns outcome data; the frontend renders.
 */

#include <algorithm>
#include <app/app_impl.hpp>
#include <caudio/app/app.hpp>
#include <caudio/ipc/command.hpp>
#include <caudio/ipc/result.hpp>
#include <caudio/utils/error.hpp>
#include <caudio/utils/json.hpp>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <variant>

namespace caudio::app {

bool isTagField(std::string_view field) {
    return std::find(tagFields.begin(), tagFields.end(), field) != tagFields.end();
}

AppResult App::tagEdit(std::int64_t id, const std::string& field, const std::string& value) {
    caudio::ipc::Command cmd{caudio::ipc::TagEdit{id, field, value}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Updated {} for track {}", field, id));
}

AppResult App::tagGet(std::int64_t id, bool cover) {
    caudio::ipc::Command cmd{caudio::ipc::TagGet{id, cover}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

std::expected<TagValue, caudio::utils::Error> App::tagValue(std::int64_t id,
                                                            const std::string& field) {
    if (!isTagField(field)) {
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::InvalidArg,
                                                        "unknown tag field '" + field + "'")};
    }

    // Cover art needs its own IPC round-trip with cover=true; for all other
    // fields a single TagGet suffices.
    if (field == "cover") {
        caudio::ipc::Command cmd{caudio::ipc::TagGet{id, true}};
        auto res = impl_->sendRaw(cmd);
        if (!res)
            return std::unexpected{res.error()};
        auto* st = std::get_if<caudio::ipc::SingleTrack>(&*res);
        if (!st || st->cover_data.empty())
            return std::unexpected{caudio::utils::makeError(
                caudio::utils::StatusCode::NotFound,
                "tag get cover: no cover art for track " + std::to_string(id))};
        caudio::utils::Json j = caudio::utils::Json::object();
        j["cover"] = st->cover_data;
        return TagValue{st->cover_data, j.dump()};
    }

    caudio::ipc::Command cmd{caudio::ipc::TagGet{id}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    auto* st = std::get_if<caudio::ipc::SingleTrack>(&*res);
    if (!st)
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::Internal,
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
    caudio::utils::Json j = caudio::utils::Json::object();
    if (numeric) {
        try {
            j[field] = std::stoll(value);
        } catch (...) {
            j[field] = value;
        }
    } else {
        j[field] = value;
    }
    return TagValue{value, j.dump()};
}

} // namespace caudio::app
