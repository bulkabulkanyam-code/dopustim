#include "Analyzer.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace fwm {

namespace {
constexpr Tick kSelfTestSpan = 300;  // ticks compared with the original run after a test point
constexpr int kSelfTestPoints = 5;
}  // namespace

const char* toString(State s) {
    switch (s) {
        case State::Idle: return "Idle";
        case State::Verifying: return "Verifying";
        case State::Running: return "Analyzing";
        case State::Paused: return "Paused";
        case State::Completed: return "Completed";
        case State::Stopped: return "Stopped";
        case State::Failed: return "Failed";
    }
    return "?";
}

Analyzer::Analyzer(std::shared_ptr<Replay const> replay, Settings settings, ITrialRunner& runner)
    : m_replay(std::move(replay)), m_settings(settings), m_runner(runner) {
    if (m_settings.repeats < 1) m_settings.repeats = 1;
    if (m_settings.maxWindow < 1) m_settings.maxWindow = 1;
    if (m_settings.horizonSeconds < 1) m_settings.horizonSeconds = 1;
    if (m_settings.shortHoldTicks < 0) m_settings.shortHoldTicks = 0;
    if (m_settings.convergeTicks < 0) m_settings.convergeTicks = 0;
    if (m_settings.controlEvery < 0) m_settings.controlEvery = 0;
    if (m_settings.gapScan < 0) m_settings.gapScan = 0;
    if (m_settings.snapGap < 1) m_settings.snapGap = 1;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
void Analyzer::fail(std::string message) {
    m_runner.cancel();
    m_inFlight = false;
    m_resumePending = false;
    m_error = std::move(message);
    m_state = State::Failed;
}

void Analyzer::start(bool verifyOnly) {
    m_runner.cancel();
    m_results.clear();
    m_selected.clear();
    m_bounds.clear();
    m_selfTicks.clear();
    m_selfIdx = 0;
    m_selfSecond = false;
    m_selfRestored = 0;
    m_sinceControl = 0;
    m_controlIndex = 0;
    m_blockPos = 0;
    m_blockResults = 0;
    m_pos = 0;
    m_curInit = false;
    m_inFlight = false;
    m_resumePending = false;
    m_error.clear();
    m_snapNote.clear();
    m_holdPolicy = 0;
    m_policyOrder.clear();
    m_policyIdx = 0;
    m_selfDrop = false;
    m_policyFailures.clear();
    m_tail = 0;
    m_tailAt = 0;
    m_verified = false;
    m_endTick = 0;
    m_totalTrials = 0;
    m_verifyOnly = verifyOnly;
    if (!m_replay || m_replay->inputs.empty()) {
        fail("The replay has no inputs.");
        return;
    }
    m_state = State::Verifying;
    m_stage = Stage::Baseline;
    launchBaseline();
}

void Analyzer::pause() {
    if (!active()) return;
    m_pausedFrom = m_state;
    m_runner.cancel();
    if (m_inFlight) {
        m_inFlight = false;
        m_resumePending = true;  // the interrupted trial restarts
    }
    m_state = State::Paused;
}

void Analyzer::resume() {
    if (m_state != State::Paused) return;
    m_state = m_pausedFrom;
    if (m_resumePending) {
        m_resumePending = false;
        relaunchCurrent();
    }
}

void Analyzer::relaunchCurrent() {
    switch (m_stage) {
        case Stage::Baseline: launchBaseline(); break;
        case Stage::SelfTest: launchSelfTest(); break;
        case Stage::Control: launchControl(m_controlIndex); break;
        case Stage::Search:
            m_repDone = 0;
            m_repPass = 0;
            launchCandidate(m_d);
            break;
    }
}

void Analyzer::stop() {
    if (m_state == State::Idle || m_state == State::Completed || m_state == State::Failed) return;
    m_runner.cancel();
    m_inFlight = false;
    m_resumePending = false;
    m_state = State::Stopped;
}

// ---------------------------------------------------------------------------
// Trial construction
// ---------------------------------------------------------------------------
void Analyzer::launchBaseline() {
    m_stage = Stage::Baseline;
    TrialRequest r;
    r.plan = m_replay->inputs;
    r.baseline = true;
    r.requireCompletion = true;
    r.recordTrace = true;
    r.validateUntil = std::numeric_limits<Tick>::max();
    r.timeoutTick = m_replay->lastInputTick() + static_cast<Tick>(m_settings.timeoutSeconds) * static_cast<Tick>(m_replay->tps);
    m_runner.begin(std::move(r));
    m_inFlight = true;
}

// Earliest tick any candidate of this input can use; everything before it is identical to the original.
Tick Analyzer::earliestChange(int index) const {
    Bound const& b = m_bounds[index];
    Tick original = m_replay->inputs[index].tick;
    // The gap scan looks up to gapScan ticks beyond the largest counted shift.
    return std::max<Tick>(0, std::max<Tick>(b.lo, original - m_settings.maxWindow - m_settings.gapScan));
}

TrialRequest Analyzer::makeHorizonRequest(int index) const {
    auto const& inputs = m_replay->inputs;
    Bound const& b = m_bounds[index];
    TrialRequest r;
    r.plan = inputs;
    Tick tps = static_cast<Tick>(m_replay->tps);
    Tick originalMax = inputs[index].tick;
    if (b.carry >= 0) originalMax = std::max(originalMax, inputs[b.carry].tick);
    r.baseline = false;
    if (m_settings.fullCompletion) {
        r.requireCompletion = true;
        r.validateUntil = std::numeric_limits<Tick>::max();
        r.timeoutTick = m_endTick + 5 * tps;
    } else {
        r.requireCompletion = false;
        r.validateUntil = originalMax + static_cast<Tick>(m_settings.horizonSeconds) * tps;
        r.timeoutTick = r.validateUntil + tps;
    }
    r.useSnapshots = m_settings.snapshots && m_runner.snapshotsEnabled();
    r.resumeTick = earliestChange(index);
    r.holdRestore = m_holdPolicy;
    r.lightReset = m_holdPolicy != 0 && m_settings.lightReset;
    r.snapGap = m_holdPolicy != 0 ? m_settings.snapGap : 100;
    return r;
}

TrialRequest Analyzer::makeCandidate(int index, int shift) const {
    auto const& inputs = m_replay->inputs;
    Bound const& b = m_bounds[index];
    TrialRequest r = makeHorizonRequest(index);
    Tick originalMax = inputs[index].tick;
    r.plan[index].tick += shift;
    Tick candidateMax = r.plan[index].tick;
    if (b.carry >= 0) {
        originalMax = std::max(originalMax, inputs[b.carry].tick);
        r.plan[b.carry].tick += shift;
        candidateMax = std::max(candidateMax, r.plan[b.carry].tick);
    }
    std::stable_sort(r.plan.begin(), r.plan.end(), [](Input const& a, Input const& c) { return a.tick < c.tick; });

    Tick tps = static_cast<Tick>(m_replay->tps);
    Tick lastChange = std::max(originalMax, candidateMax);
    r.changedUntil = lastChange;
    r.convergeTicks = m_settings.convergeTicks;
    if (!m_settings.fullCompletion) {
        r.validateUntil = lastChange + static_cast<Tick>(m_settings.horizonSeconds) * tps;
        r.timeoutTick = r.validateUntil + tps;
    }
    return r;
}

void Analyzer::launchCandidate(int shift) {
    m_stage = Stage::Search;
    m_d = shift;
    m_runner.begin(makeCandidate(m_selected[m_pos], shift));
    m_inFlight = true;
}

void Analyzer::launchControl(int index) {
    m_stage = Stage::Control;
    m_controlIndex = index;
    TrialRequest r = makeHorizonRequest(index);
    r.control = true;
    r.compareTrace = true;
    m_runner.begin(std::move(r));
    m_inFlight = true;
}

// ---------------------------------------------------------------------------
// Bounds and selection
// ---------------------------------------------------------------------------
// A press moves together with its release when the hold mode says so.
bool Analyzer::wantsCarry(int i, int next) const {
    auto const& in = m_replay->inputs;
    if (!in[i].down || next < 0 || in[next].down) return false;
    switch (m_settings.holdMode) {
        case HoldMode::KeepHold: return true;
        case HoldMode::Auto: return in[next].tick - in[i].tick <= static_cast<Tick>(m_settings.shortHoldTicks);
        case HoldMode::PressOnly: return false;
    }
    return false;
}

// Moved inputs keep the recorded ORDER of inputs of the same player and button (the plan is
// sorted stably), so landing on the tick of a neighbour is allowed whenever the order stays the
// same: a press may share the tick of the release before it, a release may share the tick of the
// press after it. A press may never reach its own release (zero-length hold).
void Analyzer::computeBounds() {
    auto const& in = m_replay->inputs;
    int n = static_cast<int>(in.size());
    std::vector<int> prevSame(n, -1), nextSame(n, -1);
    int last[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
    for (int i = 0; i < n; ++i) {
        int p = in[i].player2 ? 1 : 0;
        int btn = std::min<int>(in[i].button, 3);
        prevSame[i] = last[p][btn];
        if (last[p][btn] >= 0) nextSame[last[p][btn]] = i;
        last[p][btn] = i;
    }
    m_bounds.assign(n, Bound{});
    for (int i = 0; i < n; ++i) {
        Bound b;
        int prev = prevSame[i];
        int next = nextSame[i];
        bool press = in[i].down;
        if (press) b.lo = prev >= 0 ? in[prev].tick : 0;
        else b.lo = prev >= 0 ? in[prev].tick + 1 : 0;

        if (wantsCarry(i, next)) {
            b.carry = next;
            int after = nextSame[next];
            Tick releaseLimit = after >= 0 ? in[after].tick : m_endTick - 1;  // release may share the next press' tick
            b.hi = releaseLimit - (in[next].tick - in[i].tick);
        } else if (press) {
            b.hi = next >= 0 ? in[next].tick - 1 : m_endTick - 1;
        } else {
            b.hi = next >= 0 ? in[next].tick : m_endTick - 1;
        }
        m_bounds[i] = b;
    }
}

void Analyzer::buildSelected() {
    m_selected.clear();
    auto const& in = m_replay->inputs;
    int n = static_cast<int>(in.size());
    int from = std::max(1, m_settings.fromInput);
    int to = m_settings.toInput > 0 ? std::min(m_settings.toInput, n) : n;
    for (int i = 0; i < n; ++i) {
        int number = i + 1;
        if (number < from || number > to) continue;
        if (in[i].down ? !m_settings.presses : !m_settings.releases) continue;
        if (in[i].player2 ? !m_settings.player2 : !m_settings.player1) continue;
        m_selected.push_back(i);
    }
}

// ---------------------------------------------------------------------------
// Search (linear outwards from the original timing; stops at the first failing shift per side)
// ---------------------------------------------------------------------------
void Analyzer::initCurrent() {
    auto const& in = m_replay->inputs[m_selected[m_pos]];
    m_cur = InputResult{};
    m_cur.id = in.id;
    m_cur.tick = in.tick;
    m_cur.button = in.button;
    m_cur.player2 = in.player2;
    m_cur.down = in.down;
    m_phase = Phase::Early;
    m_k = 0;
    m_tail = 0;
    m_tailAt = 0;
    m_curInit = true;
}

void Analyzer::endPhase() {
    if (m_phase == Phase::Early) {
        m_cur.early = m_k;
        m_phase = Phase::Late;
        m_k = 0;
    } else if (m_phase == Phase::Late) {
        m_cur.late = m_k;
        m_phase = Phase::Done;
        m_k = 0;
    }
}

std::optional<int> Analyzer::nextShift() {
    int index = m_selected[m_pos];
    Bound const& b = m_bounds[index];
    Tick original = m_replay->inputs[index].tick;
    while (m_phase != Phase::Done) {
        bool early = m_phase == Phase::Early;
        if (m_tail > 0) {
            // Gap scan: look past the failing shift for isolated passing ticks.
            int dt = (early ? -1 : 1) * m_tailAt;
            Tick tt = original + dt;
            if (tt < b.lo || tt > b.hi) {
                m_tail = 0;
                endPhase();
                continue;
            }
            return dt;
        }
        int other = early ? 0 : m_cur.early;
        // Known window lower bound is 1 + other + k. Once it exceeds the counted
        // maximum there is no need to search further.
        if (1 + other + m_k > m_settings.maxWindow) {
            m_cur.capped = true;
            endPhase();
            continue;
        }
        int d = (early ? -1 : 1) * (m_k + 1);
        Tick t = original + d;
        if (t < b.lo || t > b.hi) {
            (early ? m_cur.earlyBound : m_cur.lateBound) = true;
            endPhase();
            continue;
        }
        return d;
    }
    return std::nullopt;
}

void Analyzer::launchNext() {
    for (;;) {
        bool finished = m_pos >= static_cast<int>(m_selected.size());
        bool controls = m_settings.snapshots && m_settings.controlEvery > 0 && m_runner.snapshotsEnabled();
        if (!m_curInit && controls && m_sinceControl > 0 && (finished || m_sinceControl >= m_settings.controlEvery)) {
            // Control run: the unmodified replay must still pass from a saved state. The last block of
            // inputs is checked too.
            int index = finished ? m_selected.back() : m_selected[m_pos];
            launchControl(index);
            return;
        }
        if (finished) {
            m_state = State::Completed;
            return;
        }
        if (!m_curInit) initCurrent();
        auto d = nextShift();
        if (!d) {
            m_results.push_back(m_cur);
            ++m_pos;
            ++m_sinceControl;
            m_curInit = false;
            continue;
        }
        m_repDone = 0;
        m_repPass = 0;
        launchCandidate(*d);
        return;
    }
}

// ---------------------------------------------------------------------------
// Saved-state self-test
// ---------------------------------------------------------------------------
bool Analyzer::insideHold(Tick t) const {
    bool held[2][4] = {};
    int count = 0;
    for (auto const& in : m_replay->inputs) {
        if (in.tick >= t - 1) break;
        bool& h = held[in.player2 ? 1 : 0][std::clamp<int>(in.button, 0, 3)];
        if (in.down && !h) {
            h = true;
            ++count;
        } else if (!in.down && h) {
            h = false;
            --count;
        }
    }
    return count > 0;
}

void Analyzer::planSelfTest() {
    m_selfTicks.clear();
    m_selfIdx = 0;
    m_selfSecond = false;
    m_selfRestored = 0;
    if (m_selected.empty()) return;

    if (m_settings.anyTickStates) {
        // Ultra: the test points that matter are the ones inside a hold (that is what the restore policy is for).
        std::vector<Tick> holdPts, quietPts;
        for (int idx : m_selected) {
            Tick t = earliestChange(idx);
            if (t < 8 || t + kSelfTestSpan + 8 >= m_endTick) continue;
            (insideHold(t) ? holdPts : quietPts).push_back(t);
        }
        auto pick = [&](std::vector<Tick> const& v, int n) {
            if (v.empty()) return;
            for (int j = 1; j <= n; ++j) m_selfTicks.push_back(v[std::min(v.size() - 1, v.size() * static_cast<std::size_t>(j) / (n + 1))]);
        };
        pick(holdPts, 3);
        pick(quietPts, 2);
        std::sort(m_selfTicks.begin(), m_selfTicks.end());
        m_selfTicks.erase(std::unique(m_selfTicks.begin(), m_selfTicks.end()), m_selfTicks.end());
        return;
    }

    std::size_t count = m_selected.size();
    for (int j = 1; j <= kSelfTestPoints; ++j) {
        std::size_t idx = std::min(count - 1, count * static_cast<std::size_t>(j) / (kSelfTestPoints + 1));
        Tick t = earliestChange(m_selected[idx]);
        if (t < 8) continue;
        if (t + kSelfTestSpan + 8 >= m_endTick) continue;
        if (!m_selfTicks.empty() && t <= m_selfTicks.back()) continue;
        m_selfTicks.push_back(t);
    }
}

std::string Analyzer::policyText() const {
    if (!m_settings.anyTickStates) return {};
    switch (m_holdPolicy) {
        case 1: return "Ultra: checkpoints keep held buttons (Practice Fix or similar). ";
        case 2: return "Ultra: held buttons are pressed again after loading a checkpoint. ";
        default: return "Ultra: held buttons cannot be restored exactly here, using exact saved states. ";
    }
}

void Analyzer::launchSelfTest() {
    m_stage = Stage::SelfTest;
    Tick t = m_selfTicks[m_selfIdx];
    Tick tps = static_cast<Tick>(m_replay->tps);
    TrialRequest r;
    r.plan = m_replay->inputs;
    r.baseline = false;
    r.resumeTick = t;
    r.useSnapshots = true;
    r.compareTrace = true;
    r.validateUntil = t + kSelfTestSpan;
    r.timeoutTick = r.validateUntil + tps;
    r.holdRestore = m_holdPolicy;
    r.lightReset = m_holdPolicy != 0 && m_settings.lightReset;
    r.snapGap = m_holdPolicy != 0 ? m_settings.snapGap : 100;
    r.dropSnapshot = m_selfDrop;
    m_selfDrop = false;
    m_runner.begin(std::move(r));
    m_inFlight = true;
}

void Analyzer::startSearch() {
    if (m_settings.anyTickStates && m_holdPolicy == 0) {
        // Ultra could not restore held buttons exactly (or has no saved states at all): behave like Fast.
        m_settings.anyTickStates = false;
        m_settings.lightReset = false;
        m_settings.controlEvery = m_settings.controlEveryExact;
    }
    m_stage = Stage::Search;
    m_state = State::Running;
    m_sinceControl = 0;
    m_blockPos = 0;
    m_blockResults = 0;
    m_pos = 0;
    m_curInit = false;
}

// ---------------------------------------------------------------------------
// Results handling
// ---------------------------------------------------------------------------
static std::string baselineAdvice(TrialResult const& r) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), " (tick %lld, %.1f%%)", static_cast<long long>(r.tick), r.percent);
    std::string s = "The unmodified replay did not complete the level: ";
    s += toString(r.reason);
    s += buf;
    if (!r.detail.empty()) s += " - " + r.detail;
    s += ". Check: same level and version, started from 0% in normal mode, no noclip/other bots/TPS mods, "
         "and try the other \"Input phase\" setting.";
    return s;
}

void Analyzer::onBaseline(TrialResult const& r) {
    if (!r.pass) {
        fail(baselineAdvice(r));
        return;
    }
    m_endTick = r.tick;
    m_verified = true;
    if (m_verifyOnly) {
        m_state = State::Completed;
        return;
    }
    computeBounds();
    buildSelected();
    if (m_selected.empty()) {
        fail("No inputs match the current filters (presses/releases, players, input range).");
        return;
    }
    m_policyOrder.clear();
    m_policyIdx = 0;
    if (m_settings.anyTickStates && m_settings.snapshots && m_runner.snapshotsEnabled()) m_policyOrder = {1, 2, 0};
    else m_policyOrder = {0};
    m_holdPolicy = m_policyOrder[0];
    if (m_settings.snapshots && m_runner.snapshotsEnabled()) {
        planSelfTest();
        if (!m_selfTicks.empty()) {
            launchSelfTest();
            return;
        }
        m_holdPolicy = 0;  // nothing was verified: stay exact
        m_snapNote = "Saved states: level too short to self-test, using them with periodic checks";
    }
    startSearch();
}

void Analyzer::onSelfTest(TrialResult const& r) {
    ++m_totalTrials;
    if (!r.pass) {
        std::string why = r.detail.empty() ? std::string("restored state differs from the original run") : r.detail;
        if (r.snapshotUsed) {
            if (m_policyIdx + 1 < m_policyOrder.size()) {
                // Ultra: this way of restoring held buttons is not exact here, try the next one.
                m_policyFailures += (m_policyFailures.empty() ? "" : "; ") + std::string(m_holdPolicy == 1 ? "checkpoint-only" : "press-again") + ": " + why;
                ++m_policyIdx;
                m_holdPolicy = m_policyOrder[m_policyIdx];
                m_selfIdx = 0;
                m_selfSecond = false;
                m_selfRestored = 0;
                m_selfDrop = true;
                launchSelfTest();
                return;
            }
            m_runner.disableSnapshots(why);
            m_snapNote = "Saved states are not exact in this level (" + why + "), using full replays";
            m_holdPolicy = 0;
            startSearch();
            return;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), " (around tick %lld)", static_cast<long long>(m_selfTicks[m_selfIdx]));
        fail("The game does not play the same replay the same way twice" + std::string(buf) + ": " + why +
             ". Disable TPS / physics / random mods and try again.");
        return;
    }
    if (r.snapshotUsed && m_selfSecond) ++m_selfRestored;
    if (!m_runner.snapshotsEnabled()) {
        m_snapNote = "Saved states off: " + m_runner.snapshotNote() + ", using full replays";
        m_holdPolicy = 0;
        startSearch();
        return;
    }
    if (!m_selfSecond) {
        m_selfSecond = true;
        launchSelfTest();
        return;
    }
    m_selfSecond = false;
    ++m_selfIdx;
    if (m_selfIdx < m_selfTicks.size()) {
        launchSelfTest();
        return;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Saved states verified (%d of %d checks restored a state)", m_selfRestored,
                  static_cast<int>(m_selfTicks.size()));
    m_snapNote = policyText() + buf;
    if (m_holdPolicy == 0 && !m_policyFailures.empty()) m_snapNote += " [" + m_policyFailures + "]";
    startSearch();
}

void Analyzer::onControl(TrialResult const& r) {
    ++m_totalTrials;
    m_stage = Stage::Search;
    if (r.pass) {
        m_sinceControl = 0;
        m_blockPos = m_pos;
        m_blockResults = m_results.size();
        return;
    }
    std::string why = r.detail.empty() ? std::string("control run diverged") : r.detail;
    if (r.snapshotUsed && m_runner.snapshotsEnabled()) {
        // A saved state is not exact: throw away what was measured since the last good control and redo it
        // from 0%.
        m_runner.disableSnapshots(why);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Saved states switched off at input #%d, last %d inputs measured again; reason: ",
                      m_replay->inputs[m_controlIndex].id + 1, m_pos - m_blockPos);
        m_snapNote = std::string(buf) + why;
        m_results.resize(m_blockResults);
        m_pos = m_blockPos;
        m_curInit = false;
        m_sinceControl = 0;
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (input #%d)", m_replay->inputs[m_controlIndex].id + 1);
    fail("A control run of the unmodified replay failed" + std::string(buf) + ": " + why +
         ". The game state is not reset the same way every attempt; results would be unreliable.");
}

void Analyzer::onCandidate(TrialResult const& r) {
    ++m_totalTrials;
    ++m_cur.trials;
    if (r.reason == Fail::ReplayError) {
        // Infrastructure problem: never report it as a narrow window.
        fail("Simulation error during analysis: " + (r.detail.empty() ? std::string("unknown") : r.detail));
        return;
    }
    ++m_repDone;
    if (r.pass) ++m_repPass;
    bool tail = m_tail > 0;
    if (!tail && !r.pass && r.reason == Fail::Death) {
        Tick candidate = m_replay->inputs[m_selected[m_pos]].tick + m_d;
        Tick delta = std::clamp<Tick>(r.tick - candidate, 0, 1000000000);
        (m_d < 0 ? m_cur.earlyDeath : m_cur.lateDeath) = static_cast<int>(delta);
    }
    if (m_repDone < m_settings.repeats) {
        launchCandidate(m_d);  // same candidate again
        return;
    }
    bool allPass = m_repPass == m_settings.repeats;
    if (tail) {
        if (allPass) ++m_cur.holes;
        --m_tail;
        ++m_tailAt;
        if (m_tail == 0) endPhase();
    } else if (allPass) {
        ++m_k;
    } else {
        if (m_repPass != 0) m_cur.unstable = true;
        if (m_settings.gapScan > 0) {
            m_tail = m_settings.gapScan;
            m_tailAt = m_k + 2;  // the shift right behind the failing one
        } else {
            endPhase();
        }
    }
    m_repDone = 0;
    m_repPass = 0;
}

void Analyzer::onResult(TrialResult const& r) {
    if (m_state == State::Verifying) {
        if (m_stage == Stage::Baseline) onBaseline(r);
        else onSelfTest(r);
        return;
    }
    if (m_state != State::Running) return;
    if (m_stage == Stage::Control) onControl(r);
    else onCandidate(r);
}

void Analyzer::advance() {
    if (!active()) return;
    if (m_inFlight) {
        auto r = m_runner.poll();
        if (!r) return;
        m_inFlight = false;
        onResult(*r);
        if (!active()) return;
    }
    if (m_state == State::Running && !m_inFlight) launchNext();
}

std::optional<Analyzer::Current> Analyzer::current() const {
    if (m_state != State::Running && m_state != State::Paused) return std::nullopt;
    if (!m_curInit || m_pos >= static_cast<int>(m_selected.size())) return std::nullopt;
    auto const& in = m_replay->inputs[m_selected[m_pos]];
    Current c;
    c.number = m_selected[m_pos] + 1;
    c.tick = in.tick;
    c.down = in.down;
    c.player2 = in.player2;
    c.testing = m_inFlight ? m_d : 0;
    c.early = m_phase == Phase::Early ? m_k : m_cur.early;
    c.late = m_phase == Phase::Late ? m_k : 0;
    return c;
}

std::string Analyzer::statusLine() const {
    char buf[256];
    switch (m_state) {
        case State::Idle: return "Idle";
        case State::Verifying:
            if (m_stage == Stage::SelfTest) {
                std::snprintf(buf, sizeof(buf), "Checking saved states %d/%d...",
                              static_cast<int>(m_selfIdx) + 1, static_cast<int>(m_selfTicks.size()));
                return buf;
            }
            return "Verifying the replay...";
        case State::Failed: return "Failed";
        case State::Completed:
            std::snprintf(buf, sizeof(buf), "Done: %d inputs, %d trials", static_cast<int>(m_results.size()), m_totalTrials);
            return buf;
        default: break;
    }
    if (m_stage == Stage::Control && m_inFlight) {
        std::snprintf(buf, sizeof(buf), "%s %d/%d | control run", toString(m_state), m_pos, selectedCount());
        return buf;
    }
    auto c = current();
    if (c) {
        std::snprintf(buf, sizeof(buf), "%s %d/%d | #%d %s P%d t=%lld | testing %+d | -%d/+%d",
                      toString(m_state), m_pos + 1, selectedCount(), c->number, c->down ? "press" : "release",
                      c->player2 ? 2 : 1, static_cast<long long>(c->tick), c->testing, c->early, c->late);
    } else {
        std::snprintf(buf, sizeof(buf), "%s %d/%d", toString(m_state), m_pos, selectedCount());
    }
    return buf;
}

}  // namespace fwm
