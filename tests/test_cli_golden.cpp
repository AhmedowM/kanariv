#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <common.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifndef TEST_CLI_BIN
#error "TEST_CLI_BIN must be defined to the kanariv executable under test"
#endif

using namespace kanariv::test_helpers;

namespace {

// ---------------------------------------------------------------------------
// Minimal cross-platform process runner: '<bin> args...' with stdout/stderr
// captured to temp files. Quotes every argument; works under cmd and sh.
// ---------------------------------------------------------------------------

struct RunResult {
    int exitCode{-1};
    std::string out{};
    std::string err{};
};

std::string shellQuote(const std::string& s) {
    std::string q = "\"";
    for (char c : s) {
        if (c == '"')
            q += "\\\"";
        else
            q += c;
    }
    q += "\"";
    return q;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

RunResult runCli(const std::vector<std::string>& args) {
    namespace fs = std::filesystem;
    static std::atomic<int> ctr{9000};
    fs::path work = fs::temp_directory_path() /
                    ("clirun_" + std::to_string(ctr.fetch_add(1)) + "_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(work, ec);
    fs::path outF = work / "out.txt";
    fs::path errF = work / "err.txt";
    std::string cmd = TEST_CLI_BIN; // already a quoted literal via -DTEST_CLI_BIN="..."
    for (const auto& a : args)
        cmd += " " + shellQuote(a);
    cmd += " >" + shellQuote(outF.string()) + " 2>" + shellQuote(errF.string());
    // Redirect the child's temp dir into this run's work dir so default-path
    // artifacts (e.g. a no-config daemon's kanariv.log) are removed with it.
    std::string oldTemp = [] {
        const char* v = std::getenv("TEMP");
        return v ? std::string(v) : std::string{};
    }();
    std::string oldTmp = [] {
        const char* v = std::getenv("TMP");
        return v ? std::string(v) : std::string{};
    }();
    std::string oldTmpDir = [] {
        const char* v = std::getenv("TMPDIR");
        return v ? std::string(v) : std::string{};
    }();
#ifdef _WIN32
    _putenv_s("TEMP", work.string().c_str());
    _putenv_s("TMP", work.string().c_str());
    int rc = std::system(cmd.c_str());
    _putenv_s("TEMP", oldTemp.c_str());
    _putenv_s("TMP", oldTmp.c_str());
#else
    setenv("TMPDIR", work.string().c_str(), 1);
    int rc = std::system(cmd.c_str());
    if (oldTmpDir.empty())
        unsetenv("TMPDIR");
    else
        setenv("TMPDIR", oldTmpDir.c_str(), 1);
#endif
#ifdef _WIN32
    int code = rc;
#else
    int code = (rc == -1) ? -1 : WEXITSTATUS(rc);
#endif
    RunResult r{code, readFile(outF), readFile(errF)};
    fs::remove_all(work, ec);
    return r;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Count table rows prefixed with "> " (current-track marker).
int markedRows(const std::string& out) {
    int n = 0;
    std::size_t pos = 0;
    while (pos < out.size()) {
        std::size_t eol = out.find('\n', pos);
        std::string line = out.substr(pos, eol == std::string::npos ? eol : eol - pos);
        if (line.size() >= 2 && line[0] == '>' && line[1] == ' ')
            ++n;
        if (eol == std::string::npos)
            break;
        pos = eol + 1;
    }
    return n;
}

// Hermetic config: every CLI invocation that can reach the daemon gets
// --config pointing at a fresh temp dir, so db/socket/pid never touch the
// real user locations (socket derives from the unique dbPath hash).
std::vector<std::string> withCfg(const std::string& tag, std::vector<std::string> rest) {
    namespace fs = std::filesystem;
    fs::path dir = tempDirPath(tag);
    fs::path cfg = dir / "config.json";
    {
        std::ofstream f(cfg, std::ios::binary);
        f << "{\"dbPath\": \"" << dir.generic_string() << "/library.db\"}";
    }
    rest.insert(rest.begin(), {"--config", cfg.generic_string()});
    return rest;
}

std::vector<std::string> withCfg(const std::string& tag, std::initializer_list<std::string> rest) {
    return withCfg(tag, std::vector<std::string>(rest));
}

} // namespace

TEST_CASE("cli help surfaces groups", "[cli]") {
    auto r = runCli({"--help"});
    REQUIRE(r.exitCode == 0);
    REQUIRE(contains(r.out, "SUBCOMMANDS"));
    for (const char* tok : {"start", "shutdown", "play", "status", "queue", "playlist", "library",
                            "tag", "history", "config", "device", "info"}) {
        INFO("missing token: " << tok);
        REQUIRE(contains(r.out, tok));
    }
}

TEST_CASE("cli group help lists leaf commands", "[cli]") {
    auto q = runCli({"queue", "--help"});
    REQUIRE(q.exitCode == 0);
    REQUIRE(contains(q.out, "shuffle"));
    REQUIRE(contains(q.out, "repeat"));
    auto l = runCli({"library", "scan", "--help"});
    REQUIRE(l.exitCode == 0);
    REQUIRE(contains(l.out, "full-hash"));
}

TEST_CASE("cli version prints", "[cli]") {
    auto r = runCli({"--version"});
    REQUIRE(r.exitCode == 0);
    REQUIRE(!r.out.empty());
}

TEST_CASE("cli version subcommand prints details", "[cli]") {
    auto r = runCli({"version"});
    REQUIRE(r.exitCode == 0);
    for (const char* tok : {"kanariv ", "short:", "commit:", "compiler:", "std:", "runtime:",
                            "build:", "os:", "arch:"}) {
        INFO("missing token: " << tok);
        REQUIRE(contains(r.out, tok));
    }
    auto j = runCli({"version", "--json"});
    REQUIRE(j.exitCode == 0);
    for (const char* key :
         {"\"version\":", "\"commit\":", "\"compiler\":", "\"os\":", "\"arch\":"}) {
        INFO("missing key: " << key);
        REQUIRE(contains(j.out, key));
    }
}

TEST_CASE("cli rejects unknown command", "[cli]") {
    auto r = runCli({"frobnicate"});
    REQUIRE(r.exitCode == 1);
    REQUIRE(contains(r.out + r.err, "frobnicate"));
}

TEST_CASE("cli rejects missing required arg", "[cli]") {
    auto r = runCli({"queue", "add"});
    REQUIRE(r.exitCode == 1);
}

TEST_CASE("cli validates log level", "[cli]") {
    auto r = runCli({"--log-level", "bogus", "status"});
    REQUIRE(r.exitCode == 1);
    REQUIRE(contains(r.err, "log-level"));
}

TEST_CASE("cli seek validates time grammar", "[cli]") {
    auto bad = runCli(withCfg("gold_seek1", {"seek", "abc"}));
    REQUIRE(bad.exitCode == 1);
    REQUIRE(contains(bad.err, "invalid time"));
    REQUIRE(contains(bad.err, "mm:ss"));
    auto colons = runCli(withCfg("gold_seek2", {"seek", "1:2:3:4"}));
    REQUIRE(colons.exitCode == 1);
    REQUIRE(contains(colons.err, "too many colons"));
    auto overflow = runCli(withCfg("gold_seek3", {"seek", "1:70"}));
    REQUIRE(overflow.exitCode == 1);
    REQUIRE(contains(overflow.err, "ss < 60"));
    auto overflowH = runCli(withCfg("gold_seek4", {"seek", "1:2:70"}));
    REQUIRE(overflowH.exitCode == 1);
    REQUIRE(contains(overflowH.err, "ss < 60"));
}

TEST_CASE("cli volume validates range", "[cli]") {
    auto bad = runCli(withCfg("gold_vol1", {"volume", "loud"}));
    REQUIRE(bad.exitCode == 1);
    REQUIRE(contains(bad.err, "invalid volume"));
    REQUIRE(contains(bad.err, "0-100"));
    auto range = runCli(withCfg("gold_vol2", {"volume", "101"}));
    REQUIRE(range.exitCode == 1);
    REQUIRE(contains(range.err, "out of range"));
}

TEST_CASE("cli shuffle/repeat echo valid modes", "[cli]") {
    auto s = runCli(withCfg("gold_shuf", {"queue", "shuffle", "bogus"}));
    REQUIRE(s.exitCode == 1);
    REQUIRE(contains(s.err, "invalid mode"));
    REQUIRE(contains(s.err, "on|off"));
    auto r = runCli(withCfg("gold_rep", {"queue", "repeat", "bogus"}));
    REQUIRE(r.exitCode == 1);
    REQUIRE(contains(r.err, "invalid mode"));
    REQUIRE(contains(r.err, "off|one|all"));
}

TEST_CASE("cli status watch validates interval", "[cli]") {
    auto r = runCli(withCfg("gold_int", {"status", "--watch", "--interval", "0"}));
    REQUIRE(r.exitCode == 1);
    REQUIRE(contains(r.err, "positive"));
}

TEST_CASE("cli enum options reject out-of-set values", "[cli]") {
    auto e = runCli(withCfg(
        "gold_exp", {"playlist", "export", "--pid", "1", "--path", "x", "--format", "bogus"}));
    REQUIRE(e.exitCode != 0);
    auto t = runCli(
        withCfg("gold_tag", {"tag", "edit", "--id", "1", "--field", "bogus", "--value", "x"}));
    REQUIRE(t.exitCode != 0);
}

TEST_CASE("cli no-daemon errors point at start", "[cli]") {
    for (const char* sub : {"status"}) {
        auto r = runCli(withCfg("gold_nd1", {sub}));
        REQUIRE(r.exitCode == 105);
        REQUIRE(contains(r.err, "daemon not running"));
        REQUIRE(contains(r.err, "kanariv start"));
    }
    auto q = runCli(withCfg("gold_nd2", {"queue", "list"}));
    REQUIRE(q.exitCode == 105);
    REQUIRE(contains(q.err, "daemon not running"));
    auto v = runCli(withCfg("gold_nd3", {"volume"}));
    REQUIRE(v.exitCode == 105);
    REQUIRE(contains(v.err, "daemon not running"));
}

TEST_CASE("cli direct play needs existing files", "[cli]") {
    // NOTE: relative name (CLI11 treats /abs paths as flags on Windows).
    auto r = runCli(withCfg("gold_pos1", {"gold-missing-xyz.mp3"}));
    REQUIRE(r.exitCode == 1);
    REQUIRE(contains(r.err, "no such file"));
    auto g = runCli(withCfg("gold_pos2", {"./*.zzz-no-match"}));
    REQUIRE(g.exitCode == 0);
    REQUIRE(contains(g.err, "No files matched"));
}

TEST_CASE("cli daemon lifecycle", "[cli]") {
    namespace fs = std::filesystem;
    fs::path dir = tempDirPath("gold_life");
    fs::path cfg = dir / "config.json";
    {
        std::ofstream f(cfg, std::ios::binary);
        f << "{\"dbPath\": \"" << dir.generic_string() << "/library.db\"}";
    }
    std::string cfgS = cfg.generic_string();
    auto run = [&](std::vector<std::string> a) {
        a.insert(a.begin(), {"--config", cfgS});
        return runCli(a);
    };
    // Best-effort shutdown on any path: a REQUIRE failure must not leak a
    // daemon (on Windows a live daemon locks kanariv.exe for the next build).
    struct DaemonGuard {
        decltype(run)* runFn;
        ~DaemonGuard() {
            try {
                (*runFn)({"shutdown"});
            } catch (...) {
            }
        }
    } guard{&run};
    auto start = run({"start"});
    // Bounded retry: daemon spawn can transiently fail on a loaded box
    // (AV scan/lock/scheduler); start converges (already-running succeeds),
    // so one retry only absorbs the window -- persistent failure still
    // fails below. INFO names the cause next time (spawn error vs
    // socket-unreachable).
    if (start.exitCode != 0)
        start = run({"start"});
    INFO("start stdout: " << start.out << "\nstart stderr: " << start.err);
    REQUIRE(start.exitCode == 0);
    REQUIRE(contains(start.out, "started"));
    // Poll until the daemon answers (spawn is async).
    bool up = false;
    for (int i = 0; i < 150 && !up; ++i) {
        auto s = run({"status"});
        up = (s.exitCode == 0);
        if (!up)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(up);
    // The daemon writes its startup line to the db-adjacent log file.
    bool logged = false;
    for (int i = 0; i < 50 && !logged; ++i) {
        std::ifstream lf(dir / "kanariv.log", std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
        logged = content.find("daemon started") != std::string::npos;
        if (!logged)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(logged);
    auto js = run({"status", "--json"});
    REQUIRE(js.exitCode == 0);
    REQUIRE(contains(js.out, "\"state\""));
    auto vol = run({"volume"});
    REQUIRE(vol.exitCode == 0);
    REQUIRE(contains(vol.out, "Volume:"));
    auto set72 = run({"volume", "72"});
    REQUIRE(set72.exitCode == 0);
    auto vol2 = run({"volume"});
    REQUIRE(contains(vol2.out, "72%"));
    auto mute = run({"volume", "mute"});
    REQUIRE(mute.exitCode == 0);
    REQUIRE(contains(mute.out, "Volume: muted"));
    auto unmute = run({"volume", "unmute"});
    REQUIRE(unmute.exitCode == 0);
    REQUIRE(contains(unmute.out, "72%"));
    auto zero = run({"volume", "0"});
    REQUIRE(zero.exitCode == 0);
    REQUIRE(contains(zero.out, "Volume: muted"));
    auto unmute2 = run({"volume", "unmute"});
    REQUIRE(unmute2.exitCode == 0);
    REQUIRE(contains(unmute2.out, "72%"));
    auto clamp = run({"volume", "+150"});
    REQUIRE(clamp.exitCode == 0);
    REQUIRE(contains(clamp.out, "100%"));
    // One dummy audio file: scan tolerates missing metadata.
    fs::path music = dir / "music";
    std::error_code ec;
    fs::create_directories(music, ec);
    {
        std::ofstream f(music / "song.mp3", std::ios::binary);
        f << "dummy audio content for cli golden test";
    }
    auto scan = run({"library", "scan", "--path", music.generic_string()});
    REQUIRE(scan.exitCode == 0);
    REQUIRE(contains(scan.out, "1 track added"));
    {
        std::ofstream f(music / "full.mp3", std::ios::binary);
        f << "full-hash dummy";
    }
    auto scanFull = run({"library", "scan", "--path", music.generic_string(), "--full-hash"});
    REQUIRE(scanFull.exitCode == 0);
    REQUIRE(contains(scanFull.out, "1 track added"));
    auto stats = run({"library", "stats"});
    REQUIRE(stats.exitCode == 0);
    REQUIRE(contains(stats.out, "Tracks: 2"));
    auto search = run({"library", "search", "song"});
    REQUIRE(search.exitCode == 0);
    REQUIRE(contains(search.out, "1 track found:"));
    REQUIRE(contains(search.out, "song.mp3"));
    auto add = run({"queue", "add", (music / "song.mp3").generic_string()});
    REQUIRE(add.exitCode == 0);
    auto ql = run({"queue", "list"});
    REQUIRE(ql.exitCode == 0);
    REQUIRE(contains(ql.out, "Queues (1):"));
    auto qj = run({"queue", "list", "--json"});
    REQUIRE(qj.exitCode == 0);
    REQUIRE(contains(qj.out, "\"Queues\""));
    auto qa = run({"queue", "queues"});
    REQUIRE(qa.exitCode == 0);
    REQUIRE(contains(qa.out, "Queues (1):"));
    auto qt = run({"queue", "tracks"});
    REQUIRE(qt.exitCode == 0);
    REQUIRE(contains(qt.out, "Queue (1 tracks):"));
    REQUIRE(markedRows(qt.out) == 0);
    auto qto = run({"queue", "tracks", "--order", "added"});
    REQUIRE(qto.exitCode == 0);
    REQUIRE(contains(qto.out, "Queue (1 tracks):"));
    auto qtj = run({"queue", "tracks", "--json"});
    REQUIRE(qtj.exitCode == 0);
    REQUIRE(contains(qtj.out, "\"QueueTracks\""));
    // Shuffle-order path (single-track perm trivially matches).
    auto shufT = run({"queue", "shuffle", "on"});
    REQUIRE(shufT.exitCode == 0);
    auto qts = run({"queue", "tracks"});
    REQUIRE(qts.exitCode == 0);
    REQUIRE(contains(qts.out, "Queue (1 tracks):"));
    auto shufT2 = run({"queue", "shuffle", "off"});
    REQUIRE(shufT2.exitCode == 0);
    auto save = run({"playlist", "save", "goldmix"});
    REQUIRE(save.exitCode == 0);
    auto plt = run({"playlist", "tracks", "1"});
    REQUIRE(plt.exitCode == 0);
    REQUIRE(contains(plt.out, "Playlist (1 tracks):"));
    auto plj = run({"playlist", "tracks", "1", "--json"});
    REQUIRE(plj.exitCode == 0);
    REQUIRE(contains(plj.out, "\"PlaylistData\""));
    auto mkpl = run({"playlist", "create", "built"});
    REQUIRE(mkpl.exitCode == 0);
    REQUIRE(contains(mkpl.out, "built"));
    long long builtPid = 0;
    REQUIRE(std::sscanf(mkpl.out.c_str(), "Created playlist %lld", &builtPid) == 1);
    auto padd = run({"playlist", "add", std::to_string(builtPid), "--id", "1"});
    REQUIRE(padd.exitCode == 0);
    REQUIRE(contains(padd.out, "Added song.mp3 to playlist"));
    auto pdup = run({"playlist", "add", std::to_string(builtPid), "--id", "1"});
    REQUIRE(pdup.exitCode == 0);
    REQUIRE(contains(pdup.err, "already on playlist"));
    auto ptrk = run({"playlist", "tracks", std::to_string(builtPid)});
    REQUIRE(contains(ptrk.out, "Playlist (1 tracks):"));
    {
        std::ofstream f(dir / "imp.m3u", std::ios::binary);
        f << "#EXTM3U\n"
          << (music / "song.mp3").generic_string() << "\n"
          << (music / "song.mp3").generic_string() << "\n"
          << (music / "missing.mp3").generic_string() << "\n";
    }
    auto imp = run({"playlist", "import", (dir / "imp.m3u").generic_string()});
    REQUIRE(imp.exitCode == 0);
    REQUIRE(contains(imp.out, "Imported playlist"));
    REQUIRE(contains(imp.out, "1 matched"));
    REQUIRE(contains(imp.out, "1 skipped"));
    REQUIRE(contains(imp.out, "1 duplicate"));
    auto stMp = run({"library", "stats", "--top-tracks", "0"});
    REQUIRE(stMp.exitCode == 0);
    REQUIRE(contains(stMp.out, "Library Stats (Detailed):"));
    auto stQ = run({"library", "stats", "--queue", "all"});
    REQUIRE(stQ.exitCode == 0);
    REQUIRE(contains(stQ.out, "Queues ("));
    auto stP = run({"library", "stats", "--playlist", "1"});
    REQUIRE(stP.exitCode == 0);
    REQUIRE(contains(stP.out, "Playlist 'goldmix': 1 track"));
    auto libRm = run({"library", "remove", (music / "full.mp3").generic_string()});
    REQUIRE(libRm.exitCode == 0);
    REQUIRE(contains(libRm.out, "Removed from library full.mp3"));
    auto histNeg = run({"history", "list", "--limit", "-5"});
    REQUIRE(histNeg.exitCode == 0);
    {
        std::ofstream f(music / "song2.mp3", std::ios::binary);
        f << "second dummy";
    }
    auto addDir = run({"queue", "add", music.generic_string()});
    REQUIRE(addDir.exitCode == 0);
    REQUIRE(contains(addDir.out, "Added full.mp3"));
    REQUIRE(contains(addDir.out, "Added song2.mp3"));
    REQUIRE(contains(addDir.out, "2 tracks added"));
    REQUIRE(contains(addDir.err, "already in queue"));
    auto addIdMissing = run({"queue", "add", "--id", "99999"});
    REQUIRE(addIdMissing.exitCode == 109);
    auto addIdBad = run({"queue", "add", "--id", "abc"});
    REQUIRE(addIdBad.exitCode == 1);
    auto addMissing = run({"queue", "add", (music / "nope.mp3").generic_string()});
    REQUIRE(addMissing.exitCode == 1);
    REQUIRE(contains(addMissing.err, "no such file"));
    auto addEmpty = run({"queue", "add", (music / "*.xyz").generic_string()});
    REQUIRE(addEmpty.exitCode == 0);
    REQUIRE(contains(addEmpty.err, "No files matched"));
    auto addSearchEmpty = run({"queue", "add", "xyz-no-match", "--search"});
    REQUIRE(addSearchEmpty.exitCode != 0);
    auto remPos = run({"queue", "remove", "--pos", "1"});
    REQUIRE(remPos.exitCode == 0);
    REQUIRE(contains(remPos.out, "Removed from queue full.mp3"));
    REQUIRE(!contains(remPos.out, "tracks"));
    auto remId = run({"queue", "remove", "--id", "1"});
    REQUIRE(remId.exitCode == 0);
    REQUIRE(contains(remId.out, "Removed from queue song.mp3"));
    auto remPath = run({"queue", "remove", (music / "song2.mp3").generic_string()});
    REQUIRE(remPath.exitCode == 0);
    REQUIRE(contains(remPath.out, "Removed from queue song2.mp3"));
    auto remEmpty = run({"queue", "remove", "--pos", "0"});
    REQUIRE(remEmpty.exitCode == 1);
    auto remBare = run({"queue", "remove", "0"});
    REQUIRE(remBare.exitCode == 1);
    auto mkq = run({"queue", "create", "mix2"});
    REQUIRE(mkq.exitCode == 0);
    REQUIRE(contains(mkq.out, "mix2"));
    long long newQid = 0;
    REQUIRE(std::sscanf(mkq.out.c_str(), "Created queue %lld", &newQid) == 1);
    REQUIRE(newQid > 1);
    REQUIRE(run({"queue", "switch", std::to_string(newQid)}).exitCode == 0);
    REQUIRE(run({"queue", "switch", "1"}).exitCode == 0);
    auto delActive = run({"queue", "delete", "1"});
    REQUIRE(delActive.exitCode != 0);
    REQUIRE(contains(delActive.err, "active queue"));
    auto delQ = run({"queue", "delete", std::to_string(newQid)});
    REQUIRE(delQ.exitCode == 0);
    REQUIRE(contains(delQ.out, "Deleted queue"));
    auto load = run({"playlist", "load", "1"});
    REQUIRE(load.exitCode == 0);
    REQUIRE(contains(load.out, "Loaded playlist 1 into queue"));
    long long loadedPlaylistId = 0;
    long long loadedQid = 0;
    REQUIRE(std::sscanf(load.out.c_str(), "Loaded playlist %lld into queue %lld",
                        &loadedPlaylistId, &loadedQid) == 2);
    REQUIRE(run({"queue", "switch", std::to_string(loadedQid)}).exitCode == 0);
    auto trkN = run({"queue", "tracks"});
    REQUIRE(contains(trkN.out, "Queue (1 tracks):"));
    REQUIRE(run({"queue", "switch", "1"}).exitCode == 0);
    auto trk1 = run({"queue", "tracks"});
    REQUIRE(contains(trk1.out, "Queue (0 tracks):"));
    auto loadRep = run({"playlist", "load", "1", "--replace"});
    REQUIRE(loadRep.exitCode == 0);
    REQUIRE(contains(loadRep.out, "Replaced queue 1"));
    auto trk2 = run({"queue", "tracks"});
    REQUIRE(contains(trk2.out, "Queue (1 tracks):"));
    auto addPl = run({"queue", "add", "--playlist", "1", "--replace"});
    REQUIRE(addPl.exitCode == 0);
    REQUIRE(contains(addPl.out, "Added song.mp3"));
    REQUIRE(contains(addPl.out, "1 track added"));
    auto tedit = run({"tag", "edit", "1", "title", "Hello"});
    REQUIRE(tedit.exitCode == 0);
    auto tgf = run({"tag", "get", "1", "title"});
    REQUIRE(tgf.exitCode == 0);
    REQUIRE(contains(tgf.out, "Hello"));
    auto tgj = run({"tag", "get", "1", "year", "--json"});
    REQUIRE(tgj.exitCode == 0);
    REQUIRE(contains(tgj.out, "\"year\""));
    auto tgbad = run({"tag", "get", "1", "bogus"});
    REQUIRE(tgbad.exitCode == 1);
    auto tgmiss = run({"tag", "get", "99999"});
    REQUIRE(tgmiss.exitCode == 109);
    REQUIRE(contains(tgmiss.err, "99999"));
    auto swmiss = run({"queue", "switch", "999"});
    REQUIRE(swmiss.exitCode == 109);
    REQUIRE(contains(swmiss.err, "999"));
    {
        std::ofstream f(dir / "good.json", std::ios::binary);
        f << "{\"custom_key\": \"v\"}";
    }
    auto impOk = run({"config", "import", (dir / "good.json").generic_string()});
    REQUIRE(impOk.exitCode == 0);
    {
        std::ofstream f(dir / "bad.json", std::ios::binary);
        f << "{oops";
    }
    auto impBad = run({"config", "import", (dir / "bad.json").generic_string()});
    REQUIRE(impBad.exitCode != 0);
    {
        std::ofstream f(dir / "badtype.json", std::ios::binary);
        f << "{\"logLevel\": \"loud\"}";
    }
    auto impType = run({"config", "import", (dir / "badtype.json").generic_string()});
    REQUIRE(impType.exitCode != 0);
    // Restore the hermetic config replaced by the import above.
    {
        std::ofstream f(cfg, std::ios::binary);
        f << "{\"dbPath\": \"" << dir.generic_string() << "/library.db\"}";
    }
    REQUIRE(run({"config", "set", "cli_golden_key", "hello"}).exitCode == 0);
    auto pauseIdle = run({"pause"});
    REQUIRE(pauseIdle.exitCode == 0);
    REQUIRE(contains(pauseIdle.err, "nothing playing"));
    auto prevStart = run({"prev"});
    REQUIRE(prevStart.exitCode == 0);
    REQUIRE(contains(prevStart.err, "at queue start"));
    auto set = run({"config", "set", "cli_golden_key", "hello"});
    REQUIRE(set.exitCode == 0);
    REQUIRE(set.out.empty());
    auto get = run({"config", "get", "cli_golden_key"});
    REQUIRE(get.exitCode == 0);
    REQUIRE(contains(get.out, "hello"));
    auto getJ = run({"config", "get", "cli_golden_key", "--json"});
    REQUIRE(getJ.exitCode == 0);
    REQUIRE(contains(getJ.out, "\"value\""));
    auto setNum = run({"config", "set", "cli_golden_num", "42"});
    REQUIRE(setNum.exitCode == 0);
    auto list = run({"config", "list"});
    REQUIRE(list.exitCode == 0);
    REQUIRE(contains(list.out, "cli_golden_key = hello"));
    REQUIRE(contains(list.out, "cli_golden_num = 42"));
    // logLevel accepts names (canonicalized) and ints, rejects garbage live.
    auto lvlName = run({"config", "set", "logLevel", "trace"});
    REQUIRE(lvlName.exitCode == 0);
    auto lvlGet = run({"config", "get", "logLevel"});
    REQUIRE(lvlGet.exitCode == 0);
    REQUIRE(contains(lvlGet.out, "0"));
    auto lvlNum = run({"config", "set", "logLevel", "2"});
    REQUIRE(lvlNum.exitCode == 0);
    auto lvlBad = run({"config", "set", "logLevel", "loud"});
    REQUIRE(lvlBad.exitCode == 106);
    REQUIRE(contains(lvlBad.err, "logLevel"));
    // Empty the queue, then play must fail fast without touching audio.
    auto clear = run({"queue", "clear"});
    REQUIRE(clear.exitCode == 0);
    REQUIRE(contains(clear.out, "cleared"));
    auto resumeEmpty = run({"resume"});
    REQUIRE(resumeEmpty.exitCode != 0);
    REQUIRE(contains(resumeEmpty.err, "empty queue"));
    auto shuf = run({"queue", "shuffle", "on"});
    REQUIRE(shuf.exitCode == 0);
    REQUIRE(contains(shuf.out, "Shuffle: on"));
    auto shufBare = run({"queue", "shuffle"});
    REQUIRE(shufBare.exitCode == 0);
    REQUIRE(contains(shufBare.out, "Shuffle: off"));
    auto rep1 = run({"queue", "repeat"});
    REQUIRE(rep1.exitCode == 0);
    REQUIRE(contains(rep1.out, "Repeat: all"));
    auto rep2 = run({"queue", "repeat"});
    REQUIRE(rep2.exitCode == 0);
    REQUIRE(contains(rep2.out, "Repeat: one"));
    auto repOff = run({"queue", "repeat", "off"});
    REQUIRE(repOff.exitCode == 0);
    REQUIRE(contains(repOff.out, "Repeat: off"));
    auto play = run({"play"});
    REQUIRE(play.exitCode != 0);
    auto down = run({"shutdown"});
    REQUIRE(down.exitCode == 0);
    REQUIRE(contains(down.out, "stopped"));
    auto after = run({"status"});
    REQUIRE(after.exitCode == 105);
    REQUIRE(contains(after.err, "daemon not running"));
    // Full cleanup verified by a clean restart on the same config. Bounded
    // retry: a previous shutdown's pipe/lock release races the new spawn
    // (a persistent failure still fails loudly below -- this only absorbs
    // the teardown window).
    RunResult restart{};
    bool restarted = false;
    for (int i = 0; i < 50 && !restarted; ++i) {
        restart = run({"start"});
        restarted = (restart.exitCode == 0);
        if (!restarted)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(restarted);
    REQUIRE(restart.exitCode == 0);
    REQUIRE(contains(restart.out, "started"));
    auto down2 = run({"shutdown"});
    REQUIRE(down2.exitCode == 0);
    REQUIRE(contains(down2.out, "stopped"));
    // Play autostarts the daemon when down (empty queue errors, daemon stays).
    // Bounded retry: the autostart can race a dying daemon's lock; accept the
    // expected "empty queue" only, fail on anything persistent.
    RunResult autoPlay{};
    bool autoDone = false;
    for (int i = 0; i < 50 && !autoDone; ++i) {
        autoPlay = run({"play"});
        autoDone = (autoPlay.exitCode == 109) && !contains(autoPlay.err, "daemon not running") &&
                   !contains(autoPlay.err, "socket not reachable");
        if (!autoDone)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(autoPlay.exitCode == 109);
    REQUIRE(contains(autoPlay.err, "empty queue"));
    auto autoStatus = run({"status"});
    REQUIRE(autoStatus.exitCode == 0);
    // config get answers locally without a daemon... (daemon is up here;
    // covered daemonless below).
    auto down3 = run({"shutdown"});
    REQUIRE(down3.exitCode == 0);
    auto localGet = run({"config", "get", "cli_golden_key"});
    REQUIRE(localGet.exitCode == 0);
    REQUIRE(contains(localGet.out, "hello"));
    fs::remove_all(dir, ec);
}

TEST_CASE("cli tag edit writes file", "[cli]") {
    namespace fs = std::filesystem;
    fs::path dir = tempDirPath("gold_tagfile");
    fs::path cfg = dir / "config.json";
    {
        std::ofstream f(cfg, std::ios::binary);
        f << "{\"dbPath\": \"" << dir.generic_string() << "/library.db\"}";
    }
    std::string cfgS = cfg.generic_string();
    auto run = [&](std::vector<std::string> a) {
        a.insert(a.begin(), {"--config", cfgS});
        return runCli(a);
    };
    struct DaemonGuard {
        decltype(run)* runFn;
        ~DaemonGuard() {
            try {
                (*runFn)({"shutdown"});
            } catch (...) {
            }
        }
    } guard{&run};
#ifdef TEST_DATA_DIR
    fs::path src = fs::path(std::string(TEST_DATA_DIR)) / "sample.mp3";
#else
    fs::path src;
#endif
    if (src.empty() || !fs::exists(src)) {
        SKIP("no fixture sample.mp3");
    }
    fs::path music = dir / "music";
    std::error_code ec;
    fs::create_directories(music, ec);
    fs::copy_file(src, music / "song.mp3", fs::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);
    auto start = run({"start"});
    // Same transient-spawn retry as the lifecycle test above.
    if (start.exitCode != 0)
        start = run({"start"});
    INFO("start stdout: " << start.out << "\nstart stderr: " << start.err);
    REQUIRE(start.exitCode == 0);
    bool up = false;
    for (int i = 0; i < 150 && !up; ++i) {
        auto s = run({"status"});
        up = (s.exitCode == 0);
        if (!up)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(up);
    // Single-file ensure (no scan needed); fresh database, so id 1.
    auto add = run({"library", "add", (music / "song.mp3").generic_string()});
    REQUIRE(add.exitCode == 0);
    auto edit = run({"tag", "edit", "1", "title", "Golden Title"});
    REQUIRE(edit.exitCode == 0);
    auto get = run({"tag", "get", "1"});
    REQUIRE(get.exitCode == 0);
    REQUIRE(contains(get.out, "Golden Title"));
    // Re-adding hits path dedup (same id): the row survived the rewrite.
    auto addAgain = run({"library", "add", (music / "song.mp3").generic_string()});
    REQUIRE(addAgain.exitCode == 0);
    auto get2 = run({"tag", "get", "1", "title"});
    REQUIRE(get2.exitCode == 0);
    REQUIRE(contains(get2.out, "Golden Title"));
    fs::remove_all(dir, ec);
}

TEST_CASE("cli playback one-liners", "[cli]") {
    KANARIV_SKIP_IF_NOAUDIO();
    namespace fs = std::filesystem;
    fs::path dir = tempDirPath("gold_play");
    fs::path cfg = dir / "config.json";
    {
        std::ofstream f(cfg, std::ios::binary);
        f << "{\"dbPath\": \"" << dir.generic_string() << "/library.db\"}";
    }
    std::string cfgS = cfg.generic_string();
    auto run = [&](std::vector<std::string> a) {
        a.insert(a.begin(), {"--config", cfgS});
        return runCli(a);
    };
    // Same best-effort shutdown guard as the lifecycle test above.
    struct PlaybackGuard {
        decltype(run)* runFn;
        ~PlaybackGuard() {
            try {
                (*runFn)({"shutdown"});
            } catch (...) {
            }
        }
    } playGuard{&run};
    auto EpicFail = [&](const char* what) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        FAIL(what);
    };
    // stop is idempotent server-side (already-stopped still succeeds), so a
    // single retry covers a transient IPC cancel under full-suite load.
    auto runStop = [&](const char* what) {
        auto r = run({"stop"});
        if (r.exitCode != 0)
            r = run({"stop"});
        if (r.exitCode != 0)
            EpicFail(what);
        return r;
    };
    auto startR = run({"start"});
    // Same transient-spawn retry as the lifecycle/tag tests.
    if (startR.exitCode != 0)
        startR = run({"start"});
    INFO("start stdout: " << startR.out << "\nstart stderr: " << startR.err);
    if (startR.exitCode != 0)
        EpicFail("daemon did not start");
    bool up = false;
    for (int i = 0; i < 150 && !up; ++i) {
        up = (run({"status"}).exitCode == 0);
        if (!up)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!up)
        EpicFail("daemon never came up");
    auto dev = run({"device", "list", "--json"});
    if (!contains(dev.out, "\"devices\"") || contains(dev.out, "\"devices\": []")) {
        run({"shutdown"});
        std::error_code ec;
        fs::remove_all(dir, ec);
        SKIP("no audio device for playback one-liners");
    }
    // Deterministic fixtures: the sub-second sample files can naturally end
    // (and gapless-advance) between two CLI spawns under full-suite load, so
    // every track-identity assertion below would race wall-clock playback.
    // Synthesize 6 s 16-bit stereo tones instead; container is irrelevant to
    // this CLI-shape test. (Kept the `ogg` variable name for continuity.)
    auto writeTone = [&](const char* name, double freq) {
        fs::path p = dir / name;
        std::ofstream f(p, std::ios::binary);
        if (!f)
            EpicFail("could not write synth fixture");
        constexpr uint32_t rate = 44100;
        const uint32_t frames = rate * 6;
        const uint32_t bytes = frames * 4;
        auto put32 = [&](uint32_t v) {
            f.put(static_cast<char>(v & 0xFF));
            f.put(static_cast<char>((v >> 8) & 0xFF));
            f.put(static_cast<char>((v >> 16) & 0xFF));
            f.put(static_cast<char>((v >> 24) & 0xFF));
        };
        auto put16 = [&](uint16_t v) {
            f.put(static_cast<char>(v & 0xFF));
            f.put(static_cast<char>((v >> 8) & 0xFF));
        };
        f.write("RIFF", 4);
        put32(36 + bytes);
        f.write("WAVEfmt ", 8);
        put32(16);
        put16(1);
        put16(2);
        put32(rate);
        put32(rate * 4);
        put16(4);
        put16(16);
        f.write("data", 4);
        put32(bytes);
        for (uint32_t i = 0; i < frames; ++i) {
            double t = static_cast<double>(i) / static_cast<double>(rate);
            auto s = static_cast<int16_t>(std::sin(2.0 * 3.141592653589793 * freq * t) * 20000);
            put16(static_cast<uint16_t>(s));
            put16(static_cast<uint16_t>(s));
        }
        f.close();
        if (!f)
            EpicFail("synth fixture write failed");
        return p;
    };
    fs::path wav = writeTone("toneA.wav", 440.0);
    fs::path ogg = writeTone("toneB.wav", 660.0);
    REQUIRE(run({"library", "add", wav.generic_string()}).exitCode == 0);
    REQUIRE(run({"queue", "add", wav.generic_string()}).exitCode == 0);
    REQUIRE(fs::exists(ogg));
    REQUIRE(run({"library", "add", ogg.generic_string()}).exitCode == 0);
    REQUIRE(run({"queue", "add", ogg.generic_string()}).exitCode == 0);
    // Pin the track on repeat-one for the one-liner section: ten CLI spawns
    // can still outlast the 6 s tone under full-suite load, and every
    // track-identity assertion below would then race wall-clock playback.
    // Repeat is switched back off after stopA, so the advance/end sections
    // below run exactly as before.
    REQUIRE(run({"queue", "repeat", "one"}).exitCode == 0);
    auto play = run({"play"});
    REQUIRE(play.exitCode == 0);
    REQUIRE(contains(play.out, "Playing "));
    REQUIRE(contains(play.out, "toneA.wav"));
    auto pause = run({"pause"});
    REQUIRE(pause.exitCode == 0);
    REQUIRE(contains(pause.out, "Paused toneA.wav at "));
    auto resume = run({"resume"});
    REQUIRE(resume.exitCode == 0);
    REQUIRE(contains(resume.out, "Resuming toneA.wav from "));
    // Bookmarks: add/list/goto/remove roundtrip on the current track.
    // Pause first: several CLI spawns would otherwise race the 6 s tone's
    // natural end / gapless advance under full-suite load.
    auto bmPause = run({"pause"});
    REQUIRE(bmPause.exitCode == 0);
    auto bmAdd = run({"bookmark", "add", "0:01", "--note", "test"});
    REQUIRE(bmAdd.exitCode == 0);
    REQUIRE(contains(bmAdd.out, "Bookmark added at 00:01"));
    auto bmList = run({"bookmark", "list"});
    REQUIRE(bmList.exitCode == 0);
    REQUIRE(contains(bmList.out, "Bookmarks (1):"));
    REQUIRE(contains(bmList.out, "00:01"));
    REQUIRE(contains(bmList.out, "test"));
    auto bmJson = run({"bookmark", "list", "--json"});
    REQUIRE(bmJson.exitCode == 0);
    REQUIRE(contains(bmJson.out, "\"type\": \"Bookmarks\""));
    REQUIRE(contains(bmJson.out, "\"position_ms\": 1000"));
    auto bmGoto = run({"bookmark", "goto", "1"});
    REQUIRE(bmGoto.exitCode == 0);
    REQUIRE(contains(bmGoto.out, "Resuming toneA.wav from 00:01"));
    auto bmRemove = run({"bookmark", "remove", "1"});
    REQUIRE(bmRemove.exitCode == 0);
    REQUIRE(contains(bmRemove.out, "Bookmark 1 removed"));
    auto bmEmpty = run({"bookmark", "list"});
    REQUIRE(bmEmpty.exitCode == 0);
    REQUIRE(contains(bmEmpty.out, "Bookmarks (0):"));
    // Seek silence while playing (seek 0 stays at head, so no auto-advance
    // can fire before the assertions below).
    auto seek = run({"seek", "0"});
    REQUIRE(seek.exitCode == 0);
    REQUIRE(seek.out.empty());
    auto pauseA = run({"pause"});
    REQUIRE(pauseA.exitCode == 0);
    REQUIRE(contains(pauseA.out, "Paused toneA.wav at "));
    auto stop = runStop("stop failed");
    REQUIRE(contains(stop.out, "Stopped"));
    // Play-after-stop replays the stopped track instead of advancing.
    auto again = run({"play"});
    REQUIRE(again.exitCode == 0);
    REQUIRE(contains(again.out, "Playing toneA.wav"));
    auto pause2 = run({"pause"});
    REQUIRE(pause2.exitCode == 0);
    REQUIRE(contains(pause2.out, "Paused toneA.wav at "));
    auto resume2 = run({"resume"});
    REQUIRE(resume2.exitCode == 0);
    REQUIRE(contains(resume2.out, "Resuming toneA.wav from "));
    auto stopA = runStop("second stop failed");
    REQUIRE(run({"queue", "repeat", "off"}).exitCode == 0);
    auto nx1 = run({"next"});
    REQUIRE(nx1.exitCode == 0);
    REQUIRE(contains(nx1.out, "Playing toneA.wav"));
    auto nx = run({"next"});
    REQUIRE(nx.exitCode == 0);
    REQUIRE(contains(nx.out, "Playing toneB.wav"));
    // Current-track row in queue tracks is prefixed with '>'.
    auto qtm = run({"queue", "tracks"});
    REQUIRE(qtm.exitCode == 0);
    REQUIRE(contains(qtm.out, "Queue (2 tracks):"));
    REQUIRE(markedRows(qtm.out) == 1);
    auto stop2 = runStop("stop before idle pause failed");
    auto pauseIdle = run({"pause"});
    REQUIRE(pauseIdle.exitCode == 0);
    REQUIRE(contains(pauseIdle.err, "nothing playing"));
    // Resume-after-stop plays the stopped track from its head.
    auto resumeStopped = run({"resume"});
    REQUIRE(resumeStopped.exitCode == 0);
    REQUIRE(contains(resumeStopped.out, "Playing toneB.wav"));
    // Natural end with repeat Off stops instead of looping: seek near the
    // end of the 6 s tone, wait it out, then status must show Stopped and
    // play must wrap to the head (toneA).
    auto tail = run({"seek", "5"});
    REQUIRE(tail.exitCode == 0);
    REQUIRE(tail.out.empty());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto ended = run({"status"});
    REQUIRE(ended.exitCode == 0);
    REQUIRE(contains(ended.out, "State: Stopped"));
    auto rewind = run({"play"});
    REQUIRE(rewind.exitCode == 0);
    REQUIRE(contains(rewind.out, "Playing toneA.wav"));
    // Direct play opens a temporary queue; --save keeps it permanently.
    auto direct = run({wav.generic_string()});
    REQUIRE(direct.exitCode == 0);
    REQUIRE(contains(direct.out, "Playing toneA.wav (temporary queue)"));
    auto kept = run({"--save", ogg.generic_string()});
    REQUIRE(kept.exitCode == 0);
    REQUIRE(contains(kept.out, "Playing toneB.wav"));
    REQUIRE(!contains(kept.out, "temporary"));
    auto qltemp = run({"queue", "list"});
    REQUIRE(qltemp.exitCode == 0);
    REQUIRE(contains(qltemp.out, "temp: toneA.wav"));
    REQUIRE(run({"shutdown"}).exitCode == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
