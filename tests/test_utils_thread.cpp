#include <catch2/catch_test_macros.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/thread.hpp>
#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>
namespace kanariv::utils::test {

bool thread_sleep_timing() {
    auto s = std::chrono::steady_clock::now();
    sleepFor(std::chrono::milliseconds(50));
    auto e = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(e - s).count();
    if (elapsed < 30)
        return false;
    if (elapsed >= 500)
        return false;
    s = std::chrono::steady_clock::now();
    sleepFor(std::chrono::milliseconds(0));
    e = std::chrono::steady_clock::now();
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(e - s).count();
    if (elapsed >= 100)
        return false;
    return true;
}

bool thread_jthread_basic() {
    int v = 0;
    {
        std::jthread t([&](std::stop_token) { v++; });
        if (!t.joinable())
            return false;
    }
    if (v != 1)
        return false;
    return true;
}

bool thread_parallel_10() {
    std::vector<int> vals(10, 0);
    std::vector<std::jthread> threads;
    threads.reserve(10);
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&, i](std::stop_token) { vals[i]++; });
    }
    for (auto& t : threads)
        t.join();
    for (int i = 0; i < 10; ++i)
        if (vals[i] != 1)
            return false;
    return true;
}

bool thread_setname_current() {
    auto r = setThreadName("ca-test");
    if (!r.has_value() && r.error().code != StatusCode::Unsupported &&
        r.error().code != StatusCode::InvalidArg)
        return false;
    auto r2 = setThreadName("");
    if (!r2.has_value() && r2.error().code != StatusCode::Unsupported &&
        r2.error().code != StatusCode::InvalidArg)
        return false;
    return true;
}

bool thread_setname_jthread() {
    std::jthread jt([](std::stop_token st) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        while (!st.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    auto r = setThreadName(jt, "worker-1");
    if (!r.has_value() && r.error().code != StatusCode::Unsupported &&
        r.error().code != StatusCode::State)
        return false;
    jt.request_stop();
    jt.join();
    std::jthread empty;
    auto r2 = setThreadName(empty, "nope");
    if (r2.has_value())
        return false;
    if (r2.error().code != StatusCode::State)
        return false;
    return true;
}

bool thread_100_stress() {
    const int N = 100;
    std::vector<int> vals(N, 0);
    std::vector<std::jthread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i](std::stop_token) { vals[i]++; });
    }
    for (auto& t : threads)
        t.join();
    int sum = 0;
    for (int i = 0; i < N; ++i)
        sum += vals[i];
    if (sum != N)
        return false;
    return true;
}

bool thread_sleepForMs() {
    auto s = std::chrono::steady_clock::now();
    sleepForMs(5);
    auto e = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(e - s).count();
    if (elapsed >= 200)
        return false;
    return true;
}

} // namespace kanariv::utils::test

using namespace kanariv::utils::test;

TEST_CASE("Thread sleepFor timing", "[utils][thread]") {
    REQUIRE(thread_sleep_timing());
}

TEST_CASE("Thread jthread creation and join", "[utils][thread]") {
    REQUIRE(thread_jthread_basic());
}

TEST_CASE("Thread parallel 10 jthreads", "[utils][thread]") {
    REQUIRE(thread_parallel_10());
}

TEST_CASE("Thread setThreadName current", "[utils][thread]") {
    REQUIRE(thread_setname_current());
}

TEST_CASE("Thread setThreadName via jthread", "[utils][thread]") {
    REQUIRE(thread_setname_jthread());
}

TEST_CASE("Thread 100 stress", "[utils][thread]") {
    REQUIRE(thread_100_stress());
}

TEST_CASE("Thread sleepForMs wrapper", "[utils][thread]") {
    REQUIRE(thread_sleepForMs());
}
