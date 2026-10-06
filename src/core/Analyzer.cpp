#include "Analyzer.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace fwm {

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
    m_pos = 0;
    m_curInit = false;
    m_inFlight = false;
    m_resumePending = false;
    m_error.clear();
    m_verified = false;
    m_endTick = 0;
    m_totalTrials = 0;
    m_verifyOnly = verifyOnly;
    if (!m_replay || m_replay->inputs.empty()) {
        fail("The replay has no inputs.");
        return;
    }
    m_state = State::Verifying;
    launchBaseline();
}

void Analyzer::pause() {
    if (!active()) return;
    m_pausedFrom = m_state;
    m_runner.cancel();
    if (m_inFlight) {
        m_inFlight = false;
        m_resumePending = true;  // the interrupted trial restarts from scratch
    }
    m_state = State::Paused;
}

void Analyzer::resume() {
    if (m_state != State::Paused) return;
    m_state = m_pausedFrom;
    if (m_resumePending) {
        m_resumePending = false;
        if (m_state == State::Verifying) {
            launchBaseline();
        } else {
            m_repDone = 0;
            m_repPass = 0;
            launchCandidate(m_d);
        }
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
    TrialRequest r;
    r.plan = m_replay->inputs;
    r.baseline = true;
    r.requireCompletion = true;
    r.validateUntil = std::numeric_limits<Tick>::max();
    r.timeoutTick = m_replay->lastInputTick() + static_cast<Tick>(m_settings.timeoutSeconds) * static_cast<Tick>(m_replay->tps);
    m_runner.begin(std::move(r));
    m_inFlight = true;
}

TrialRequest Analyzer::makeCandidate(int index, int shift) const {
    auto const& inputs = m_replay->inputs;
    Bound const& b = m_bounds[index];
    TrialRequest r;
    r.plan = inputs;
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
    r.baseline = false;
    if (m_settings.fullCompletion) {
        r.requireCompletion = true;
        r.validateUntil = std::numeric_limits<Tick>::max();
        r.timeoutTick = m_endTick + 5 * tps;
    } else {
        r.requireCompletion = false;
        r.validateUntil = std::max(originalMax, candidateMax) + static_cast<Tick>(m_settings.horizonSeconds) * tps;
        r.timeoutTick = r.validateUntil + tps;
    }
    return r;
}

void Analyzer::launchCandidate(int shift) {
    m_d = shift;
    m_runner.begin(makeCandidate(m_selected[m_pos], shift));
    m_inFlight = true;
}

// ---------------------------------------------------------------------------
// Bounds and selection
// ---------------------------------------------------------------------------
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
        b.lo = prevSame[i] >= 0 ? in[prevSame[i]].tick + 1 : 0;
        int next = nextSame[i];
        if (in[i].down && m_settings.keepHoldLength && next >= 0 && !in[next].down) {
            // press with a paired release: both move together
            b.carry = next;
            int after = nextSame[next];
            Tick releaseLimit = after >= 0 ? in[after].tick - 1 : m_endTick - 1;
            b.hi = releaseLimit - (in[next].tick - in[i].tick);
        } else {
            b.hi = next >= 0 ? in[next].tick - 1 : m_endTick - 1;
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
// Search (linear outwards from the original timing; windows are small)
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
        if (m_pos >= static_cast<int>(m_selected.size())) {
            m_state = State::Completed;
            return;
        }
        if (!m_curInit) initCurrent();
        auto d = nextShift();
        if (!d) {
            m_results.push_back(m_cur);
            ++m_pos;
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

void Analyzer::onResult(TrialResult const& r) {
    if (m_state == State::Verifying) {
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
        m_pos = 0;
        m_curInit = false;
        m_state = State::Running;
        return;
    }

    if (m_state != State::Running) return;
    ++m_totalTrials;
    ++m_cur.trials;
    if (r.reason == Fail::ReplayError) {
        // Infrastructure problem: never report it as a narrow window.
        fail("Simulation error during analysis: " + (r.detail.empty() ? std::string("unknown") : r.detail));
        return;
    }
    ++m_repDone;
    if (r.pass) ++m_repPass;
    if (m_repDone < m_settings.repeats) {
        launchCandidate(m_d);  // same candidate again
        return;
    }
    if (m_repPass == m_settings.repeats) {
        ++m_k;
    } else {
        if (m_repPass != 0) m_cur.unstable = true;
        endPhase();
    }
    m_repDone = 0;
    m_repPass = 0;
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
        case State::Verifying: return "Verifying the replay...";
        case State::Failed: return "Failed";
        case State::Completed:
            std::snprintf(buf, sizeof(buf), "Done: %d inputs, %d trials", static_cast<int>(m_results.size()), m_totalTrials);
            return buf;
        default: break;
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
