---
title: IPC — caudio::ipc
description: Daemon protocol, framing, commands, results, shared-memory status
---

# IPC — `caudio::ipc`

The `caudio::ipc` component provides the daemon protocol, framing, command and
result types, and shared-memory status snapshots.

## Message framing

```
+--------+---------------------+
| 4 bytes|   N bytes JSON  |
| (BE u32)|   payload       |
+--------+---------------------+
```

## Command and Result

```cpp
#include <caudio/ipc/command.hpp>
#include <caudio/ipc/result.hpp>

// Commands: Command variant with type discriminator
// Results: Result variant with ok/error fields
```

## Shared memory status

```cpp
#include <caudio/ipc/shm_status.hpp>

struct ShmStatus {
    uint64_t seq{0};
    double position{0.0};
    double duration{0.0};
    float volume{1.0f};
    bool muted{false};
    int state{0};
    int64_t track_id{0};
    size_t queueSize{0};
    char title[256]{0};
    char artist[256]{0};
};
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/ipc/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/ipc)
- [IPC Protocol concept](/concepts/ipc-protocol) — protocol concepts and wire format
