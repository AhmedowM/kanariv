---
title: Media — caudio::media
description: Reading, decoding, metadata, and library discovery (Source, Reader, scan)
---

# Media — `caudio::media`

The `caudio::media` component provides reading, decoding, metadata extraction,
and library discovery primitives.

## Reader

Abstract base class for audio data input sources:

```cpp
#include <caudio/media/reader.hpp>

class Reader {
public:
    virtual ~Reader() = default;
    virtual std::size_t read(std::span<std::byte> dst) = 0;
    [[nodiscard]] virtual Expected<void> seek(int64_t offset, int whence) = 0;
    [[nodiscard]] virtual int64_t tell() noexcept = 0;
    [[nodiscard]] virtual int64_t size() noexcept = 0;
    [[nodiscard]] virtual bool seekable() const noexcept = 0;
};
```

Concrete implementations:

| Class | Description |
|---|---|
| `FileReader` | File-based reading with 64-bit seek support |
| `MemoryReader` | In-memory buffer reading |
| `DescriptorReader` | Native file-descriptor reading |

## Source

Abstract factory for readers:

```cpp
#include <caudio/media/source.hpp>

class Source {
public:
    virtual ~Source() = default;
    virtual Expected<std::unique_ptr<Reader>> open() = 0;
};

class PathSource : public Source {  // filesystem path
};
class DescriptorSource : public Source {  // native descriptor
};
```

## Scanning

```cpp
#include <caudio/media/scan.hpp>

auto tracks = caudio::media::scanTracksToVector(".");
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/media/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/media)
