#include "raw.hpp"

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/result.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "config_detail.hpp"

namespace kanariv::config::detail {

using kanariv::utils::Json;

kanariv::utils::Expected<std::string> configGetRaw(const std::filesystem::path& p,
                                                  std::string_view key) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "config not found")};
    }
    auto fileRes = readFileString(p);
    if (!fileRes)
        return std::unexpected{fileRes.error()};
    std::string content = std::move(*fileRes);
    if (content.empty()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                        "key not found: " + std::string(key))};
    }
    try {
        const Json j = Json::parse(content);
        if (!j.isObject()) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                                            "config is not an object")};
        }
        std::string k(key);
        if (!j.contains(k)) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                            "key not found: " + k)};
        }
        const Json v = j[k];
        if (v.isString())
            return v.get<std::string>();
        if (v.isNull())
            return std::string{"null"};
        return v.dump();
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

kanariv::utils::Expected<void> configSetRaw(const std::filesystem::path& p, std::string_view key,
                                           std::string_view value) {
    Json j = Json::object();
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        auto fileRes = readFileString(p);
        if (fileRes) {
            std::string content = std::move(*fileRes);
            if (!content.empty()) {
                try {
                    auto parsed = Json::parse(content);
                    if (parsed.isObject())
                        j = std::move(parsed);
                    else
                        j = Json::object();
                } catch (...) {
                    j = Json::object();
                }
            }
        }
    }
    std::string k(key);
    Json v;
    bool parsedAsJson = false;
    if (!value.empty()) {
        try {
            auto tmp = Json::parse(value);
            v = std::move(tmp);
            parsedAsJson = true;
        } catch (...) {
            parsedAsJson = false;
        }
    } else {
        v = std::string{};
        parsedAsJson = true;
    }
    if (!parsedAsJson)
        v = std::string(value);
    j[k] = std::move(v);
    try {
        auto parent = p.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        std::ofstream out(p);
        if (!out) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot write config")};
        }
        out << j.dump(2);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

kanariv::utils::Expected<std::vector<RawConfigValue>> configListRaw(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return std::vector<RawConfigValue>{};
    auto fileRes = readFileString(p);
    if (!fileRes)
        return std::unexpected{fileRes.error()};
    std::string content = std::move(*fileRes);
    if (content.empty())
        return std::vector<RawConfigValue>{};
    try {
        const Json j = Json::parse(content);
        if (!j.isObject())
            return std::vector<RawConfigValue>{};
        std::vector<RawConfigValue> out;
        out.reserve(j.size());
        for (std::size_t i = 0, n = j.size(); i < n; ++i) {
            auto kkExp = j.keyAt(i);
            if (!kkExp)
                continue;
            const std::string kk = std::move(*kkExp);
            if (kk.empty() || kk == "type")
                continue;
            // NOTE: at(size_t) only serves arrays (resolveConstImpl rejects
            // non-arrays), so look the value up by key for objects.
            auto vvExp = j.at(kk);
            if (!vvExp)
                continue;
            const Json vv = std::move(*vvExp);
            std::string vs;
            if (vv.isString())
                vs = vv.get<std::string>();
            else if (vv.isNull())
                vs = "null";
            else
                vs = vv.dump();
            out.push_back(RawConfigValue{.key=kk, .value=vs});
        }
        return out;
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

kanariv::utils::Expected<void> configDeleteRaw(const std::filesystem::path& p,
                                              std::string_view key) {
    if (key.empty()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "empty key")};
    }
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "config not found")};
    }
    auto fileRes = readFileString(p);
    if (!fileRes)
        return std::unexpected{fileRes.error()};
    std::string content = std::move(*fileRes);
    if (content.empty()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                        "key not found: " + std::string(key))};
    }
    try {
        Json j = Json::parse(content);
        if (!j.isObject()) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                                            "config is not an object")};
        }
        std::string k(key);
        if (!j.contains(k)) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                            "key not found: " + k)};
        }
        j.erase(k);
        auto parent = p.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        std::ofstream out(p);
        if (!out) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot write config")};
        }
        out << j.dump(2);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

kanariv::utils::Expected<void> configResetAllRaw(const std::filesystem::path& p) {
    try {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            std::filesystem::remove(p, ec);
            if (ec) {
                return std::unexpected{
                    kanariv::utils::makeError(kanariv::utils::StatusCode::Io, ec.message())};
            }
        }
        return {};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

} // namespace kanariv::config::detail
