/**
 * @file test_cli_commands.cpp
 * @brief Tripwire for the CLI declaration vocabulary.
 * @details The dispatch chain is hand-written (roadmap L6, option 1), so this
 * test fails when a declared leaf subcommand or a dispatch handler drifts.
 * `declareAll` is called exactly as `Shell::run` calls it; the leaf paths it
 * registers must equal the documented vocabulary below.
 */

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

#include "commands.hpp"

namespace {

void collectLeaves(const CLI::App& app, const std::string& prefix, std::set<std::string>& out) {
    // CLI11's unfiltered get_subcommands() returns only *parsed* subcommands;
    // the definition list is reached through the filter overload.
    for (const auto* sub : app.get_subcommands([](const CLI::App*) { return true; })) {
        const std::string path = prefix.empty() ? sub->get_name() : prefix + " " + sub->get_name();
        if (sub->get_subcommands([](const CLI::App*) { return true; }).empty())
            out.insert(path);
        else
            collectLeaves(*sub, path, out);
    }
}

} // namespace

TEST_CASE("declareAll matches the documented leaf vocabulary", "[cli][commands]") {
    CLI::App cli{"kanariv"};
    kanariv::app::cli::detail::Options options;
    kanariv::app::cli::detail::declareAll(cli, options);

    std::set<std::string> declared;
    collectLeaves(cli, "", declared);

    const std::set<std::string> expected{
        "start",
        "shutdown",
        "play",
        "pause",
        "resume",
        "restart",
        "stop",
        "next",
        "prev",
        "seek",
        "status",
        "volume",
        "queue list",
        "queue queues",
        "queue tracks",
        "queue switch",
        "queue add",
        "queue remove",
        "queue create",
        "queue delete",
        "queue move",
        "queue clear",
        "queue shuffle",
        "queue repeat",
        "playlist list",
        "playlist tracks",
        "playlist create",
        "playlist add",
        "playlist load",
        "playlist save",
        "playlist delete",
        "playlist rename",
        "playlist export",
        "playlist import",
        "library scan",
        "library analyze",
        "library search",
        "library stats",
        "library add",
        "library remove",
        "library list",
        "library favorites",
        "library maintenance",
        "tag edit",
        "tag get",
        "tag batch",
        "track rate",
        "history list",
        "history clear",
        "bookmark add",
        "bookmark list",
        "bookmark remove",
        "bookmark goto",
        "lyrics set",
        "lyrics get",
        "lyrics remove",
        "preview",
        "config get",
        "config set",
        "config list",
        "config export",
        "config import",
        "config reset",
        "device list",
        "device set",
        "device test",
        "info",
        "version",
    };

    for (const auto& path : expected) {
        INFO("missing declaration: " << path);
        CHECK(declared.count(path) == 1);
    }
    for (const auto& path : declared) {
        INFO("undeclared by the tripwire: " << path);
        CHECK(expected.count(path) == 1);
    }
    CHECK(declared.size() == expected.size());
}
