---
title: Client — caudio::client
description: Daemon client and output formatting
---

# Client — `caudio::client`

The `caudio::client` component provides a synchronous client for the caudio
daemon, used by the CLI and third-party frontends.

## Client

```cpp
#include <caudio/client/client.hpp>

struct Config {
    std::filesystem::path dbPath{"library.db"};
    std::string socketPath{};
};

class Client {
public:
    explicit Client(Config cfg);
    explicit Client(std::filesystem::path dbPath);
    explicit Client(std::filesystem::path dbPath, std::string sock);

    // probe the daemon
    bool probe() const;

    // send a command and get a result
    template <typename Command>
    Expected<typename Command::Result> send(const Command& cmd);
};
```

## Usage

```cpp
caudio::client::Client client("/path/to/library.db");
auto result = client.send(caudio::ipc::command::Playback::Play{});
```

## Thread safety

A `Client` instance is safe for concurrent `probe()` and `send()` calls because
configuration is immutable after construction. Each call uses its own transport
connection.

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/client/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/client)
