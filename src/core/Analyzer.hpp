// Frame Window Mod - analysis state machine (no Geode dependencies).
//
// Definition used (same as other replay-based tools): for an input recorded at
// tick F, the window is the number of consecutive ticks [E..L] around F such that,
// when ONLY that input is moved to any tick in [E..L] and every other input stays
// at its recorded tick, the level still plays correctly (survives for the
// validation horizon, or to completion in "full completion" mode).
//   window = L - E + 1,  early = F - E,  late = L - F
// The ticks are physics ticks at the replay's TPS (normally 240).
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
    struct Bound {
        Tick lo = 0, hi = 0;  // allowed ticks for the moved input
        int carry = -1;       // index of the paired release moved along (keepHoldLength)
    };

    void fail(std::string message);
    void onResult(TrialResult const& r);
    void launchBaseline();
    void launchNext();
    void launchCandidate(int shift);
    std::optional<int> nextShift();
    void endPhase();
    void initCurrent();
    void computeBounds();
    void buildSelected();
    TrialRequest makeCandidate(int index, int shift) const;

    std::shared_ptr<Replay const> m_replay;
    Settings m_settings;
    ITrialRunner& m_runner;

    State m_state = State::Idle;
    State m_pausedFrom = State::Idle;
    std::string m_error;
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

    bool m_curInit = false;
    InputResult m_cur;
    Phase m_phase = Phase::Early;
    int m_k = 0;
    int m_d = 0;
    int m_repDone = 0;
    int m_repPass = 0;
};

}  // namespace fwm
