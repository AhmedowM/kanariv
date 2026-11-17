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
#include <kanariv/types/format.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/status_format.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::ipc {

std::string serializeRequest(const IpcRequest& req) {
    Json j;
    j["id"] = req.id;
    j["cmd"] = toJson(req.cmd);
    return j.dump();
}

std::expected<IpcRequest, kanariv::utils::Error> deserializeRequest(std::string_view sv) {
    try {
        const Json j = Json::parse(sv);
        if (!j.contains("id") || !j.contains("cmd")) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "missing id/cmd")};
        }
        uint32_t id = j["id"].get<uint32_t>();
        auto cmd = commandFromJson(j["cmd"]);
        if (!cmd)
            return std::unexpected{cmd.error()};
        return IpcRequest{id, std::move(*cmd)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::string serializeReply(const IpcReply& rep) {
    Json j;
    j["id"] = rep.id;
    if (rep.result.has_value()) {
        j["ok"] = true;
        j["result"] = toJson(*rep.result);
    } else {
        j["ok"] = false;
        j["error"] = detail::errorToJson(rep.result.error());
    }
    return j.dump();
}

std::expected<IpcReply, kanariv::utils::Error> deserializeReply(std::string_view sv) {
    try {
        const Json j = Json::parse(sv);
        if (!j.contains("id")) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "missing id")};
        }
        uint32_t id = j["id"].get<uint32_t>();
        // Fail closed: a reply without an explicit boolean `ok` is corrupt, not
        // a success. The old fallback treated a missing `ok` as success.
        if (!j.contains("ok") || !j["ok"].isBoolean()) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                                             "missing boolean ok")};
        }
        const bool ok = j["ok"].get<bool>();

        if (ok) {
            if (!j.contains("result")) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg, "missing result")};
            }
            auto r = resultFromJson(j["result"]);
            if (!r)
                return std::unexpected{r.error()};
            return IpcReply{id, std::move(*r)};
        } else {
            // `ok == false` requires an explicit error payload (the sole
            // producer always emits one); never reinterpret `result`.
            if (!j.contains("error")) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::Corrupt, "missing error")};
            }
            Json ej = j["error"];
            auto e = detail::errorFromJson(ej);
            if (!e)
                return std::unexpected{e.error()};
            return IpcReply{id, std::unexpected{*e}};
        }
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::array<std::byte, frameHeaderBytes> encodeFrameLength(std::uint32_t length) noexcept {
    return {static_cast<std::byte>((length >> 24) & 0xFF),
            static_cast<std::byte>((length >> 16) & 0xFF),
            static_cast<std::byte>((length >> 8) & 0xFF), static_cast<std::byte>(length & 0xFF)};
}

std::expected<std::uint32_t, kanariv::utils::Error>
decodeFrameLength(std::span<const std::byte, frameHeaderBytes> header) {
    const std::uint32_t len =
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(header[0])) << 24) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(header[1])) << 16) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(header[2])) << 8) |
        static_cast<std::uint32_t>(std::to_integer<unsigned char>(header[3]));
    // Cap before any caller allocates: never honour a peer-claimed
    // multi-gigabyte length.
    if (len > maxFrameBytes) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame too large")};
    }
    return len;
}

std::expected<std::vector<std::byte>, kanariv::utils::Error> frame(std::string_view json) {
    // Reject before narrowing size_t -> uint32_t: an oversized payload would
    // otherwise be framed with a bogus length and desync the stream.
    if (json.size() > maxFrameBytes) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame too large")};
    }
    std::vector<std::byte> out;
    out.reserve(frameHeaderBytes + json.size());
    const auto header = encodeFrameLength(static_cast<std::uint32_t>(json.size()));
    out.insert(out.end(), header.begin(), header.end());
    // Bulk insert (M-2): the old per-byte push_back loop defeated the
    // reserve() above with per-byte size checks on some STLs.
    const auto* bytes = reinterpret_cast<const std::byte*>(json.data());
    out.insert(out.end(), bytes, bytes + json.size());
    return out;
}

std::expected<std::string, kanariv::utils::Error> deframe(std::span<const std::byte> buf) {
    if (buf.size() < frameHeaderBytes) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame too short")};
    }
    const auto declared = decodeFrameLength(buf.first<frameHeaderBytes>());
    if (!declared) {
        return std::unexpected{declared.error()};
    }
    const std::size_t len = *declared;
    const std::size_t total = frameHeaderBytes + len;
    if (buf.size() < total) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame incomplete")};
    }
    if (buf.size() != total) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "trailing bytes after frame")};
    }
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(std::to_integer<unsigned char>(buf[frameHeaderBytes + i])));
    }
    return s;
}

} // namespace kanariv::ipc
