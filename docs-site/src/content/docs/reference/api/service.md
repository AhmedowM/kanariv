---
title: Service — caudio::service
description: Daemon service and in-process service host
---

# Service — `caudio::service`

The `caudio::service` component provides the daemon `Service` class and an
in-process `Host` wrapper.

## Service

```cpp
#include <caudio/service/service.hpp>

struct ServiceConfig : public caudio::config::Config {
    bool logToStderr{false};
    bool inProcess{false};
    caudio::config::RuntimePaths runtime{};
};

class Service {
public:
    using ExpectedService = std::expected<std::unique_ptr<Service>, Error>;

    static ExpectedService create(
        const ServiceConfig& cfg,
        std::shared_ptr<caudio::contracts::IDatabase> db = nullptr);
    ~Service();

    Expected<void> run(std::stop_token st);
    [[nodiscard]] bool running() const noexcept;
    Expected<void> attachDatabase(
        std::shared_ptr<caudio::contracts::IDatabase> db);
    void shutdown();
};
```

## Host (in-process)

```cpp
#include <caudio/service/host.hpp>

caudio::service::ServiceConfig cfg;
cfg.runtime.database = "/data/app/caudio/caudio.db";
cfg.runtime.logs = "/data/app/caudio/logs";
cfg.inProcess = true;
auto host = caudio::service::Host::create(cfg);
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/service/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/service)
