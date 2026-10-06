// Frame Window Mod - Session: owns the imported replay, the running analysis and the results.
// Everything here runs on the main thread.
#pragma once
#include <Geode/Geode.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/Analyzer.hpp"
#include "../core/Export.hpp"
#include "../core/Precision.hpp"
#include "GameRunner.hpp"

namespace fwm {

struct Config {
    Settings analysis;
    PrecisionParams precision;
    bool precisionReleases = false;
    int speed = 0;       // physics steps per real time: 1, 5, 20 or 0 = as fast as possible
    bool mute = true;
    int inputPhase = 0;  // 0 = auto, 1 = before commands, 2 = after commands
};

// Reads all values from the mod settings.
Config readConfig();

class Session {
public:
    static Session& get();

    // ----- replay -----
    std::shared_ptr<Replay const> replay;
    void loadReplay(std::filesystem::path const& path);  // throws std::runtime_error
    std::string replayWarning() const;                   // level id / LDM mismatch hints, may be empty

    // ----- analysis control -----
    bool start(bool verifyOnly, std::string& error);
    void pauseAnalysis();
    void resumeAnalysis();
    void stopAnalysis();                 // keeps the results, level stays frozen until release()
    void release(bool restartLevel);     // gives the level back to the player
    bool analyzerExists() const { return m_analyzer != nullptr; }
    State state() const { return m_analyzer ? m_analyzer->state() : State::Idle; }
    bool analysisActive() const { return m_analyzer && m_analyzer->active(); }
    std::string status() const;
    std::string analyzerError() const { return m_analyzer ? m_analyzer->error() : std::string(); }
    float progress() const;  // 0..1

    // ----- results -----
    std::vector<InputResult> const& results() const { return m_analyzer ? m_analyzer->results() : m_results; }
    Tick endTick() const { return m_analyzer ? m_analyzer->endTick() : m_endTick; }
    bool hasResults() const { return !results().empty(); }
    PrecisionResult computePrecision(int* usedInputs = nullptr) const;
    // Writes .json / .fwc / NaNDL .json / .csv to <save dir>/exports and returns that directory.
    std::filesystem::path exportAll(std::string& baseName);

    // ----- hooks -----
    GameRunner* runnerFor(GJBaseGameLayer* layer);  // runner if the analysis owns this layer
    bool bound() const { return m_analyzer != nullptr; }
    void runFrame(float dt, std::function<void(float)> const& physicsStep);
    void onPauseOpened();
    void layerGone(PlayLayer* layer);

private:
    Session() = default;
    void setMuted(bool mute);
    void ensureOverlay();
    void removeOverlay();
    void refreshOverlay(bool force);
    void announceFinished();

    PlayLayer* m_layer = nullptr;
    std::unique_ptr<GameRunner> m_runner;
    std::unique_ptr<Analyzer> m_analyzer;
    Config m_cfg;
    std::vector<InputResult> m_results;
    Tick m_endTick = 0;

    double m_acc = 0.0;
    bool m_announced = true;
    bool m_muted = false;
    float m_oldMusic = 1.f, m_oldSfx = 1.f;
    geode::Ref<cocos2d::CCLabelBMFont> m_label;
    geode::Ref<cocos2d::CCLayerColor> m_bar;
    std::chrono::steady_clock::time_point m_lastOverlay{};
};

}  // namespace fwm
