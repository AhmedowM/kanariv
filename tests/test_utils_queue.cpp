#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

#include "../src/utils/mpsc_queue.hpp"

namespace kanariv::utils::test {

bool queue_busy_on_full() {
    MpscQueue<int> q{2};
    if (!q.push(1).has_value())
        return false;
    if (!q.push(2).has_value())
        return false;
    auto r = q.push(3);
    if (r.has_value())
        return false;
    if (r.error().code != StatusCode::Busy)
        return false;
    return true;
}

bool queue_push_pop_fifo() {
    MpscQueue<int> q{4};
    if (!q.push(10).has_value())
        return false;
    if (!q.push(20).has_value())
        return false;
    auto v1 = q.pop();
    if (!v1.has_value() || *v1 != 10)
        return false;
    auto v2 = q.pop();
    if (!v2.has_value() || *v2 != 20)
        return false;
    auto v3 = q.pop();
    if (v3.has_value())
        return false;
    if (v3.error().code != StatusCode::State)
        return false;
    return true;
}

bool queue_wrap() {
    MpscQueue<int> q{4};
    for (int i = 0; i < 3; ++i)
        if (!q.push(i).has_value())
            return false;
    auto o = q.pop();
    if (!o.has_value() || *o != 0)
        return false;
    o = q.pop();
    if (!o.has_value() || *o != 1)
        return false;
    for (int i = 3; i < 6; ++i)
        if (!q.push(i).has_value())
            return false;
    for (int i = 2; i < 6; ++i) {
        auto v = q.pop();
        if (!v.has_value() || *v != i)
            return false;
    }
    if (q.pop().has_value())
        return false;
    MpscQueue<int> q2{4};
    for (int iter = 0; iter < 1000; ++iter) {
        if (!q2.push(iter).has_value())
            return false;
        auto v = q2.pop();
        if (!v.has_value() || *v != iter)
            return false;
    }
    MpscQueue<int> q1{1};
    for (int i = 0; i < 100; ++i) {
        if (!q1.push(i).has_value())
            return false;
        if (q1.push(i).has_value())
            return false;
        auto v = q1.pop();
        if (!v.has_value())
            return false;
        if (q1.pop().has_value())
            return false;
    }
    return true;
}

bool queue_mpsc_thread() {
    MpscQueue<int> q{32};
    const int NPROD = 4;
    const int PER = 500;
    const int TOTAL = NPROD * PER;
    std::vector<std::jthread> threads;
    threads.reserve(NPROD);
    for (int id = 0; id < NPROD; ++id) {
        threads.emplace_back([&, id](std::stop_token) {
            for (int i = 0; i < PER; ++i) {
                int val = id * 100000 + i;
                while (true) {
                    auto r = q.push(val);
                    if (r.has_value())
                        break;
                    if (r.error().code != StatusCode::Busy)
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });
    }
    std::vector<int> collected;
    collected.reserve(TOTAL);
    while ((int)collected.size() < TOTAL) {
        auto r = q.pop();
        if (r.has_value()) {
            collected.push_back(*r);
        } else {
            if (r.error().code != StatusCode::State)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (auto& t : threads)
        t.join();
    while (true) {
        auto r = q.pop();
        if (!r.has_value())
            break;
        collected.push_back(*r);
    }
    if ((int)collected.size() != TOTAL)
        return false;
    std::sort(collected.begin(), collected.end());
    int idx = 0;
    for (int id = 0; id < NPROD; ++id) {
        for (int i = 0; i < PER; ++i) {
            int exp = id * 100000 + i;
            if (collected[idx] != exp)
                return false;
            ++idx;
        }
    }
    return true;
}

bool queue_10k_loop() {
    MpscQueue<int> q{64};
    for (int i = 0; i < 10000; ++i) {
        if (!q.push(i).has_value())
            return false;
        auto v = q.pop();
        if (!v.has_value() || *v != i)
            return false;
    }
    return true;
}

} // namespace kanariv::utils::test

using namespace kanariv::utils::test;

TEST_CASE("MpscQueue busy on full", "[utils][queue]") {
    REQUIRE(queue_busy_on_full());
}

TEST_CASE("MpscQueue push/pop FIFO and empty State", "[utils][queue]") {
    REQUIRE(queue_push_pop_fifo());
}

TEST_CASE("MpscQueue wrap-around", "[utils][queue]") {
    REQUIRE(queue_wrap());
}

TEST_CASE("MpscQueue MPSC thread producers", "[utils][queue]") {
    REQUIRE(queue_mpsc_thread());
}

TEST_CASE("MpscQueue 10k loop stress", "[utils][queue]") {
    REQUIRE(queue_10k_loop());
}
