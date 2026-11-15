/**
 * @file commands_config.cpp
 * @brief `config`, `device` and `info` commands.
 */

#include <iostream>
#include <kanariv/utils/print.hpp>
#include <optional>
#include <string>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"

namespace kanariv::app::cli::detail {

void declareConfig(CLI::App& cli, Options& o) {
    auto& c = o.config;
    c.config = cli.add_subcommand("config", "Config operations");
    c.get = c.config->add_subcommand("get", "Get config value");
    c.get->add_option("key", c.getKey, "Key")->required();
    addJsonFlag(c.get, c.getJson);
    c.set = c.config->add_subcommand("set", "Set config value");
    c.set->add_option("key", c.setKey, "Key")->required();
    c.set->add_option("value", c.setValue, "Value")->required();
    c.list = c.config->add_subcommand("list", "List config");
    addJsonFlag(c.list, c.listJson);
    c.exportFile = c.config->add_subcommand("export", "Export config");
    c.exportFile->add_option("path", c.exportPath, "Path")->required();
    addJsonFlag(c.exportFile, c.exportJson);
    c.importFile = c.config->add_subcommand("import", "Import config");
    c.importFile->add_option("path", c.importPath, "Path")->required();
    addJsonFlag(c.importFile, c.importJson);
    c.reset = c.config->add_subcommand("reset", "Reset config to defaults");
    c.reset->add_option("key", c.resetKey, "Key to reset (omit to reset all)");
    addJsonFlag(c.reset, c.resetJson);
    c.device = cli.add_subcommand("device", "Audio device operations");
    c.devList = c.device->add_subcommand("list", "List audio output devices");
    addJsonFlag(c.devList, c.devListJson);
    c.devSet = c.device->add_subcommand("set", "Set default audio device");
    c.devSet->add_option("id", c.devSetId, "Device ID")->required();
    addJsonFlag(c.devSet, c.devSetJson);
    c.devTest = c.device->add_subcommand("test", "Check audio device is available");
    c.devTest->add_option("--id", c.devTestId, "Device ID (default: current config)");
    c.devTest->add_flag("--tone", c.devTestTone, "Play an audible test tone");
    addJsonFlag(c.devTest, c.devTestJson);
    c.info = cli.add_subcommand("info", "Show current track info");
    addJsonFlag(c.info, c.infoJson);
}

std::optional<int> runConfig(App& app, const Options& o) {
    const auto& c = o.config;
    if (c.config->parsed()) {
        if (c.get->parsed())
            return render(app.configGet(c.getKey), c.getJson);
        if (c.set->parsed())
            return render(app.configSet(c.setKey, c.setValue), false);
        if (c.list->parsed())
            return render(app.configList(), c.listJson);
        if (c.exportFile->parsed())
            return render(app.configExport(c.exportPath), c.exportJson);
        if (c.importFile->parsed())
            return render(app.configImport(c.importPath), c.importJson);
        if (c.reset->parsed()) {
            std::optional<std::string> key;
            if (c.reset->count("key") > 0 && !c.resetKey.empty())
                key = c.resetKey;
            else if (c.reset->count("key") > 0 && c.resetKey.empty()) {
                // explicit empty string passed -> treat as error
                kanariv::println(std::cerr, "config reset: empty key (omit --key to reset all)");
                return exitUsage;
            }
            return render(app.configReset(key), c.resetJson);
        }
        std::cout << c.config->help() << "\n";
        return exitOk;
    }
    if (c.device->parsed()) {
        if (c.devList->parsed())
            return render(app.deviceList(), c.devListJson);
        if (c.devSet->parsed())
            return render(app.deviceSet(c.devSetId), c.devSetJson);
        if (c.devTest->parsed()) {
            std::optional<std::string> id;
            if (!c.devTestId.empty())
                id = c.devTestId;
            return render(app.deviceTest(id, c.devTestTone), c.devTestJson);
        }
        std::cout << c.device->help() << "\n";
        return exitOk;
    }
    if (c.info->parsed())
        return render(app.info(), c.infoJson);
    return std::nullopt;
}

} // namespace kanariv::app::cli::detail
