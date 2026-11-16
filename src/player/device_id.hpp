#pragma once

// Pure helpers for the opaque device ids DeviceList reports. Kept free of
// miniaudio so the rules (tag, hex payload, legacy numeric index) can be
// unit-tested without a device backend.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kanariv::player::detail {

/// Encodes a backend-specific device identifier as `<backend>:<lowercase-hex>`.
/// The value is opaque: it is stable across runs for backends whose id is
/// stable, but may change when the backend or the system changes. An empty
/// payload means the backend's default device.
inline std::string formatDeviceId(std::string_view backend, std::span<const std::uint8_t> raw) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(backend.size() + 1 + raw.size() * 2);
    out.append(backend);
    out.push_back(':');
    for (const std::uint8_t b : raw) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0f]);
    }
    return out;
}

/// Returns the backend tag of an encoded device id, or nullopt when the string
/// is not one (`"auto"`, a legacy numeric index, a plain device name, or a
/// malformed id whose payload is not lowercase hex).
inline std::optional<std::string_view> deviceIdBackend(std::string_view id) {
    const std::size_t colon = id.find(':');
    if (colon == std::string_view::npos || colon == 0 || (id.size() - colon - 1) % 2 != 0)
        return std::nullopt;
    for (std::size_t i = colon + 1; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return std::nullopt;
    }
    return id.substr(0, colon);
}

} // namespace kanariv::player::detail
