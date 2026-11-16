#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <ipc/protocol_internal.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/types/parse.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/result.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::ipc::detail {

std::string resultCodeToString(kanariv::utils::StatusCode r) {
    return std::string(kanariv::utils::toString(r));
}

std::expected<kanariv::utils::StatusCode, kanariv::utils::Error>
resultCodeFromString(std::string_view sv) {
    using R = kanariv::utils::StatusCode;
    if (sv == "Ok")
        return R::Ok;
    if (sv == "InvalidArg")
        return R::InvalidArg;
    if (sv == "NotFound")
        return R::NotFound;
    if (sv == "Unsupported")
        return R::Unsupported;
    if (sv == "Io")
        return R::Io;
    if (sv == "Device")
        return R::Device;
    if (sv == "State")
        return R::State;
    if (sv == "NoMem")
        return R::NoMem;
    if (sv == "Internal")
        return R::Internal;
    if (sv == "AlreadyExists")
        return R::AlreadyExists;
    if (sv == "Busy")
        return R::Busy;
    if (sv == "Corrupt")
        return R::Corrupt;
    if (sv == "NoSpace")
        return R::NoSpace;
    if (sv == "Timeout")
        return R::Timeout;
    return std::unexpected{kanariv::utils::makeError(R::InvalidArg, "unknown Result code")};
}

// TrackRow conversion lives in kanariv::utils (utils/record_json.hpp) -- single
// definition shared by db export/import and the IPC codec.
// (The former ipc::detail subset is deleted; wire readers tolerate the
// fuller db field set: all reads are contains-guarded.)

Json playlistToJson(const kanariv::types::Playlist& p) {
    Json j;
    j["id"] = p.id;
    j["name"] = p.name;
    j["type"] = p.type;
    j["smart_query"] = p.smart_query;
    j["created"] = p.created;
    j["modified"] = p.modified;
    j["library_id"] = p.library_id;
    return j;
}

std::expected<kanariv::types::Playlist, kanariv::utils::Error> playlistFromJson(const Json& j) {
    try {
        kanariv::types::Playlist p{};
        if (j.contains("id") && j["id"].isNumber())
            p.id = j["id"].get<int64_t>();
        if (j.contains("name") && j["name"].isString())
            p.name = j["name"].get<std::string>();
        if (j.contains("type") && j["type"].isNumber())
            p.type = j["type"].get<int32_t>();
        if (j.contains("smart_query") && j["smart_query"].isString())
            p.smart_query = j["smart_query"].get<std::string>();
        if (j.contains("created") && j["created"].isNumber())
            p.created = j["created"].get<int64_t>();
        if (j.contains("modified") && j["modified"].isNumber())
            p.modified = j["modified"].get<int64_t>();
        if (j.contains("library_id") && j["library_id"].isNumber())
            p.library_id = j["library_id"].get<int64_t>();
        return p;
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

Json errorToJson(const kanariv::utils::Error& e) {
    // Only emitted inside `ok:false` replies; the envelope already marks it as
    // an error, so the object carries just the code and message.
    Json j;
    j["code"] = resultCodeToString(e.code);
    j["code_value"] = std::to_underlying(e.code);
    j["message"] = e.message;
    return j;
}

std::expected<kanariv::utils::Error, kanariv::utils::Error> errorFromJson(const Json& j) {
    try {
        std::string codeStr;
        std::string msg;
        if (j.contains("code") && j["code"].isString())
            codeStr = j["code"].get<std::string>();
        else if (j.contains("code_value") && j["code_value"].isNumber()) {
            int v = j["code_value"].get<int>();
            // map via to_underlying comparison
            for (auto c :
                 {kanariv::utils::StatusCode::Ok, kanariv::utils::StatusCode::InvalidArg,
                  kanariv::utils::StatusCode::NotFound, kanariv::utils::StatusCode::Unsupported,
                  kanariv::utils::StatusCode::Io, kanariv::utils::StatusCode::Device,
                  kanariv::utils::StatusCode::State, kanariv::utils::StatusCode::NoMem,
                  kanariv::utils::StatusCode::Internal, kanariv::utils::StatusCode::AlreadyExists,
                  kanariv::utils::StatusCode::Busy, kanariv::utils::StatusCode::Corrupt,
                  kanariv::utils::StatusCode::NoSpace, kanariv::utils::StatusCode::Timeout}) {
                if (std::to_underlying(c) == v) {
                    codeStr = std::string(kanariv::utils::toString(c));
                    break;
                }
            }
        }
        if (j.contains("message") && j["message"].isString())
            msg = j["message"].get<std::string>();
        if (codeStr.empty())
            codeStr = "Internal";
        auto rc = resultCodeFromString(codeStr);
        if (!rc)
            return std::unexpected{rc.error()};
        return kanariv::utils::Error{*rc, msg};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

} // namespace kanariv::ipc::detail
