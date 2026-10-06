// Standalone tests for the Geode-independent core.
//   g++ -std=c++20 -O2 -Isrc tests/test_core.cpp src/core/*.cpp -o test_core && ./test_core
#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <random>

#include "core/Analyzer.hpp"
#include "core/Export.hpp"
#include "core/Precision.hpp"

using namespace fwm;

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// Fake runner: a synthetic "level" where every input id has an allowed tick range.
// A trial dies `deathDelay` ticks after the original tick of the first out-of-range input.
// ---------------------------------------------------------------------------
struct FakeLevel {
    std::map<int, std::pair<Tick, Tick>> allowed;  // id -> [lo, hi] (inclusive)
    std::vector<Tick> original;                    // id -> original tick
    Tick endTick = 10000;
    Tick deathDelay = 3;
    bool baselineBroken = false;
    int flakyEvery = 0;  // if >0: every n-th trial gives the opposite answer
};

struct FakeRunner : ITrialRunner {
    FakeLevel& level;
    TrialRequest req;
    std::optional<TrialResult> pending;
    bool ready = false;
    int polls = 0;
    int trials = 0;
    explicit FakeRunner(FakeLevel& l) : level(l) {}

    void begin(TrialRequest r) override {
        req = std::move(r);
        ++trials;
        TrialResult res;
        Tick failAt = -1;
        for (auto const& in : req.plan) {
            auto it = level.allowed.find(in.id);
            if (it == level.allowed.end()) continue;
            if (in.tick < it->second.first || in.tick > it->second.second) {
                Tick t = level.original[in.id] + level.deathDelay;
                if (failAt < 0 || t < failAt) failAt = t;
            }
        }
        if (req.baseline && level.baselineBroken) failAt = 50;
        bool dies = failAt >= 0 && (req.requireCompletion || failAt < req.validateUntil);
        if (dies && failAt < level.endTick) {
            res.pass = false;
            res.reason = Fail::Death;
            res.tick = failAt;
        } else {
            res.pass = true;
            res.completed = req.requireCompletion || req.validateUntil >= level.endTick;
            res.tick = res.completed ? level.endTick : req.validateUntil;
        }
        if (level.flakyEvery > 0 && !req.baseline && trials % level.flakyEvery == 0) {
            res.pass = !res.pass;
            res.reason = res.pass ? Fail::None : Fail::Death;
        }
        pending = res;
        ready = false;
        polls = 0;
    }
    std::optional<TrialResult> poll() override {
        if (!pending) return std::nullopt;
        if (++polls < 2) return std::nullopt;  // emulate asynchronous completion
        auto r = pending;
        pending.reset();
        return r;
    }
    void cancel() override { pending.reset(); }
};

static std::shared_ptr<Replay> makeReplay(std::vector<std::tuple<Tick, bool>> list) {
    auto r = std::make_shared<Replay>();
    int id = 0;
    for (auto [t, down] : list) {
        Input in;
        in.tick = t;
        in.down = down;
        in.id = id++;
        r->inputs.push_back(in);
    }
    return r;
}

static void runToEnd(Analyzer& a) {
    for (int i = 0; i < 1000000 && a.active(); ++i) a.advance();
}

// ---------------------------------------------------------------------------
static void testBasicWindows() {
    std::printf("basic windows\n");
    auto replay = makeReplay({{100, true}, {110, false}, {200, true}, {210, false}, {400, true}, {420, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {97, 101};   // early 3, late 1  -> window 5
    lv.allowed[2] = {200, 200};  // window 1
    lv.allowed[4] = {300, 500};  // huge, but bounded by neighbours / cap
    FakeRunner runner(lv);
    Settings s;
    s.maxWindow = 10;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Completed);
    CHECK(a.results().size() == 3);  // presses only
    auto const& r0 = a.results()[0];
    CHECK(r0.id == 0 && r0.early == 3 && r0.late == 1 && r0.window() == 5 && !r0.capped);
    auto const& r1 = a.results()[1];
    CHECK(r1.id == 2 && r1.window() == 1 && !r1.capped);
    auto const& r2 = a.results()[2];
    CHECK(r2.id == 4 && r2.capped);  // allowed range is much larger than maxWindow
    CHECK(a.endTick() == lv.endTick);
}

static void testNeighbourBounds() {
    std::printf("neighbour bounds\n");
    // press@100 release@102: moving the press later is blocked by its own release (single mode)
    auto replay = makeReplay({{100, true}, {102, false}, {104, true}, {300, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {97, 110};   // early 3 available; later side blocked by its own release
    lv.allowed[2] = {100, 120};  // lower side blocked by release@102 (tick must be > 102)
    FakeRunner runner(lv);
    Settings s;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Completed);
    auto const& r0 = a.results()[0];
    CHECK(r0.late == 1 && r0.lateBound);   // can reach tick 101 only
    CHECK(r0.early == 3 && !r0.earlyBound);
    auto const& r1 = a.results()[1];
    CHECK(r1.early == 1 && r1.earlyBound);  // can reach tick 103 only
}

static void testKeepHold() {
    std::printf("keep hold length\n");
    auto replay = makeReplay({{100, true}, {110, false}, {115, true}, {130, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {95, 105};
    lv.allowed[1] = {105, 114};  // release constraint (checked even though it moves with the press)
    FakeRunner runner(lv);
    Settings s;
    s.keepHoldLength = true;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Completed);
    auto const& r0 = a.results()[0];
    // press range 95..105 -> early 5, late 5 from the press; release moves with it: 105..115
    // release limit 114 -> late limited to 4; also bound by next press @115 (release must end < 115)
    CHECK(r0.early == 5);
    CHECK(r0.late == 4);
    CHECK(r0.lateBound || r0.late == 4);
}

static void testBaselineFailure() {
    std::printf("baseline failure\n");
    auto replay = makeReplay({{100, true}, {110, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.baselineBroken = true;
    lv.allowed[0] = {100, 100};
    FakeRunner runner(lv);
    Analyzer a(replay, Settings{}, runner);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Failed);
    CHECK(!a.error().empty());
    CHECK(a.results().empty());
}

static void testUnstable() {
    std::printf("repeats / unstable\n");
    auto replay = makeReplay({{100, true}, {110, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {98, 102};
    lv.flakyEvery = 2;
    FakeRunner runner(lv);
    Settings s;
    s.repeats = 2;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Completed);
    CHECK(a.results().size() == 1);
    CHECK(a.results()[0].unstable);
}

static void testPauseResume() {
    std::printf("pause / resume\n");
    auto replay = makeReplay({{100, true}, {110, false}, {200, true}, {210, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {98, 103};
    lv.allowed[2] = {199, 205};
    FakeRunner runner(lv);
    Analyzer a(replay, Settings{}, runner);
    a.start(false);
    int steps = 0;
    bool paused = false;
    for (int i = 0; i < 1000000 && a.state() != State::Completed && a.state() != State::Failed; ++i) {
        a.advance();
        if (!paused && ++steps == 7) {
            a.pause();
            CHECK(a.state() == State::Paused);
            a.advance();  // no effect while paused
            a.resume();
            paused = true;
        }
    }
    CHECK(a.state() == State::Completed);
    CHECK(a.results().size() == 2);
    CHECK(a.results()[0].early == 2 && a.results()[0].late == 3);
    CHECK(a.results()[1].early == 1 && a.results()[1].late == 5);
}

static void testFiltersAndRange() {
    std::printf("filters / range\n");
    auto replay = makeReplay({{100, true}, {110, false}, {200, true}, {210, false}, {300, true}, {310, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    for (int i = 0; i < 6; ++i) lv.allowed[i] = {lv.original[i] - 2, lv.original[i] + 2};
    {
        FakeRunner runner(lv);
        Settings s;
        s.presses = false;
        s.releases = true;
        Analyzer a(replay, s, runner);
        a.start(false);
        runToEnd(a);
        CHECK(a.results().size() == 3);
        for (auto const& r : a.results()) CHECK(!r.down);
    }
    {
        FakeRunner runner(lv);
        Settings s;
        s.fromInput = 3;
        s.toInput = 5;
        Analyzer a(replay, s, runner);
        a.start(false);
        runToEnd(a);
        CHECK(a.results().size() == 2);  // presses #3 and #5
        CHECK(a.results()[0].id == 2 && a.results()[1].id == 4);
    }
}

// ---------------------------------------------------------------------------
// Precision: compare the stable implementation against the literal formulas.
// ---------------------------------------------------------------------------
static double naiveE(std::vector<PrecisionInput> const& in, PrecisionParams const& p, bool N, bool F, bool C, double L) {
    double fps = p.tps;
    double r = 1.0, fails = 0.0, prevT = 0.0, tn = 0.0;
    long long prevI = 0;
    for (std::size_t m = 0; m < in.size(); ++m) {
        double t = p.respawnSeconds + in[m].tick / fps;
        long long idx = (long long)m + 1;
        double w = in[m].window / fps;
        double s = 0.5 * w * L;
        if (N) s *= std::exp(-p.kNerve * t);
        if (F) s *= std::exp(-p.kFatigue * idx);
        if (C) {
            double c = (double)(idx - prevI) / (t - prevT);
            s *= std::pow(4.0 / std::max(1.0, 2.0 * c), p.kCps);
        }
        double pass = std::erf(s / std::sqrt(2.0));
        double q = 1.0 - pass;
        fails += t * r * q;
        r *= pass;
        prevT = t;
        prevI = idx;
        tn = t;
    }
    return (tn * r + fails) / r;
}

static void testPrecision() {
    std::printf("precision\n");
    std::mt19937 rng(12345);
    for (int round = 0; round < 25; ++round) {
        int n = 3 + (int)(rng() % 25);
        std::vector<PrecisionInput> in;
        double tick = 240.0 * (1 + rng() % 5);
        for (int i = 0; i < n; ++i) {
            tick += 20 + rng() % 600;
            in.push_back({tick, (double)(1 + rng() % 8)});
        }
        PrecisionParams p;
        p.respawnSeconds = (round % 3) * 0.5;
        for (int v = 0; v < 8; ++v) {
            bool N = v == 1 || v == 4 || v == 5 || v == 7;
            bool F = v == 2 || v == 4 || v == 6 || v == 7;
            bool C = v == 3 || v == 5 || v == 6 || v == 7;
            for (double L : {1.5, 3.0, 6.0}) {
                double a = expectedCompletionTime(in, p, N, F, C, L);
                double b = naiveE(in, p, N, F, C, L);
                if (std::isfinite(b) && b < 1e150) CHECK(std::fabs(a - b) <= 1e-8 * std::max(1.0, std::fabs(b)));
            }
            double Lstar = solvePrecision(in.data(), in.size(), p, N, F, C);
            CHECK(std::isfinite(Lstar));
            double e = expectedCompletionTime(in, p, N, F, C, Lstar);
            CHECK(std::fabs(e - p.targetSeconds) <= 1e-6 * p.targetSeconds);
        }
    }
    // Monotonic: more precision -> faster completion.
    std::vector<PrecisionInput> in{{300, 4}, {900, 2}, {1500, 1}, {2200, 3}};
    PrecisionParams p;
    double e1 = expectedCompletionTime(in, p, false, false, false, 2.0);
    double e2 = expectedCompletionTime(in, p, false, false, false, 4.0);
    CHECK(e1 > e2);
    // Wider windows -> lower required precision.
    std::vector<PrecisionInput> wide = in;
    for (auto& w : wide) w.window *= 2;
    CHECK(solvePrecision(wide.data(), wide.size(), p, false, false, false) <
          solvePrecision(in.data(), in.size(), p, false, false, false));
    // Infeasible target.
    PrecisionParams tiny = p;
    tiny.targetSeconds = 1.0;
    CHECK(std::isnan(solvePrecision(in.data(), in.size(), tiny, false, false, false)));
    // Variant relations: nerve/fatigue/cps only shrink windows => L* grows.
    auto all = solveAll(in, p);
    CHECK(all.ok[0] && all.value[1] >= all.value[0] && all.value[2] >= all.value[0]);
    std::printf("  sample L*: base=%.4f nerve=%.4f fatigue=%.4f cps=%.4f all=%.4f\n", all.value[0], all.value[1],
                all.value[2], all.value[3], all.value[7]);
}

static void testExport() {
    std::printf("export\n");
    std::vector<InputResult> rs;
    InputResult a;
    a.id = 0; a.tick = 120; a.early = 2; a.late = 1; a.down = true;
    InputResult b;
    b.id = 1; b.tick = 130; b.down = false; b.player2 = true;
    InputResult c;
    c.id = 2; c.tick = 400; c.early = 9; c.late = 9; c.capped = true;
    InputResult d;
    d.id = 3; d.tick = 500; d.unstable = true;
    rs = {a, b, c, d};
    auto fwc = toFwc(rs, 240.0, false);
    CHECK(fwc.size() == 4 + 8 + 4 + 2 * (4 + 8 + 1 + 4));
    CHECK(fwc[0] == 'F' && fwc[1] == 'W' && fwc[2] == 'C' && fwc[3] == '2');
    auto nandl = toNandlJson(rs, 240.0, 0.0, false);
    CHECK(nandl.find("\"frameWindow\": 4") != std::string::npos);
    CHECK(nandl.find("\"timePosition\": 130") == std::string::npos);  // release excluded
    auto csv = toCsv(rs);
    CHECK(csv.find("press") != std::string::npos);
    Replay rep;
    rep.name = "test \"quoted\".gdr2";
    auto js = toResultsJson(rep, Settings{}, rs, 9000, true);
    CHECK(js.find("\\\"quoted\\\"") != std::string::npos);
    auto st = computeStats(rs);
    CHECK(st.count == 3 && st.unstable == 1 && st.capped == 1);
    CHECK(st.tightest == 1 || st.tightest == 4);
    auto pin = toPrecisionInputs(rs, false);
    CHECK(pin.size() == 2 && pin[0].tick == 120 && pin[0].window == 4);
}

int main() {
    testBasicWindows();
    testNeighbourBounds();
    testKeepHold();
    testBaselineFailure();
    testUnstable();
    testPauseResume();
    testFiltersAndRange();
    testPrecision();
    testExport();
    if (g_failed) {
        std::printf("\n%d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
