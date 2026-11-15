/**
 * @file commands_tags.cpp
 * @brief Tag reading/editing and track rating commands.
 */

#include <cstdint>
#include <fstream>
#include <iostream>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/parse.hpp>
#include <kanariv/utils/print.hpp>
#include <limits>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"

namespace kanariv::app::cli::detail {

void declareTags(CLI::App& cli, Options& o) {
    auto& t = o.tag;
    t.tag = cli.add_subcommand("tag", "Tag operations");
    t.edit = t.tag->add_subcommand("edit", "Edit track tag");
    t.edit->add_option("id", t.editId, "Track id or file path")->required();
    t.edit
        ->add_option("field", t.editField,
                     "Field (title,artist,album,album_artist,genre,year,track_number,disc_number)")
        ->required()
        ->check(CLI::IsMember({"title", "artist", "album", "album_artist", "genre", "year",
                               "track_number", "disc_number"}));
    t.edit->add_option("value", t.editValue, "New value")->required();
    addJsonFlag(t.edit, t.editJson);
    t.get = t.tag->add_subcommand("get", "Get track tags");
    t.get->add_option("id", t.getId, "Track id or file path")->required();
    t.get->add_option("field", t.getField, "Single field (default: all)");
    addJsonFlag(t.get, t.getJson);
    t.get->add_option("--out", t.getOut, "Output file path (cover art only)");
    t.batch = t.tag->add_subcommand("batch", "Batch-edit tags from a file");
    t.batch
        ->add_option("--file", t.batchFile, "Tag assignments file (JSON or id|field|value lines)")
        ->required();
    t.batch->add_flag("--dry-run", t.batchDryRun, "Validate only, do not write");
    addJsonFlag(t.batch, t.batchJson);
    t.track = cli.add_subcommand("track", "Track operations");
    t.rate = t.track->add_subcommand("rate", "Rate a track (0-5)");
    t.rate->add_option("id", t.rateId, "Track id or file path")->required();
    t.rate->add_option("rating", t.rateRating, "Rating (0-5)")->check(CLI::Range(0, 5));
    addJsonFlag(t.rate, t.rateJson);
}

std::optional<int> runTags(App& app, const Options& o) {
    const auto& t = o.tag;
    if (t.tag->parsed()) {
        if (t.edit->parsed())
            return render(app.tagEdit(t.editId, t.editField, t.editValue), t.editJson);
        if (t.get->parsed()) {
            if (!t.getField.empty() && !kanariv::app::isTagField(t.getField)) {
                kanariv::println(std::cerr,
                                 "tag get: unknown field '{}' (expected one of "
                                 "title|artist|album|album_artist|genre|year|"
                                 "track_number|disc_number|cover)",
                                 t.getField);
                return exitUsage;
            }
            if (t.getField.empty())
                return render(app.tagGet(t.getId), t.getJson);
            auto tv = app.tagValue(t.getId, t.getField);
            if (!tv)
                return renderError(tv.error());
            if (!t.getOut.empty() && t.getField == "cover") {
                // --out only applies to cover art: decode base64 and write to file.
                auto decoded = kanariv::utils::Base64::decode(tv->value);
                if (!decoded)
                    return renderError(decoded.error());
                std::ofstream ofs(t.getOut, std::ios::binary);
                if (!ofs)
                    return renderError(kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                                 "tag get: cannot open output '" +
                                                                     t.getOut + "'"));
                if (decoded->size() >
                    static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
                    return renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                  "tag get: cover art is too large to write"));
                ofs.write(reinterpret_cast<const char*>(decoded->data()),
                          static_cast<std::streamsize>(decoded->size()));
                ofs.flush();
                if (!ofs)
                    return renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                  "tag get: write failed for '" + t.getOut + "'"));
                kanariv::println(std::cout, "Wrote cover art ({} bytes) to {}", decoded->size(),
                                 t.getOut);
                return 0;
            }
            if (t.getJson)
                kanariv::println("{}", tv->json);
            else
                kanariv::println("{}", tv->value);
            return 0;
        }
        if (t.batch->parsed()) {
            std::ifstream ifs(t.batchFile, std::ios::binary);
            if (!ifs) {
                kanariv::println(std::cerr, "tag batch: cannot open file '{}'", t.batchFile);
                return 1;
            }
            std::stringstream ss;
            ss << ifs.rdbuf();
            std::string content = ss.str();

            std::vector<kanariv::ipc::TagBatchEditEntry> entries;
            // Detect JSON by leading '[' or '{'; otherwise parse line-oriented
            // "id|field|value" or "path|field|value" records.
            std::size_t first = content.find_first_not_of(" \t\r\n");
            bool isJson =
                first != std::string::npos && (content[first] == '[' || content[first] == '{');
            if (isJson) {
                auto j = kanariv::utils::Json::tryParse(content);
                if (!j)
                    return renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                  "tag batch: invalid JSON: " + j.error().message));
                const kanariv::utils::Json& arr = *j;
                if (!arr.isArray())
                    return renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                  "tag batch: JSON must be an array of entries"));
                for (std::size_t i = 0, n = arr.size(); i < n; ++i) {
                    auto jeExp = arr.at(i);
                    if (!jeExp)
                        return renderError(jeExp.error());
                    const kanariv::utils::Json& je = *jeExp;
                    kanariv::ipc::TagBatchEditEntry e{};
                    if (je.contains("id") && je["id"].isNumber())
                        e.id = je["id"].get<int64_t>();
                    if (je.contains("path") && je["path"].isString())
                        e.path = je["path"].get<std::string>();
                    if (je.contains("field") && je["field"].isString())
                        e.field = je["field"].get<std::string>();
                    if (je.contains("value") && je["value"].isString())
                        e.value = je["value"].get<std::string>();
                    entries.push_back(std::move(e));
                }
            } else {
                std::istringstream lines(content);
                std::string line;
                while (std::getline(lines, line)) {
                    // Strip trailing \r
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty() || line[0] == '#')
                        continue;
                    std::vector<std::string> parts;
                    std::size_t start = 0;
                    std::size_t pos = line.find('|');
                    while (pos != std::string::npos) {
                        parts.push_back(line.substr(start, pos - start));
                        start = pos + 1;
                        pos = line.find('|', start);
                    }
                    parts.push_back(line.substr(start));
                    if (parts.size() != 3) {
                        kanariv::println(std::cerr,
                                         "tag batch: malformed line (expected id|field|value): {}",
                                         line);
                        return exitUsage;
                    }
                    kanariv::ipc::TagBatchEditEntry e{};
                    // First part: numeric id or path.
                    if (const auto id = kanariv::utils::parseId(parts[0]))
                        e.id = *id;
                    else
                        e.path = parts[0];
                    e.field = parts[1];
                    e.value = parts[2];
                    entries.push_back(std::move(e));
                }
            }

            if (entries.empty()) {
                kanariv::println(std::cerr, "tag batch: no entries found in '{}'", t.batchFile);
                return 1;
            }

            auto res = app.tagBatchEdit(std::move(entries), t.batchDryRun);
            if (!res)
                return renderError(res.error());
            if (auto* outcome = &*res) {
                if (!outcome->result) {
                    kanariv::println(std::cerr, "tag batch: no result from daemon");
                    return 1;
                }
                auto& ipc_res = *outcome->result;
                if (auto* br = std::get_if<kanariv::ipc::TagBatchEditResult>(&ipc_res)) {
                    if (t.batchJson) {
                        kanariv::println("{}", kanariv::ipc::toJson(ipc_res).dump());
                        return 0;
                    }
                    kanariv::println(std::cout, "Tag batch: {} succeeded, {} failed", br->succeeded,
                                     br->failed);
                    for (const auto& er : br->results) {
                        std::string idStr = er.id.has_value()
                                                ? std::to_string(*er.id)
                                                : (er.path.has_value() ? *er.path : "?");
                        kanariv::println(std::cout, "  [{}] {}={} {}", idStr, er.field,
                                         er.ok ? "OK" : "FAIL", er.ok ? "" : er.error);
                    }
                    return br->failed > 0 ? 1 : 0;
                }
                kanariv::println(std::cerr, "tag batch: unexpected result type");
                return 1;
            }
            return render(std::move(res), t.batchJson);
        }
        std::cout << t.tag->help() << "\n";
        return 0;
    }
    if (t.track->parsed()) {
        if (t.rate->parsed())
            return render(app.trackRate(t.rateId, t.rateRating), t.rateJson);
        std::cout << t.track->help() << "\n";
        return 0;
    }
    return std::nullopt;
}

} // namespace kanariv::app::cli::detail
