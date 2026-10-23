#include "GameRunner.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace fwm {

namespace {
// GD 2.2081: GJGameState::m_currentProgress counts half ticks; one full physics
// step (1/240 s) advances it by two.
Tick physicsTick(PlayLayer* layer) {
    return static_cast<Tick>(layer->m_gameState.m_currentProgress / 2);
}

std::uint32_t flagsOf(PlayerObject* p) {
    std::uint32_t f = 0;
    auto bit = [&](bool v, int i) {
        if (v) f |= (1u << i);
    };
    bit(p->m_isUpsideDown, 0);
    bit(p->m_isOnGround, 1);
    bit(p->m_isShip, 2);
    bit(p->m_isBall, 3);
    bit(p->m_isBird, 4);
    bit(p->m_isDart, 5);
    bit(p->m_isRobot, 6);
    bit(p->m_isSpider, 7);
    bit(p->m_isSwing, 8);
    return f;
}

constexpr double kEps = 0.002;         // allowed difference when two runs are compared
}  // namespace

bool sigEqual(Sig const& a, Sig const& b, double eps, double* worst) {
    double w = 0;
    auto upd = [&](double d) { w = std::max(w, std::fabs(d)); };
    upd(a.x - b.x);
    upd(a.y - b.y);
    upd(a.yv - b.yv);
    bool same = a.flags == b.flags && a.dual == b.dual;
    if (a.dual && b.dual) {
        upd(a.x2 - b.x2);
        upd(a.y2 - b.y2);
        upd(a.yv2 - b.yv2);
        same = same && a.flags2 == b.flags2;
    }
    if (worst) *worst = same ? w : std::max(w, 1.0);
    return same && w <= eps;
}

GameRunner::GameRunner(PlayLayer* layer, bool inputsAfterCommands, bool allowSnapshots)
    : m_layer(layer), m_after(inputsAfterCommands), m_originalTestMode(layer->m_isTestMode), m_snapAllowed(allowSnapshots) {
    // Test mode: the game does not save progress / stats for these attempts.
    m_layer->m_isTestMode = true;
}

GameRunner::~GameRunner() {
    restoreLayer();
}

void GameRunner::restoreLayer() {
    m_snap = Snapshot{};
    m_trace.clear();
    m_trace.shrink_to_fit();
    if (!m_layer) return;
    m_injecting = true;
    for (int button = 1; button <= 3; ++button) {
        m_layer->handleButton(false, button, true);
        m_layer->handleButton(false, button, false);
    }
    m_injecting = false;
    m_layer->m_queuedButtons.clear();
    m_layer->m_isTestMode = m_originalTestMode;
    m_layer = nullptr;
    m_state = St::Idle;
}

Tick GameRunner::ticks() const {
    return m_layer ? physicsTick(m_layer) : 0;
}

void GameRunner::disableSnapshots(std::string const& why) {
    m_snapOk = false;
    m_snap = Snapshot{};
    m_snapTarget = -1;
    m_snapNote = why;
}

// ---------------------------------------------------------------------------
// ITrialRunner
// ---------------------------------------------------------------------------
void GameRunner::begin(TrialRequest request) {
    m_req = std::move(request);
    m_result.reset();
    m_cursor = 0;
    m_lastLimit = -1;
    m_lastSeen = -1;
    m_idleUpdates = 0;
    m_conv = 0;
    m_usedSnapshot = false;
    m_snapTarget = -1;
    if (m_req.dropSnapshot) m_snap = Snapshot{};
    if (m_req.recordTrace) m_trace.clear();
    m_state = m_layer ? St::NeedsReset : St::Idle;
    if (!m_layer) finish(false, Fail::ReplayError, false, "the level is no longer open");
}

std::optional<TrialResult> GameRunner::poll() {
    auto r = std::move(m_result);
    m_result.reset();
    return r;
}

void GameRunner::cancel() {
    m_state = St::Idle;
    m_result.reset();
}

// ---------------------------------------------------------------------------
// Capturing / comparing the state of a run
// ---------------------------------------------------------------------------
Sig GameRunner::capture() const {
    Sig s;
    if (!m_layer || !m_layer->m_player1) return s;
    auto p1 = m_layer->m_player1;
    s.x = p1->getPositionX();
    s.y = p1->getPositionY();
    s.yv = p1->m_yVelocity;
    s.flags = flagsOf(p1);
    if (m_layer->m_gameState.m_isDualMode && m_layer->m_player2) {
        auto p2 = m_layer->m_player2;
        s.dual = true;
        s.x2 = p2->getPositionX();
        s.y2 = p2->getPositionY();
        s.yv2 = p2->m_yVelocity;
        s.flags2 = flagsOf(p2);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Saved states
// ---------------------------------------------------------------------------
// Latest "quiet" tick <= resumeTick - 2: all buttons are up and there is no input within one tick on
// either side. At such a moment the game state does not depend on button handling details that a
// checkpoint may not store (held buttons, queued presses). Returns -1 when there is none.
Tick GameRunner::pickSnapshotTick() const {
    if (m_req.holdRestore != 0) {
        // Ultra: any tick will do, the held buttons are restored explicitly. The latest one is
        // resumeTick - 1 (the saved state then contains only inputs before resumeTick).
        Tick t = m_req.resumeTick - 1;
        return t >= 2 ? t : -1;
    }
    Tick limit = m_req.resumeTick - 2;
    if (limit < 2) return -1;
    bool held[2][4] = {};
    int heldCount = 0;
    bool any = false;
    Tick lastEvent = 0;
    Tick best = -1;
    auto consider = [&](Tick from, Tick to) {
        Tick t = std::min(to, limit);
        if (t >= from && t >= 2) best = std::max(best, t);
    };
    for (auto const& in : m_req.plan) {
        if (in.tick >= m_req.resumeTick) break;
        if (heldCount == 0) consider(any ? lastEvent + 2 : 0, in.tick - 2);
        int p = in.player2 ? 1 : 0;
        int b = std::clamp<int>(in.button, 0, 3);
        bool& h = held[p][b];
        if (in.down && !h) {
            h = true;
            ++heldCount;
        } else if (!in.down && h) {
            h = false;
            --heldCount;
        }
        lastEvent = in.tick;
        any = true;
    }
    if (heldCount == 0) consider(any ? lastEvent + 2 : 0, limit);
    return best;
}

bool GameRunner::snapshotUsable() const {
    return m_snapAllowed && m_snapOk && m_req.useSnapshots && m_req.resumeTick >= 0 && m_snap.valid() &&
           m_snap.lastLimit < m_req.resumeTick;
}

void GameRunner::takeSnapshot(Tick now) {
    auto cp = m_layer->createCheckpoint();
    if (!cp) {
        disableSnapshots("the game refused to create a checkpoint");
        return;
    }
    Snapshot s;
    s.tick = now;
    s.lastLimit = m_lastLimit;
    s.progress = static_cast<long long>(m_layer->m_gameState.m_currentProgress);
    s.cp = cp;
    s.sig = capture();
    for (int p = 0; p < 2; ++p)
        for (int b = 0; b < 4; ++b) s.held[p][b] = m_held[p][b];
    m_snap = std::move(s);
}

bool GameRunner::restoreSnapshot() {
    m_layer->loadFromCheckpoint(m_snap.cp);
    m_layer->m_queuedButtons.clear();
    if (physicsTick(m_layer) != m_snap.tick) {
        // Some versions keep the clock outside of the checkpoint: put it back.
        m_layer->m_gameState.m_currentProgress =
            static_cast<decltype(m_layer->m_gameState.m_currentProgress)>(m_snap.progress);
    }
    if (physicsTick(m_layer) != m_snap.tick) {
        m_snapNote = "the physics clock is not restored by checkpoints";
        return false;
    }
    double worst = 0;
    if (!sigEqual(capture(), m_snap.sig, kEps, &worst)) {
        m_snapNote = fmt::format("player state differs after loading a checkpoint (off by {:.3f})", worst);
        return false;
    }
    m_lastLimit = m_snap.lastLimit;
    m_cursor = 0;
    while (m_cursor < m_req.plan.size() && m_req.plan[m_cursor].tick <= m_lastLimit) ++m_cursor;
    m_lastSeen = m_snap.tick;

    // Held buttons (Ultra). Policy 1: the checkpoint already brought them back (Practice Fix); policy 2: press again.
    for (int p = 0; p < 2; ++p)
        for (int b = 0; b < 4; ++b) m_held[p][b] = m_snap.held[p][b];
    if (m_req.holdRestore == 2) {
        m_injecting = true;
        for (int p = 0; p < 2; ++p)
            for (int b = 1; b <= 3; ++b)
                if (m_held[p][b]) m_layer->handleButton(true, b, p == 0);
        m_injecting = false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Trial lifecycle
// ---------------------------------------------------------------------------
void GameRunner::finish(bool pass, Fail reason, bool completed, std::string detail, bool converged) {
    if (m_state == St::Idle && m_result) return;  // already finished
    TrialResult r;
    r.pass = pass;
    r.reason = pass ? Fail::None : reason;
    r.completed = completed;
    r.converged = converged;
    r.snapshotUsed = m_usedSnapshot;
    r.detail = std::move(detail);
    if (m_layer) {
        r.tick = physicsTick(m_layer);
        r.percent = completed ? 100.0 : static_cast<double>(m_layer->getCurrentPercent());
    }
    m_result = std::move(r);
    m_state = St::Idle;
}

void GameRunner::resetForTrial() {
    m_resetting = true;
    m_layer->m_isTestMode = true;
    m_layer->resetLevelFromStart();
    m_resetting = false;

    // Clean input state: a reset does not release buttons held by the previous trial.
    m_layer->m_queuedButtons.clear();
    if (m_layer->m_player1) m_layer->m_player1->releaseAllButtons();
    if (m_layer->m_player2) m_layer->m_player2->releaseAllButtons();
    m_layer->m_queuedButtons.clear();

    m_cursor = 0;
    m_lastLimit = -1;
    m_lastSeen = -1;
    m_idleUpdates = 0;
    m_conv = 0;
    for (auto& row : m_held)
        for (auto& v : row) v = false;
}

// Ultra: the trial starts from a saved state, so the level is not restarted. What the previous trial left
// behind (held buttons, queued presses, a death flag) is cleaned here; the saved state brings back the rest.
void GameRunner::lightPrepare() {
    m_layer->m_isTestMode = true;
    m_layer->m_queuedButtons.clear();
    for (auto* p : {m_layer->m_player1, m_layer->m_player2}) {
        if (!p) continue;
        p->m_isDead = false;
        p->releaseAllButtons();
    }
    m_layer->m_queuedButtons.clear();
    m_cursor = 0;
    m_lastLimit = -1;
    m_lastSeen = -1;
    m_idleUpdates = 0;
    m_conv = 0;
    for (auto& row : m_held)
        for (auto& v : row) v = false;
}

void GameRunner::prepareStep() {
    if (m_state != St::NeedsReset) return;
    if (!m_layer) {
        finish(false, Fail::ReplayError, false, "the level is no longer open");
        return;
    }
    if (m_layer->m_isPlatformer) {
        finish(false, Fail::ReplayError, false, "platformer levels are not supported");
        return;
    }
    if (m_layer->m_isPracticeMode || m_layer->m_startPosObject) {
        finish(false, Fail::ReplayError, false, "use normal mode from 0% (no practice mode, no start pos)");
        return;
    }

    bool useSnap = snapshotUsable();
    m_usedSnapshot = false;
    if (useSnap && m_req.lightReset) {
        // Ultra: no level restart, load the saved state straight away.
        lightPrepare();
        if (restoreSnapshot()) {
            m_usedSnapshot = true;
        } else {
            disableSnapshots(m_snapNote.empty() ? std::string("restoring a saved state failed") : m_snapNote);
            resetForTrial();
        }
    } else {
        resetForTrial();
        if (useSnap) {
            if (restoreSnapshot()) {
                m_usedSnapshot = true;
            } else {
                // The saved state is not trustworthy: switch the speed-up off and start this trial from 0%.
                disableSnapshots(m_snapNote.empty() ? std::string("restoring a saved state failed") : m_snapNote);
                resetForTrial();
            }
        }
    }

    // Does this trial need to leave a new saved state behind for the next ones?
    m_snapTarget = -1;
    if (m_snapAllowed && m_snapOk && m_req.useSnapshots && !m_req.baseline && m_req.resumeTick >= 3) {
        Tick want = pickSnapshotTick();
        Tick from = m_usedSnapshot ? m_snap.tick : 0;
        if (want >= 2 && want > from && (!m_usedSnapshot || want - from >= m_req.snapGap)) m_snapTarget = want;
    }
    m_state = St::Running;
}

void GameRunner::injectUpTo(Tick limit) {
    if (limit == m_lastLimit) return;
    Tick expected = m_lastLimit < 0 ? 0 : m_lastLimit + 1;
    if (limit != expected) {
        finish(false, Fail::ReplayError, false,
               fmt::format("physics tick sequence broken (expected {}, got {}); disable TPS/speed mods", expected, limit));
        return;
    }
    m_lastLimit = limit;

    m_injecting = true;
    while (m_cursor < m_req.plan.size()) {
        auto const& in = m_req.plan[m_cursor];
        if (in.tick > limit) break;
        if (in.tick < limit) {
            m_injecting = false;
            finish(false, Fail::ReplayError, false, fmt::format("input at tick {} was skipped", in.tick));
            return;
        }
        // GDR: button 1 = jump. Classic levels only; player 2 = second player.
        m_layer->handleButton(in.down, static_cast<int>(in.button), !in.player2);
        m_held[in.player2 ? 1 : 0][std::clamp<int>(in.button, 0, 3)] = in.down;
        if (m_onInput) m_onInput(in);
        ++m_cursor;
    }
    m_injecting = false;
}

void GameRunner::beforeCommands(bool halfTick) {
    if (m_state != St::Running) return;
    if (halfTick) {
        finish(false, Fail::ReplayError, false,
               "half-tick physics detected: disable sub-tick / 480 TPS options of other mods");
        return;
    }
    auto raw = m_layer->m_gameState.m_currentProgress;
    if (raw % 2 != 0) {
        finish(false, Fail::ReplayError, false, "unaligned physics clock");
        return;
    }
    // "Before commands": frame N is queued before the physics step that starts at tick N.
    // "After commands": frame N >= 1 is queued after the clock advanced to N (Mega Hack GDR2
    // convention); frame 0 is still a start-up input queued here.
    if (!m_after || raw == 0) injectUpTo(static_cast<Tick>(raw / 2));
}

void GameRunner::afterCommands(bool halfTick) {
    if (m_state != St::Running || !m_after || halfTick) return;
    injectUpTo(physicsTick(m_layer));
}

void GameRunner::afterStep() {
    if (m_state != St::Running) return;
    Tick now = physicsTick(m_layer);

    // The simulation must keep advancing.
    if (now == m_lastSeen) {
        if (++m_idleUpdates > 2400) {
            finish(false, Fail::ReplayError, false, "the simulation stopped advancing");
            return;
        }
    } else {
        m_idleUpdates = 0;
        m_lastSeen = now;
    }

    if (m_layer->m_player1->m_isDead || (m_layer->m_gameState.m_isDualMode && m_layer->m_player2->m_isDead)) {
        onDeath();
        return;
    }
    if (m_lastLimit < 0) return;  // nothing simulated yet

    // --- original-run trace, saved states, early pass -------------------------------------------------
    bool needSig = m_req.recordTrace || m_req.compareTrace || m_snapTarget == now ||
                   (m_req.convergeTicks > 0 && m_req.changedUntil >= 0 && now > m_req.changedUntil);
    if (needSig) {
        Sig cur = capture();
        auto index = static_cast<std::size_t>(now);

        if (m_req.recordTrace) {
            if (m_trace.size() <= index) m_trace.resize(index + 1);
            m_trace[index] = cur;
        }
        if (m_snapTarget >= 0 && now == m_snapTarget) {
            m_snapTarget = -1;
            takeSnapshot(now);
        }
        if (m_req.compareTrace && !m_req.recordTrace && index < m_trace.size()) {
            double worst = 0;
            if (!sigEqual(cur, m_trace[index], kEps, &worst)) {
                finish(false, Fail::ReplayError, false,
                       fmt::format("state differs from the original run at tick {} (off by {:.3f})", now, worst));
                return;
            }
        } else if (m_req.convergeTicks > 0 && !m_req.recordTrace && m_req.changedUntil >= 0 && now > m_req.changedUntil &&
                   index < m_trace.size()) {
            // After the last moved input the plan equals the original: once the state equals the original
            // run again, the rest of the run is identical and it will pass.
            if (sigEqual(cur, m_trace[index], kEps)) {
                if (++m_conv >= m_req.convergeTicks) {
                    finish(true, Fail::None, false, {}, true);
                    return;
                }
            } else {
                m_conv = 0;
            }
        }
    }

    if (!m_req.requireCompletion && !m_req.baseline && now >= m_req.validateUntil) {
        finish(true, Fail::None, false);
        return;
    }
    if (now >= m_req.timeoutTick) finish(false, Fail::Timeout, false);
}

void GameRunner::onDeath() {
    if (m_state != St::Running) return;
    finish(false, Fail::Death, false);
}

void GameRunner::onExternalReset() {
    if (m_state != St::Running || m_resetting) return;
    finish(false, Fail::ReplayError, false, "the level was restarted while a trial was running");
}

void GameRunner::onComplete() {
    if (m_state != St::Running) return;
    bool consumed = m_cursor == m_req.plan.size();
    if (consumed) {
        finish(true, Fail::None, true);
    } else {
        finish(false, Fail::InputsNotConsumed, true,
               fmt::format("{} of {} inputs executed", m_cursor, m_req.plan.size()));
    }
}

}  // namespace fwm
