#include <array>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/utils/ring.hpp>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <thread>
namespace kanariv::utils::test {

bool ring_write_read_wrap() {
    SpscRing<float> r{8192, 2};
    std::array<float, 4> in{0.1f, 0.2f, 0.3f, 0.4f};
    if (r.write(in) != 2)
        return false;
    std::array<float, 4> out{};
    if (r.read(out) != 2)
        return false;
    for (size_t i = 0; i < 4; ++i)
        if (out[i] != in[i])
            return false;
    return true;
}

bool ring_basic_mono() {
    SpscRing<float> r{8, 1};
    if (r.availableRead() != 0)
        return false;
    if (r.availableWrite() != 8)
        return false;
    std::array<float, 4> w{1, 2, 3, 4};
    if (r.write(w) != 4)
        return false;
    if (r.availableRead() != 4)
        return false;
    if (r.availableWrite() != 4)
        return false;
    std::array<float, 4> ro{};
    if (r.read(ro) != 4)
        return false;
    for (int i = 0; i < 4; ++i)
        if (ro[i] != w[i])
            return false;
    if (r.availableRead() != 0)
        return false;
    return true;
}

bool ring_channels() {
    SpscRing<float> s{4, 2};
    if (s.availableWrite() != 4)
        return false;
    std::array<float, 6> stereoIn{1, 2, 3, 4, 5, 6};
    if (s.write(stereoIn) != 3)
        return false;
    if (s.availableRead() != 3)
        return false;
    if (s.availableWrite() != 1)
        return false;
    std::array<float, 6> stereoOut{};
    if (s.read(stereoOut) != 3)
        return false;
    for (int i = 0; i < 6; ++i)
        if (stereoOut[i] != stereoIn[i])
            return false;
    SpscRing<float> q{2, 4};
    std::array<float, 8> quadIn{1, 2, 3, 4, 5, 6, 7, 8};
    if (q.write(quadIn) != 2)
        return false;
    std::array<float, 8> quadOut{};
    if (q.read(quadOut) != 2)
        return false;
    for (int i = 0; i < 8; ++i)
        if (quadOut[i] != quadIn[i])
            return false;
    return true;
}

bool ring_wrap() {
    SpscRing<float> r{8, 1};
    std::array<float, 8> data{};
    for (int i = 0; i < 8; ++i)
        data[i] = static_cast<float>(i);
    if (r.write(std::span<const float>(data.data(), 6)) != 6)
        return false;
    std::array<float, 4> out{};
    if (r.read(std::span<float>(out.data(), 4)) != 4)
        return false;
    if (out[0] != 0 || out[3] != 3)
        return false;
    std::array<float, 6> wrapIn{};
    for (int i = 0; i < 6; ++i)
        wrapIn[i] = static_cast<float>(6 + i);
    if (r.write(wrapIn) != 6)
        return false;
    std::array<float, 8> wrapOut{};
    if (r.read(wrapOut) != 8)
        return false;
    if (wrapOut[0] != 4 || wrapOut[1] != 5)
        return false;
    if (wrapOut[2] != 6 || wrapOut[7] != 11)
        return false;
    return true;
}

bool ring_truncation() {
    SpscRing<float> r{4, 1};
    std::array<float, 8> in{1, 2, 3, 4, 5, 6, 7, 8};
    if (r.write(in) != 4)
        return false;
    if (r.availableRead() != 4)
        return false;
    if (r.availableWrite() != 0)
        return false;
    if (r.write(std::span<const float>(in.data(), 2)) != 0)
        return false;
    std::array<float, 8> out{};
    if (r.read(std::span<float>(out.data(), 8)) != 4)
        return false;
    if (out[0] != 1 || out[3] != 4)
        return false;
    if (r.availableRead() != 0)
        return false;
    if (r.read(std::span<float>(out.data(), 2)) != 0)
        return false;
    return true;
}

bool ring_available_reset() {
    SpscRing<float> r{10, 1};
    if (r.availableRead() != 0)
        return false;
    if (r.availableWrite() != 10)
        return false;
    std::array<float, 5> d{1, 2, 3, 4, 5};
    if (r.write(d) != 5)
        return false;
    if (r.availableRead() != 5)
        return false;
    if (r.availableWrite() != 5)
        return false;
    std::array<float, 3> o{};
    if (r.read(o) != 3)
        return false;
    if (r.availableRead() != 2)
        return false;
    if (r.availableWrite() != 8)
        return false;
    r.reset();
    if (r.availableRead() != 0)
        return false;
    if (r.availableWrite() != 10)
        return false;
    std::array<float, 10> full{};
    for (int i = 0; i < 10; ++i)
        full[i] = static_cast<float>(i);
    if (r.write(full) != 10)
        return false;
    if (r.availableRead() != 10)
        return false;
    std::array<float, 10> out{};
    if (r.read(out) != 10)
        return false;
    for (int i = 0; i < 10; ++i)
        if (out[i] != full[i])
            return false;
    return true;
}

bool ring_10k_loop() {
    SpscRing<float> r{256, 1};
    for (int iter = 0; iter < 10000; ++iter) {
        std::array<float, 16> wi{};
        std::array<float, 16> ro{};
        for (int i = 0; i < 16; ++i)
            wi[i] = static_cast<float>(iter * 16 + i);
        if (r.write(wi) != 16)
            return false;
        if (r.availableRead() < 16)
            return false;
        if (r.read(ro) != 16)
            return false;
        for (int i = 0; i < 16; ++i)
            if (ro[i] != wi[i])
                return false;
        if (r.availableRead() != 0)
            return false;
    }
    return true;
}

bool ring_concurrent_spsc() {
    SpscRing<float> r{128, 1};
    const int total = 10000;
    std::jthread prod([&](std::stop_token) {
        int produced = 0;
        while (produced < total) {
            std::array<float, 32> buf{};
            int batch = 32;
            if (produced + batch > total)
                batch = total - produced;
            for (int i = 0; i < batch; ++i)
                buf[i] = static_cast<float>(produced + i);
            auto n = r.write(std::span<const float>(buf.data(), static_cast<std::size_t>(batch)));
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            produced += static_cast<int>(n);
        }
    });
    int consumed = 0;
    float expected = 0;
    std::array<float, 32> out{};
    while (consumed < total) {
        auto n = r.read(std::span<float>(out.data(), 32));
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i] != expected)
                return false;
            expected += 1.0f;
        }
        consumed += static_cast<int>(n);
    }
    prod.join();
    if (consumed != total)
        return false;
    if (r.availableRead() != 0)
        return false;
    return true;
}

} // namespace kanariv::utils::test

using namespace kanariv::utils::test;

TEST_CASE("SpscRing write/read wrap", "[utils][ring]") {
    REQUIRE(ring_write_read_wrap());
}

TEST_CASE("SpscRing basic SPSC mono", "[utils][ring]") {
    REQUIRE(ring_basic_mono());
}

TEST_CASE("SpscRing channel counts stereo/quad", "[utils][ring]") {
    REQUIRE(ring_channels());
}

TEST_CASE("SpscRing wrap-around", "[utils][ring]") {
    REQUIRE(ring_wrap());
}

TEST_CASE("SpscRing truncation over-read/write", "[utils][ring]") {
    REQUIRE(ring_truncation());
}

TEST_CASE("SpscRing available and reset", "[utils][ring]") {
    REQUIRE(ring_available_reset());
}

TEST_CASE("SpscRing 10k loop stress", "[utils][ring]") {
    REQUIRE(ring_10k_loop());
}

TEST_CASE("SpscRing concurrent SPSC", "[utils][ring]") {
    REQUIRE(ring_concurrent_spsc());
}
