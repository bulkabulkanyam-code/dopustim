// Frame Window Mod - drives Geometry Dash's real physics to run one trial.
//
// A trial = reset the level, replay a list of inputs at exact physics ticks, and
// report death / completion / "survived N ticks". The simulation is the game's own
// (GJBaseGameLayer::update stepped at 1/TPS), there is no re-implemented physics.
//
// Design follows the approach used by other open-source replay analysers:
//   * fixed-step GJBaseGameLayer::update(1/240)
//   * inputs injected from GJBaseGameLayer::processCommands (once per physics step)
//   * real user input swallowed in handleButton
//   * PlayLayer::destroyPlayer / levelComplete intercepted as trial outcomes
#pragma once
#include <Geode/Geode.hpp>

#include <optional>

#include "../core/Types.hpp"

namespace fwm {

class GameRunner final : public ITrialRunner {
public:
    GameRunner(PlayLayer* layer, bool inputsAfterCommands);
    ~GameRunner() override;

    // ITrialRunner
    void begin(TrialRequest request) override;
    std::optional<TrialResult> poll() override;
    void cancel() override;

    // --- game loop interface (called from hooks / Session::pump) ---
    bool hasTrial() const { return m_state != St::Idle; }
    bool running() const { return m_state == St::Running; }
    void prepareStep();                  // performs the level reset when a new trial starts
    void beforeCommands(bool halfTick);  // before the original processCommands
    void afterCommands(bool halfTick);   // after it
    void afterStep();                    // after GJBaseGameLayer::update returned
    void onDeath();
    void onComplete();
    void onExternalReset();  // the level was restarted by something else during a trial
    bool resetting() const { return m_resetting; }
    bool injecting() const { return m_injecting; }
    Tick ticks() const;  // physics ticks simulated in the current trial

    // Give the level back to the player (release buttons, restore test mode).
    void restoreLayer();
    // The layer is being destroyed: forget it without touching it.
    void abandon() { m_layer = nullptr; m_state = St::Idle; }

private:
    enum class St { Idle, NeedsReset, Running };
    void finish(bool pass, Fail reason, bool completed, std::string detail = {});
    void injectUpTo(Tick limit);

    PlayLayer* m_layer;
    bool m_after;  // input phase: true = after processCommands (Mega Hack style)
    bool m_originalTestMode;

    TrialRequest m_req;
    St m_state = St::Idle;
    std::optional<TrialResult> m_result;

    std::size_t m_cursor = 0;
    Tick m_lastLimit = -1;  // highest tick already injected
    Tick m_lastSeen = -1;   // physics tick seen by the previous afterStep()
    int m_idleUpdates = 0;
    bool m_resetting = false;
    bool m_injecting = false;
};

}  // namespace fwm
