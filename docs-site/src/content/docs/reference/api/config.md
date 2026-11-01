---
title: Config — caudio::config
description: Configuration management, canonical path helpers
---

# Config — `caudio::config`

The `caudio::config` component provides configuration management and canonical
path helpers.

## Config

```cpp
#include <caudio/config.hpp>

struct Config {
    std::filesystem::path dbPath{};
    std::filesystem::path configPath{};
    std::string device{"auto"};
    int logLevel{2};
    std::string socketPath{};
    bool autoResumeOnPlay{false};
    bool clearStopsPlayback{true};
    std::string repeatDefault{"off"};
    bool autostart{false};
};

Expected<Config> loadConfig(const std::filesystem::path& path);
Expected<void> saveConfig(const Config& cfg);
```

## Canonical paths

```cpp
Expected<std::string> socketPathFor(const std::filesystem::path& dbPath);
Expected<std::filesystem::path> pidPathFor(const std::filesystem::path& dbPath);
Expected<std::filesystem::path> lockPathFor(const std::filesystem::path& dbPath);
Expected<std::filesystem::path> logPathFor(const std::filesystem::path& dbPath);
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/config.hpp`
- [Configuration guide](/guides/configuration) — runtime configuration guide
- [Configuration reference](/reference/config) — config file reference
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/config.hpp)
