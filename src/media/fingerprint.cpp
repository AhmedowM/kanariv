#include <blake3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <media/fingerprint.hpp>
#include <system_error>

namespace kanariv::media::internal {

constexpr int fingerprintSize = 32;

auto computeFingerprint(const std::filesystem::path& path)
    -> std::expected<std::array<uint8_t, fingerprintSize>, kanariv::utils::Error> {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, ec.message())};
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open file")};
    }
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    thread_local std::array<std::byte, sample> buf{};
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sample));
    auto n = static_cast<size_t>(f.gcount());
    if (n != 0) {
        blake3_hasher_update(&hasher, buf.data(), n);
    }
    if (sz > sample) {
        f.clear();
        f.seekg(static_cast<std::streamoff>(sz - sample), std::ios::beg);
        if (f) {
            f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sample));
            n = static_cast<size_t>(f.gcount());
            if (n != 0) {
                blake3_hasher_update(&hasher, buf.data(), n);
            }
        }
    }
    auto sz64 = static_cast<uint64_t>(sz);
    blake3_hasher_update(&hasher, &sz64, sizeof(sz64));
    uint32_t ver = 1;
    blake3_hasher_update(&hasher, &ver, sizeof(ver));
    std::array<uint8_t, fingerprintSize> out{};
    blake3_hasher_finalize(&hasher, out.data(), out.size());
    return out;
}

} // namespace kanariv::media::internal
