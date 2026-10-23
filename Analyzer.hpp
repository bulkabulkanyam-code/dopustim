// Frame Window Mod - analysis state machine (no Geode dependencies).
//
// Definition used (same as other replay-based tools): for an input recorded at
// tick F, the window is the number of consecutive ticks [E..L] around F such that,
// when ONLY that input is moved to any tick in [E..L] and every other input stays
// at its recorded tick, the level still plays correctly (survives for the
// validation horizon, or to completion in "full completion" mode).
//   window = L - E + 1,  early = F - E,  late = L - F
// The ticks are physics ticks at the replay's TPS (normally 240).
//
// Search rule (fast variant): the search goes outwards one tick at a time and STOPS at the first
// shift that fails, separately for the earlier and the later side. If the shifts -1 and +1 both
// fail, the window is 1 and the analysis goes straight to the next input. Gaps behind a failing
// shift are never tested.
//
// Speed-ups that do not change the result:
//   * saved states: every trial of an input starts from a game state taken shortly before the
//     earliest tick the input can move to (`TrialRequest::resumeTick`), not from 0%;
//   * early pass: a trial ends as soon as the run is identical to the original again;
//   * a self-test and periodic control runs make sure saved states are exact; if they are not,
//     the analysis silently falls back to full replays.
#pragma once
#include <string>
#include <vector>

#include "Types.hpp"

namespace fwm {

enum class State { Idle, Verifying, Running, Paused, Completed, Stopped, Failed };
const char* toString(State s);

class Analyzer {
public:
    Analyzer(std::shared_ptr<Replay const> replay, Settings settings, ITrialRunner& runner);

    // Verify the unmodified replay (it must complete the level) and, unless
    // verifyOnly is set, analyze every selected input.
    void start(bool verifyOnly);
    // Non-blocking; call repeatedly (e.g. every game step). Polls the runner,
    // processes results and launches the next trial.
    void advance();
    void pause();
    void resume();
    void stop();

    State state() const { return m_state; }
    bool active() const { return m_state == State::Verifying || m_state == State::Running; }
    std::string const& error() const { return m_error; }
    std::vector<InputResult> const& results() const { return m_results; }
    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    int doneCount() const { return m_pos; }
    Tick endTick() const { return m_endTick; }
    bool verified() const { return m_verified; }
    int totalTrials() const { return m_totalTrials; }
    Settings const& settings() const { return m_settings; }
    // Human readable note about saved states ("verified", "switched off: ..."); empty if unused.
    std::string const& snapshotNote() const { return m_snapNote; }

    struct Current {
        int number = 0;  // 1-based input number
        Tick tick = 0;
        bool down = true;
        bool player2 = false;
        int testing = 0;  // shift under test (0 while verifying)
        int early = 0, late = 0;
    };
    std::optional<Current> current() const;
    std::string statusLine() const;

private:
    enum class Phase { Early, Late, Done };
    enum class Stage { Baseline, SelfTest, Search, Control };
    struct Bound {
        Tick lo = 0, hi = 0;  // allowed ticks for the moved input
        int carry = -1;       // index of the paired release moved along (hold mode)
    };

    void fail(std::string message);
    void onResult(TrialResult const& r);
    void onBaseline(TrialResult const& r);
    void onSelfTest(TrialResult const& r);
    void onControl(TrialResult const& r);
    void onCandidate(TrialResult const& r);
    void relaunchCurrent();
    void launchBaseline();
    void launchSelfTest();
    void launchControl(int index);
    void launchNext();
    void launchCandidate(int shift);
    std::optional<int> nextShift();
    void endPhase();
    void initCurrent();
    void computeBounds();
    void buildSelected();
    void planSelfTest();
    void startSearch();
    bool wantsCarry(int i, int next) const;
    Tick earliestChange(int index) const;
    bool insideHold(Tick t) const;  // a button is held at the moment a saved state for tick t would be taken
    std::string policyText() const;
    TrialRequest makeCandidate(int index, int shift) const;
    TrialRequest makeHorizonRequest(int index) const;  // unmodified plan, shaped like a candidate of `index`

    std::shared_ptr<Replay const> m_replay;
    Settings m_settings;
    ITrialRunner& m_runner;

    State m_state = State::Idle;
    State m_pausedFrom = State::Idle;
    Stage m_stage = Stage::Baseline;
    std::string m_error;
    std::string m_snapNote;
    bool m_verifyOnly = false;
    bool m_verified = false;
    bool m_inFlight = false;
    bool m_resumePending = false;
    Tick m_endTick = 0;
    int m_totalTrials = 0;

    std::vector<Bound> m_bounds;
    std::vector<int> m_selected;
    int m_pos = 0;
    std::vector<InputResult> m_results;

    // self-test of saved states
    std::vector<Tick> m_selfTicks;
    std::size_t m_selfIdx = 0;
    bool m_selfSecond = false;  // second trial of a pair restores the state made by the first one
    int m_selfRestored = 0;     // checks that really started from a saved state

    // control runs
    int m_sinceControl = 0;
    int m_controlIndex = 0;   // input the running control run was shaped for
    int m_blockPos = 0;       // position / result count at the last successful control
    std::size_t m_blockResults = 0;

    // Ultra mode: how held buttons are restored (see TrialRequest::holdRestore), chosen by the self-test
    int m_holdPolicy = 0;
    std::vector<int> m_policyOrder;
    std::size_t m_policyIdx = 0;
    bool m_selfDrop = false;  // the next self-test trial must forget the old saved state
    std::string m_policyFailures;

    // gap scan (Accurate mode): shifts still to test beyond the first failing one on the current side
    int m_tail = 0;
    int m_tailAt = 0;

    bool m_curInit = false;
    InputResult m_cur;
    Phase m_phase = Phase::Early;
    int m_k = 0;
    int m_d = 0;
    int m_repDone = 0;
    int m_repPass = 0;
};

}  // namespace fwm
