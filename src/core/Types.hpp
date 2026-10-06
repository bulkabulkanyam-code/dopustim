// Frame Window Mod - core types (no Geode dependencies, unit-testable).
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fwm {

using Tick = std::int64_t;

// One recorded input. `button` follows GDR: 1 = jump (classic levels only).
struct Input {
    Tick tick = 0;
    std::uint8_t button = 1;
    bool player2 = false;
    bool down = false;  // true = press, false = release
    int id = 0;         // index in Replay::inputs (invariant: inputs[i].id == i)
};

struct Replay {
    std::vector<Input> inputs;  // sorted by tick (stable w.r.t. file order)
    double tps = 240.0;
    std::string name;
    std::string bot;
    std::string author;
    std::uint32_t levelId = 0;
    std::string levelName;
    bool ldm = false;
    bool platformer = false;

    Tick lastInputTick() const { return inputs.empty() ? 0 : inputs.back().tick; }
};

// ----------------------------------------------------------------------------
// Analysis settings
// ----------------------------------------------------------------------------
struct Settings {
    bool presses = true;
    bool releases = false;
    bool player1 = true;
    bool player2 = true;
    bool keepHoldLength = false;  // when moving a press, move its paired release by the same amount
    bool fullCompletion = false;  // validate against full level completion instead of a horizon
    int horizonSeconds = 5;       // validation horizon after the input (ignored if fullCompletion)
    int maxWindow = 10;           // stop searching once the window is known to exceed this
    int repeats = 1;              // identical trials per candidate (detects non-determinism)
    int timeoutSeconds = 60;      // baseline: max time after the last input to wait for completion
    int fromInput = 1;            // 1-based input number range (to = 0 -> last)
    int toInput = 0;
    bool inputsAfterCommands = false;  // "Mega Hack style" input phase
};

// ----------------------------------------------------------------------------
// Trial protocol between Analyzer (core) and runner (game or test fake)
// ----------------------------------------------------------------------------
enum class Fail {
    None,
    Death,
    Timeout,            // validation / completion not reached in time
    InputsNotConsumed,  // level completed before every planned input was executed
    ReplayError,        // simulation did not behave as expected (skipped tick, reset, ...)
};

inline const char* toString(Fail f) {
    switch (f) {
        case Fail::None: return "none";
        case Fail::Death: return "died";
        case Fail::Timeout: return "timeout";
        case Fail::InputsNotConsumed: return "level ended before all inputs ran";
        case Fail::ReplayError: return "replay/simulation error";
    }
    return "?";
}

struct TrialRequest {
    std::vector<Input> plan;  // effective inputs, sorted by tick (stable)
    Tick validateUntil = 0;   // pass once this many ticks were simulated alive (ignored if requireCompletion)
    bool requireCompletion = false;
    bool baseline = false;    // all planned inputs must be executed before completion
    Tick timeoutTick = 0;     // hard stop (ticks since start of attempt)
};

struct TrialResult {
    bool pass = false;
    Fail reason = Fail::None;
    Tick tick = 0;  // ticks simulated when the trial ended
    double percent = 0;
    bool completed = false;
    std::string detail;
};

class ITrialRunner {
public:
    virtual ~ITrialRunner() = default;
    virtual void begin(TrialRequest request) = 0;
    virtual std::optional<TrialResult> poll() = 0;  // result of a finished trial (consumed on read)
    virtual void cancel() = 0;
};

// ----------------------------------------------------------------------------
// Results
// ----------------------------------------------------------------------------
struct InputResult {
    int id = 0;
    Tick tick = 0;
    std::uint8_t button = 1;
    bool player2 = false;
    bool down = true;
    int early = 0;            // consecutive passing ticks earlier than the original
    int late = 0;             // consecutive passing ticks later than the original
    bool earlyBound = false;  // search stopped by a neighbouring input / level start
    bool lateBound = false;   // search stopped by a neighbouring input / level end
    bool capped = false;      // window is larger than Settings::maxWindow (value is a lower bound)
    bool unstable = false;    // repeated trials disagreed
    int trials = 0;

    int window() const { return early + late + 1; }
};

}  // namespace fwm
