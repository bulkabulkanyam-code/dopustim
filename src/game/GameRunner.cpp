#include "GameRunner.hpp"

using namespace geode::prelude;

namespace fwm {

namespace {
// GD 2.2081: GJGameState::m_currentProgress counts half ticks; one full physics
// step (1/240 s) advances it by two.
Tick physicsTick(PlayLayer* layer) {
    return static_cast<Tick>(layer->m_gameState.m_currentProgress / 2);
}
}  // namespace

GameRunner::GameRunner(PlayLayer* layer, bool inputsAfterCommands)
    : m_layer(layer), m_after(inputsAfterCommands), m_originalTestMode(layer->m_isTestMode) {
    // Test mode: the game does not save progress / stats for these attempts.
    m_layer->m_isTestMode = true;
}

GameRunner::~GameRunner() {
    restoreLayer();
}

void GameRunner::restoreLayer() {
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
// Trial lifecycle
// ---------------------------------------------------------------------------
void GameRunner::finish(bool pass, Fail reason, bool completed, std::string detail) {
    if (m_state == St::Idle && m_result) return;  // already finished
    TrialResult r;
    r.pass = pass;
    r.reason = pass ? Fail::None : reason;
    r.completed = completed;
    r.detail = std::move(detail);
    if (m_layer) {
        r.tick = physicsTick(m_layer);
        r.percent = completed ? 100.0 : static_cast<double>(m_layer->getCurrentPercent());
    }
    m_result = std::move(r);
    m_state = St::Idle;
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
