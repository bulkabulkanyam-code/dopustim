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
//
// v0.3 "Ultra" mode (S&Q setting): saved states may be taken at ANY tick, also while a button is held. The
// runner remembers which buttons the replay holds at that moment and brings them back after loading
// (TrialRequest::holdRestore: 1 = the game's checkpoint does it, e.g. Practice Fix; 2 = press again), and a trial
// that starts from a saved state does not restart the level first (lightReset). The analyzer verifies this with
// a self-test and falls back to the exact v0.2 behaviour when it does not reproduce the original run.
//
// v0.2 speed-ups:
//   * saved states: a trial may start from a game checkpoint (PlayLayer::createCheckpoint) taken at a
//     quiet moment shortly before the first changed input. Taken lazily by the first trial that needs it.
//   * per-tick trace of the original run: lets a trial finish early once the moved input has been
//     "absorbed" (state identical to the original again) and lets the analyzer verify saved states.
#pragma once
#include <Geode/Geode.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../core/Types.hpp"

namespace fwm {

// Compact per-tick state of both players, used to compare runs.
struct Sig {
    float x = 0, y = 0;
    double yv = 0;
    std::uint32_t flags = 0;
    bool dual = false;
    float x2 = 0, y2 = 0;
    double yv2 = 0;
    std::uint32_t flags2 = 0;
};

// eps: allowed difference of positions / velocities.
bool sigEqual(Sig const& a, Sig const& b, double eps, double* worst = nullptr);

class GameRunner final : public ITrialRunner {
public:
    GameRunner(PlayLayer* layer, bool inputsAfterCommands, bool allowSnapshots);
    ~GameRunner() override;

    // ITrialRunner
    void begin(TrialRequest request) override;
    std::optional<TrialResult> poll() override;
    void cancel() override;
    bool snapshotsEnabled() const override { return m_snapAllowed && m_snapOk; }
    void disableSnapshots(std::string const& why) override;
    std::string snapshotNote() const override { return m_snapNote; }

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

    // Called after every injected input (used by the replay viewer to draw markers).
    void setInputCallback(std::function<void(Input const&)> cb) { m_onInput = std::move(cb); }

    // Give the level back to the player (release buttons, restore test mode).
    void restoreLayer();
    // The layer is being destroyed: forget it without touching it.
    void abandon() {
        m_layer = nullptr;
        m_state = St::Idle;
        m_snap = Snapshot{};
    }

private:
    enum class St { Idle, NeedsReset, Running };

    struct Snapshot {
        Tick tick = -1;       // physics ticks simulated when it was taken
        Tick lastLimit = -1;  // highest input tick already injected at that moment
        long long progress = 0;  // raw GJGameState::m_currentProgress
        geode::Ref<CheckpointObject> cp;
        Sig sig;
        bool held[2][4] = {};  // buttons held by the replay when it was taken (player x button)
        bool valid() const { return cp && tick >= 0; }
    };

    void finish(bool pass, Fail reason, bool completed, std::string detail = {}, bool converged = false);
    void injectUpTo(Tick limit);
    Sig capture() const;
    Tick pickSnapshotTick() const;  // latest quiet tick <= resumeTick - 2 (or -1)
    bool snapshotUsable() const;
    void takeSnapshot(Tick now);
    bool restoreSnapshot();
    void resetForTrial();
    void lightPrepare();  // Ultra: prepare a trial that starts from a saved state without restarting the level

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

    // saved states / trace
    bool m_snapAllowed;
    bool m_snapOk = true;
    std::string m_snapNote;
    Snapshot m_snap;
    Tick m_snapTarget = -1;       // tick at which this trial should take a new snapshot
    bool m_usedSnapshot = false;
    bool m_held[2][4] = {};       // buttons currently held by the injected inputs (player x button)
    std::vector<Sig> m_trace;     // trace[t] = state after t ticks of the ORIGINAL run
    int m_conv = 0;
    std::function<void(Input const&)> m_onInput;
};

}  // namespace fwm
