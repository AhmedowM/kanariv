#include <sqlite3.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <thread>

#include "../src/db/database_access.hpp"
#include "../src/db/write_thread.hpp"
#include "../src/utils/mpsc_queue.hpp"

using namespace kanariv::utils;

TEST_CASE("WriterThread bounded full BUSY and flush timeout 200ms", "[db][writer_thread]") {
    // Use MpscQueue directly to verify Bounded full -> Busy
    kanariv::utils::MpscQueue<int> q(2);
    REQUIRE(q.push(1).has_value());
    REQUIRE(q.push(2).has_value());
    auto r = q.push(3);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == kanariv::utils::StatusCode::Busy);

    // Real WriterThread: capacity 2, do NOT open so queue never drains -> flush should timeout
    // after ~200ms with Busy
    {
        kanariv::db::WriterThread wt(2);
        // push two ops without opening thread -> queue stays full
        // need to construct WriteOperation-like pushes via WriterThread::push
        // Use sql strings; stmt nullptr
        auto pr1 = wt.push("SELECT 1;", {});
        REQUIRE(pr1.has_value());
        auto pr2 = wt.push("SELECT 1;", {});
        REQUIRE(pr2.has_value());
        auto pr3 = wt.push("SELECT 1;", {});
        REQUIRE(!pr3.has_value());
        REQUIRE(pr3.error().code == kanariv::utils::StatusCode::Busy);
        auto start = std::chrono::steady_clock::now();
        auto fr = wt.flush();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        REQUIRE(!fr.has_value());
        REQUIRE(fr.error().code == kanariv::utils::StatusCode::Busy);
        REQUIRE(elapsed.count() >= 190);
        REQUIRE(elapsed.count() < 600);
        // after draining by close, flush should succeed quickly
        wt.close();
        // after close queue drained
        auto fr2 = wt.flush();
        REQUIRE(fr2.has_value());
    }
    // Flush succeeds when queue empty
    {
        kanariv::db::WriterThread wt2(4);
        auto start = std::chrono::steady_clock::now();
        auto fr = wt2.flush();
        REQUIRE(fr.has_value());
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        REQUIRE(elapsed.count() < 50);
    }
    // After open, flush drains within 200ms (worker processes)
    {
        // Take the handle from the db component (Database::open +
        // DatabaseAccess) instead of calling sqlite3_open_v2 here: the writer
        // thread must run on a handle owned by the SQLite copy the db
        // component links. Shared builds enforce one copy via sqlite3.dll;
        // the ownership rule keeps the test valid for static builds too.
        auto opened = kanariv::db::Database::open(":memory:");
        REQUIRE(opened.has_value());
        sqlite3* db = kanariv::db::DatabaseAccess::handle(**opened);
        REQUIRE(db != nullptr);
        kanariv::db::WriterThread wt(4);
        wt.open(db);
        auto prCreate = wt.push("CREATE TABLE t(id INTEGER)", {});
        REQUIRE(prCreate.has_value());
        // push an insert using SQL string (stmt=nullptr)
        std::atomic<bool> cbCalled{false};
        auto pr = wt.push("INSERT INTO t(id) VALUES (1)",
                          [&](std::expected<void, kanariv::utils::Error> e) {
                              cbCalled.store(true);
                              (void)e;
                          });
        REQUIRE(pr.has_value());
        auto fr = wt.flush();
        REQUIRE(fr.has_value());
        // wait for callback outside lock (worker calls cb)
        auto start = std::chrono::steady_clock::now();
        while (!cbCalled.load() &&
               std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(cbCalled.load());
        wt.close();
    }
}
