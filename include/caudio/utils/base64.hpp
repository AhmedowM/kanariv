#pragma once

/**
 * @file base64.hpp
 * @brief Base64 encode/decode (RFC 4648 with standard padding).
 * @ingroup caudio_utils
 * @details Header-only implementation; no third-party dependencies.
 * Encoding uses the standard A-Z a-z 0-9 + / alphabet with '=' padding.
 * Decoding accepts input with or without padding and ignores ASCII whitespace.
 */

#include <array>
#include <caudio/utils/error.hpp>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace caudio::utils {

/**
 * @brief Stateless RFC 4648 Base64 conversion helpers.
 * @ingroup caudio_utils
 *
 * All methods are static and allocate their result. They do not retain
 * references to input data and are safe to call concurrently.
 */
class Base64 {
  public:
    /**
     * @brief Encodes binary data to a base64 string.
     * @param data Input bytes.
     * @return Base64-encoded string with standard '=' padding.
     */
    static std::string encode(std::span<const std::byte> data);

    /**
     * @brief Decodes a base64 string to binary data.
     * @param encoded Base64-encoded input (padding optional; whitespace ignored).
     * @return Decoded bytes, or Error InvalidArg on invalid characters or
     *         incorrect padding.
     */
    static std::expected<std::vector<std::byte>, caudio::utils::Error>
    decode(std::string_view encoded);

    /**
     * @brief Detects a MIME type from a file path extension.
     * @param path File path (extension is used).
     * @return MIME type string (e.g., "image/jpeg"), or "application/octet-stream"
     *         when the extension is unknown.
     */
    [[nodiscard]] static std::string mimeTypeFromPath(std::string_view path);

  private:
    static constexpr char kEncodeTable[65] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    static std::array<std::int8_t, 256> buildDecodeTable() {
        std::array<std::int8_t, 256> table{};
        for (std::size_t i = 0; i < 256; ++i)
            table[i] = -1;
        for (std::size_t i = 0; i < 64; ++i)
            table[static_cast<std::size_t>(kEncodeTable[i])] = static_cast<std::int8_t>(i);
        return table;
    }

    static inline std::array<std::int8_t, 256> kDecodeTable = buildDecodeTable();

    static std::int8_t decodeChar(char c) noexcept {
        return kDecodeTable[static_cast<std::size_t>(static_cast<std::uint8_t>(c))];
    }
};

inline std::string Base64::encode(std::span<const std::byte> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        std::uint32_t triple =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
            static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 12) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 6) & 0x3F]);
        out.push_back(kEncodeTable[triple & 0x3F]);
        i += 3;
    }

    std::size_t remaining = data.size() - i;
    if (remaining == 1) {
        std::uint32_t triple = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        std::uint32_t triple =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 12) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

inline std::expected<std::vector<std::byte>, caudio::utils::Error>
Base64::decode(std::string_view encoded) {
    // Remove ASCII whitespace. Use an unsigned conversion because the
    // classification functions are undefined for negative char values.
    std::string clean;
    clean.reserve(encoded.size());
    for (char c : encoded) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            clean.push_back(c);
    }

    if (clean.empty())
        return std::vector<std::byte>{};

    // RFC 4648: total encoded length (including '=' padding) must be a
    // multiple of 4.
    if (clean.size() % 4 != 0)
        return std::unexpected{caudio::utils::makeError(
            caudio::utils::StatusCode::InvalidArg,
            "base64: invalid encoded length")};

    // Count trailing padding ('=' characters at the end).
    std::size_t padCount = 0;
    while (padCount < clean.size() &&
           clean[clean.size() - 1 - padCount] == '=')
        ++padCount;
    if (padCount > 2)
        return std::unexpected{caudio::utils::makeError(
            caudio::utils::StatusCode::InvalidArg,
            "base64: too many padding characters")};

    // Padding must appear only at the end of the string.
    const std::size_t dataEnd = clean.size() - padCount;
    if (clean.find('=') < dataEnd)
        return std::unexpected{caudio::utils::makeError(
            caudio::utils::StatusCode::InvalidArg,
            "base64: padding character in middle of string")};

    // RFC 4648 section 3.5: validate unused padding bits are zero.
    // With one '=': the last 2 bits of the last data character must be 0.
    // With two '=': the last 4 bits of the last data character must be 0.
    if (padCount == 1 && dataEnd > 0) {
        std::int8_t v = decodeChar(clean[dataEnd - 1]);
        if (v < 0 || (v & 0x03) != 0)
            return std::unexpected{caudio::utils::makeError(
                caudio::utils::StatusCode::InvalidArg,
                "base64: non-zero padding bits")};
    } else if (padCount == 2 && dataEnd > 0) {
        std::int8_t v = decodeChar(clean[dataEnd - 1]);
        if (v < 0 || (v & 0x0F) != 0)
            return std::unexpected{caudio::utils::makeError(
                caudio::utils::StatusCode::InvalidArg,
                "base64: non-zero padding bits")};
    }

    // Decode: process 4 characters at a time, yielding up to 3 bytes per
    // group. Left-shift the partial buffer so its significant bits align
    // with the top of a 24-bit word before extracting bytes.
    std::vector<std::byte> out;
    out.reserve((clean.size() / 4) * 3 - padCount);

    for (std::size_t i = 0; i < clean.size(); i += 4) {
        std::uint32_t buffer = 0;
        std::size_t validChars = 0;
        for (int j = 0; j < 4; ++j) {
            char c = clean[i + j];
            if (c == '=')
                break;
            std::int8_t v = decodeChar(c);
            if (v < 0)
                return std::unexpected{caudio::utils::makeError(
                    caudio::utils::StatusCode::InvalidArg,
                    "base64: invalid character '" + std::string(1, c) + "'")};
            buffer = (buffer << 6) | static_cast<std::uint32_t>(v & 0x3F);
            ++validChars;
        }

        // A group must have at least 2 data characters to produce output.
        if (validChars < 2)
            return std::unexpected{caudio::utils::makeError(
                caudio::utils::StatusCode::InvalidArg,
                "base64: invalid encoded length")};

        // Align the significant bits to the top of the 24-bit buffer.
        buffer <<= (4 - validChars) * 6;

        std::size_t outBytes;
        if (validChars == 2)
            outBytes = 1;
        else if (validChars == 3)
            outBytes = 2;
        else
            outBytes = 3;

        if (outBytes >= 1)
            out.push_back(static_cast<std::byte>((buffer >> 16) & 0xFF));
        if (outBytes >= 2)
            out.push_back(static_cast<std::byte>((buffer >> 8) & 0xFF));
        if (outBytes >= 3)
            out.push_back(static_cast<std::byte>(buffer & 0xFF));
    }

    return out;
}

inline std::string Base64::mimeTypeFromPath(std::string_view path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot == path.size() - 1)
        return "application/octet-stream";
    std::string ext;
    ext.reserve(path.size() - dot - 1);
    for (std::size_t i = dot + 1; i < path.size(); ++i) {
        char c = path[i];
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "png")
        return "image/png";
    if (ext == "gif")
        return "image/gif";
    if (ext == "bmp")
        return "image/bmp";
    if (ext == "webp")
        return "image/webp";
    if (ext == "tiff" || ext == "tif")
        return "image/tiff";
    return "application/octet-stream";
}

} // namespace caudio::utils
