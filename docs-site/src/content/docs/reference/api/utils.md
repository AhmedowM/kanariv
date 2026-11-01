---
title: Utils — caudio::utils
description: Errors, logging, JSON, queues, threads, and versioning
---

# Utils — `caudio::utils`

The `caudio::utils` component provides foundational types and utilities used
across the entire library: error handling, logging, JSON, ring buffers, queues,
threads, and versioning.

## Error handling

```cpp
#include <caudio/utils/error.hpp>

struct Error {
    StatusCode code{StatusCode::Ok};
    std::string message{};
};

enum class StatusCode : std::uint8_t {
    Ok = 0, InvalidArg = 1, NotFound = 2, Unsupported = 3, Io = 4,
    Device = 5, State = 6, NoMem = 7, Internal = 8,
    AlreadyExists = 9, Busy = 10, Corrupt = 11, NoSpace = 12
};

template <typename T>
using Expected = std::expected<T, Error>;
```

All fallible operations return `std::expected<T, Error>`. Use
`caudio::utils::makeError(code, message)` to construct errors.

## Logging

```cpp
#include <caudio/utils/log.hpp>

caudio::utils::Logger* logger = ...;
logger->log(caudio::utils::LogLevel::Info, "message");
```

## Version

```cpp
#include <caudio/version.hpp>

caudio::utils::version();  // returns version string
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/utils/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/utils)
