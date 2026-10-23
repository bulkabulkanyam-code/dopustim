// Standalone tests for the Geode-independent core.
//   g++ -std=c++20 -O2 -Isrc tests/test_core.cpp src/core/*.cpp -o test_core && ./test_core
#include <algorithm>
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

    // saved-state emulation
    bool snapshots = false;         // the runner offers saved states
    bool snapsBroken = false;       // restored states are wrong from the start
    int snapsBrokenAfter = -1;      // restored states become wrong after this many trials (-1 = never)

    std::map<int, std::vector<Tick>> extraAllowed;  // id -> isolated passing ticks outside the allowed range (holes)
    bool holdWorks[3] = {true, true, true};         // does restore policy 1 / 2 work for a state taken inside a hold?
};

struct FakeRunner : ITrialRunner {
    FakeLevel& level;
    TrialRequest req;
    std::optional<TrialResult> pending;
    bool ready = false;
    int polls = 0;
    int trials = 0;
    // statistics
    bool snapOn = false;
    std::string note;
    bool haveSnap = false;
    Tick snapTick = -1;
    int snapUsed = 0, controlTrials = 0, selfTrials = 0, resumeViolations = 0, disabledCalls = 0;
    std::vector<int> searchPolicies;  // holdRestore of every search trial
    explicit FakeRunner(FakeLevel& l) : level(l), snapOn(l.snapshots) {}

    bool snapshotsEnabled() const override { return snapOn; }
    void disableSnapshots(std::string const& why) override {
        snapOn = false;
        haveSnap = false;
        note = why;
        ++disabledCalls;
    }
    std::string snapshotNote() const override { return note; }

    void begin(TrialRequest r) override {
        req = std::move(r);
        ++trials;
        if (req.control) ++controlTrials;
        if (req.compareTrace && !req.control) ++selfTrials;

        // Invariant of the speed-up: before resumeTick the plan equals the original replay.
        if (req.resumeTick >= 0) {
            for (auto const& in : req.plan) {
                if (level.original[in.id] < req.resumeTick || in.tick < req.resumeTick) {
                    if (in.tick != level.original[in.id]) ++resumeViolations;
                }
            }
        }

        if (!req.baseline && !req.control && !req.compareTrace) searchPolicies.push_back(req.holdRestore);
        if (req.dropSnapshot) haveSnap = false;
        Tick margin = req.holdRestore != 0 ? 1 : 2;  // any-tick states are taken one tick before the first change
        bool wantSnap = snapOn && req.useSnapshots && req.resumeTick >= 2;
        bool usedSnap = wantSnap && haveSnap && snapTick <= req.resumeTick - margin;
        Tick usedAt = snapTick;
        if (wantSnap) {
            haveSnap = true;
            snapTick = req.resumeTick - margin;
        }
        if (usedSnap) ++snapUsed;
        bool corrupt = usedSnap && (level.snapsBroken || (level.snapsBrokenAfter >= 0 && trials > level.snapsBrokenAfter));
        if (usedSnap && req.holdRestore != 0 && !level.holdWorks[req.holdRestore]) {
            int held = 0;  // single button: presses minus releases before the saved state
            for (auto const& in : req.plan)
                if (in.tick < usedAt) held += in.down ? 1 : -1;
            if (held > 0) corrupt = true;
        }

        TrialResult res;
        res.snapshotUsed = usedSnap;
        Tick failAt = -1;
        for (auto const& in : req.plan) {
            auto it = level.allowed.find(in.id);
            if (it == level.allowed.end()) continue;
            bool extra = false;
            auto ex = level.extraAllowed.find(in.id);
            if (ex != level.extraAllowed.end())
                extra = std::find(ex->second.begin(), ex->second.end(), in.tick) != ex->second.end();
            if (!extra && (in.tick < it->second.first || in.tick > it->second.second)) {
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
        if (corrupt) {
            // A wrong saved state: everything started from it looks like a death / a mismatch.
            res.pass = false;
            res.reason = (req.compareTrace || req.control) ? Fail::ReplayError : Fail::Death;
            res.tick = req.resumeTick + 5;
            res.detail = "fake state mismatch";
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
    lv.allowed[2] = {100, 120};  // lower side blocked by the release@102 (may equal it, not pass it)
    FakeRunner runner(lv);
    Settings s;
    s.holdMode = HoldMode::PressOnly;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Completed);
    auto const& r0 = a.results()[0];
    CHECK(r0.late == 1 && r0.lateBound);   // can reach tick 101 only (never its own release)
    CHECK(r0.early == 3 && !r0.earlyBound);
    auto const& r1 = a.results()[1];
    // press@104 may share the tick of the release before it (@102): order stays release -> press
    CHECK(r1.early == 2 && r1.earlyBound);
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
    s.holdMode = HoldMode::KeepHold;
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
// Hold handling: taps (spam) move together with their release in Auto mode.
// ---------------------------------------------------------------------------
static void testHoldAuto() {
    std::printf("hold mode auto (taps)\n");
    // 1-tick taps every 10 ticks, then one long hold.
    auto replay = makeReplay({{100, true}, {101, false}, {110, true}, {111, false}, {300, true}, {340, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {98, 102};
    lv.allowed[1] = {99, 103};    // release of the tap: moves with the press
    lv.allowed[4] = {298, 302};   // long hold: only the press moves
    {
        FakeRunner runner(lv);
        Settings s;  // Auto
        Analyzer a(replay, s, runner);
        a.start(false);
        runToEnd(a);
        CHECK(a.state() == State::Completed);
        auto const& tap = a.results()[0];
        CHECK(tap.early == 2 && tap.late == 2 && tap.window() == 5);
        // the long hold is NOT dragged along: its release never leaves the recorded tick
        auto const& hold = a.results()[2];
        CHECK(hold.early == 2 && hold.late == 2);
    }
    {
        FakeRunner runner(lv);
        Settings s;
        s.holdMode = HoldMode::PressOnly;
        Analyzer a(replay, s, runner);
        a.start(false);
        runToEnd(a);
        auto const& tap = a.results()[0];
        CHECK(tap.late == 0 && tap.lateBound);  // press cannot reach its own release@101
    }
}

static void testDeathDiagnostics() {
    std::printf("death diagnostics\n");
    auto replay = makeReplay({{100, true}, {110, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {97, 101};
    FakeRunner runner(lv);
    Analyzer a(replay, Settings{}, runner);
    a.start(false);
    runToEnd(a);
    auto const& r = a.results()[0];
    CHECK(r.early == 3 && r.late == 1);
    CHECK(r.earlyDeath == 3 + 4);  // died deathDelay ticks after the original tick; candidate was 4 earlier
    CHECK(r.lateDeath == 1);
}

// A long replay: 60 taps, each with a window of 5 ticks.
static std::shared_ptr<Replay> longReplay(FakeLevel& lv) {
    std::vector<std::tuple<Tick, bool>> list;
    for (int k = 0; k < 60; ++k) {
        list.push_back({500 + 100 * k, true});
        list.push_back({510 + 100 * k, false});
    }
    auto replay = makeReplay(list);
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    for (auto const& in : replay->inputs)
        if (in.down) lv.allowed[in.id] = {in.tick - 2, in.tick + 2};
    return replay;
}

static void checkAllFive(Analyzer const& a) {
    CHECK(a.state() == State::Completed);
    CHECK(a.results().size() == 60);
    for (auto const& r : a.results()) CHECK(r.early == 2 && r.late == 2 && r.window() == 5);
}

static void testSavedStatesHealthy() {
    std::printf("saved states (healthy)\n");
    FakeLevel lv;
    lv.snapshots = true;
    auto replay = longReplay(lv);
    FakeRunner runner(lv);
    Settings s;
    s.controlEvery = 10;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    checkAllFive(a);
    CHECK(runner.resumeViolations == 0);
    CHECK(runner.snapUsed > 100);
    CHECK(runner.controlTrials >= 6);  // 60 inputs / 10, plus the final block check
    CHECK(runner.selfTrials >= 6);
    CHECK(runner.disabledCalls == 0);
    CHECK(a.snapshotNote().find("verified") != std::string::npos);
}

static void testSavedStatesBrokenFromStart() {
    std::printf("saved states (broken, caught by the self-test)\n");
    FakeLevel lv;
    lv.snapshots = true;
    lv.snapsBroken = true;
    auto replay = longReplay(lv);
    FakeRunner runner(lv);
    Analyzer a(replay, Settings{}, runner);
    a.start(false);
    runToEnd(a);
    checkAllFive(a);  // results are still right: the analysis fell back to full replays
    CHECK(runner.disabledCalls == 1);
    CHECK(a.snapshotNote().find("not exact") != std::string::npos);
    CHECK(runner.resumeViolations == 0);
}

static void testSavedStatesBreakLater() {
    std::printf("saved states (break later, caught by a control run)\n");
    FakeLevel lv;
    lv.snapshots = true;
    lv.snapsBrokenAfter = 80;  // passes the self-test, then goes wrong
    auto replay = longReplay(lv);
    FakeRunner runner(lv);
    Settings s;
    s.controlEvery = 5;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    checkAllFive(a);  // wrong blocks were measured again
    CHECK(runner.disabledCalls == 1);
    CHECK(a.snapshotNote().find("switched off") != std::string::npos);
}

static void testControlWithoutSnapshotsFails() {
    std::printf("control run failing without saved states = real problem\n");
    FakeLevel lv;
    lv.snapshots = true;
    lv.snapsBrokenAfter = 80;
    auto replay = longReplay(lv);
    FakeRunner runner(lv);
    // A runner whose saved states never report themselves as used: the analyzer cannot blame them.
    struct Liar : FakeRunner {
        using FakeRunner::FakeRunner;
        void begin(TrialRequest r) override {
            FakeRunner::begin(std::move(r));
            if (pending && pending->snapshotUsed && !pending->pass) pending->snapshotUsed = false;
        }
    } liar(lv);
    Settings s;
    s.controlEvery = 5;
    Analyzer a(replay, s, liar);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Failed);
    CHECK(!a.error().empty());
}


// ---------------------------------------------------------------------------
// S&Q modes
// ---------------------------------------------------------------------------
static void testSpeedModeMapping() {
    std::printf("S&Q mode mapping\n");
    Settings base;
    base.horizonSeconds = 5;
    base.controlEvery = 25;
    auto fast = applySpeedMode(base);
    CHECK(fast.snapshots && fast.convergeTicks == 120 && fast.horizonSeconds == 5 && !fast.anyTickStates && fast.gapScan == 0);

    base.mode = SpeedMode::Accurate;
    auto acc = applySpeedMode(base);
    CHECK(!acc.snapshots && acc.convergeTicks == 0 && acc.horizonSeconds == 15 && acc.gapScan == 3 && acc.controlEvery == 0);
    CHECK(applySpeedMode(acc).horizonSeconds == 15);  // idempotent

    base.mode = SpeedMode::Ultra;
    auto ultra = applySpeedMode(base);
    CHECK(ultra.snapshots && ultra.anyTickStates && ultra.lightReset && ultra.horizonSeconds == 2);
    CHECK(ultra.convergeTicks == 40 && ultra.controlEvery == 0 && ultra.controlEveryExact == 25);
    CHECK(applySpeedMode(ultra).controlEveryExact == 25);  // second call must not lose the saved value
}

static void testGapScan() {
    std::printf("gap scan (Accurate)\n");
    auto replay = makeReplay({{100, true}, {110, false}, {300, true}, {310, false}});
    FakeLevel lv;
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    lv.allowed[0] = {98, 102};
    lv.extraAllowed[0] = {94};  // an isolated good tick behind a failing one
    lv.allowed[2] = {300, 300};
    FakeRunner runner(lv);
    Settings s;
    s.gapScan = 3;
    Analyzer a(replay, s, runner);
    a.start(false);
    runToEnd(a);
    CHECK(a.state() == State::Completed && a.results().size() == 2);
    auto const& r0 = a.results()[0];
    CHECK(r0.early == 2 && r0.late == 2 && r0.window() == 5);
    CHECK(r0.holes == 1);
    CHECK(r0.earlyDeath == 3 + 3);  // death diagnostics still describe the first failing shift
    CHECK(a.results()[1].holes == 0 && a.results()[1].window() == 1);

    // Without the scan the hole is not seen and fewer trials are needed.
    FakeRunner runner2(lv);
    Analyzer b(replay, Settings{}, runner2);
    b.start(false);
    runToEnd(b);
    CHECK(b.results()[0].holes == 0 && b.results()[0].window() == 5);
    CHECK(runner2.trials < runner.trials);

    // With saved states the plan must still equal the original before resumeTick (the scan reaches further back).
    FakeLevel lv2;
    lv2.snapshots = true;
    auto replay2 = longReplay(lv2);
    FakeRunner runner3(lv2);
    Settings s3;
    s3.gapScan = 3;
    Analyzer c(replay2, s3, runner3);
    c.start(false);
    runToEnd(c);
    checkAllFive(c);
    CHECK(runner3.resumeViolations == 0);
}

// 20 long holds (250 ticks); with releases measured, many saved states fall inside a hold.
static std::shared_ptr<Replay> holdReplay(FakeLevel& lv) {
    std::vector<std::tuple<Tick, bool>> list;
    for (int k = 0; k < 20; ++k) {
        list.push_back({500 + 400 * k, true});
        list.push_back({750 + 400 * k, false});
    }
    auto replay = makeReplay(list);
    for (auto const& in : replay->inputs) lv.original.push_back(in.tick);
    for (auto const& in : replay->inputs) lv.allowed[in.id] = {in.tick - 2, in.tick + 2};
    return replay;
}

static Settings ultraSettings() {
    Settings s;
    s.mode = SpeedMode::Ultra;
    s.releases = true;
    return applySpeedMode(s);
}

static void checkAllWindows(Analyzer const& a) {
    CHECK(a.state() == State::Completed);
    CHECK(a.results().size() == 40);
    for (auto const& r : a.results()) CHECK(r.early == 2 && r.late == 2 && r.window() == 5);
}

static void testUltraPolicies() {
    std::printf("Ultra: held-button restore policy\n");
    {  // everything works: the checkpoint restores holds by itself (policy 1), no control runs
        FakeLevel lv;
        lv.snapshots = true;
        auto replay = holdReplay(lv);
        FakeRunner runner(lv);
        Analyzer a(replay, ultraSettings(), runner);
        a.start(false);
        runToEnd(a);
        checkAllWindows(a);
        CHECK(!runner.searchPolicies.empty());
        for (int p : runner.searchPolicies) CHECK(p == 1);
        CHECK(runner.controlTrials == 0);
        CHECK(runner.snapUsed > 100);
        CHECK(runner.resumeViolations == 0);
        CHECK(a.snapshotNote().find("keep held buttons") != std::string::npos);
    }
    {  // checkpoints lose the hold, pressing again works (policy 2)
        FakeLevel lv;
        lv.snapshots = true;
        lv.holdWorks[1] = false;
        auto replay = holdReplay(lv);
        FakeRunner runner(lv);
        Analyzer a(replay, ultraSettings(), runner);
        a.start(false);
        runToEnd(a);
        checkAllWindows(a);
        for (int p : runner.searchPolicies) CHECK(p == 2);
        CHECK(runner.disabledCalls == 0);
        CHECK(a.snapshotNote().find("pressed again") != std::string::npos);
    }
    {  // neither works: fall back to exact saved states, control runs come back
        FakeLevel lv;
        lv.snapshots = true;
        lv.holdWorks[1] = false;
        lv.holdWorks[2] = false;
        auto replay = holdReplay(lv);
        FakeRunner runner(lv);
        Analyzer a(replay, ultraSettings(), runner);
        a.start(false);
        runToEnd(a);
        checkAllWindows(a);
        for (int p : runner.searchPolicies) CHECK(p == 0);
        CHECK(runner.disabledCalls == 0);
        CHECK(runner.controlTrials >= 2);
        CHECK(runner.resumeViolations == 0);
        CHECK(a.snapshotNote().find("exact") != std::string::npos);
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
    // Warm start (live display) gives the same answer.
    {
        double exact = solvePrecision(in.data(), in.size(), p, true, true, true);
        double warm = solvePrecision(in.data(), in.size(), p, true, true, true, exact * 1.7, 1e-9);
        double cold = solvePrecision(in.data(), in.size(), p, true, true, true, 0.0, 1e-9);
        CHECK(std::fabs(warm - exact) <= 1e-6 * exact);
        CHECK(std::fabs(cold - exact) <= 1e-6 * exact);
        double low = solvePrecision(in.data(), in.size(), p, true, true, true, exact * 0.001, 1e-9);
        CHECK(std::fabs(low - exact) <= 1e-6 * exact);
    }
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
    testHoldAuto();
    testDeathDiagnostics();
    testSavedStatesHealthy();
    testSavedStatesBrokenFromStart();
    testSavedStatesBreakLater();
    testControlWithoutSnapshotsFails();
    testSpeedModeMapping();
    testGapScan();
    testUltraPolicies();
    testPrecision();
    testExport();
    if (g_failed) {
        std::printf("\n%d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
