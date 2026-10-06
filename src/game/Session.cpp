#include "Session.hpp"

#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>

#include "../ui/FrameWindowPopup.hpp"
#include "ReplayImport.hpp"

using namespace geode::prelude;

namespace fwm {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
Config readConfig() {
    auto m = Mod::get();
    Config c;
    auto flag = [&](char const* key) { return m->getSettingValue<bool>(key); };
    auto integer = [&](char const* key) { return static_cast<int>(m->getSettingValue<int64_t>(key)); };
    auto real = [&](char const* key) { return m->getSettingValue<double>(key); };
    auto text = [&](char const* key) { return m->getSettingValue<std::string>(key); };

    c.analysis.presses = flag("presses");
    c.analysis.releases = flag("releases");
    c.analysis.player1 = flag("player1");
    c.analysis.player2 = flag("player2");
    c.analysis.keepHoldLength = flag("keep-hold");
    c.analysis.fullCompletion = flag("full-completion");
    c.analysis.horizonSeconds = integer("horizon");
    c.analysis.maxWindow = integer("max-window");
    c.analysis.repeats = integer("repeats");
    c.analysis.fromInput = integer("from-input");
    c.analysis.toInput = integer("to-input");

    auto speed = text("speed");
    c.speed = speed == "1x" ? 1 : speed == "5x" ? 5 : speed == "20x" ? 20 : 0;
    c.mute = flag("mute");
    auto phase = text("input-phase");
    c.inputPhase = phase == "Before commands" ? 1 : phase == "After commands" ? 2 : 0;

    c.precision.respawnSeconds = real("p-respawn");
    c.precision.targetSeconds = real("p-target");
    c.precision.kNerve = real("p-kt");
    c.precision.kFatigue = real("p-ku");
    c.precision.kCps = real("p-kc");
    c.precisionReleases = flag("p-releases");
    return c;
}

Session& Session::get() {
    static Session* instance = new Session();  // intentionally never destroyed
    return *instance;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------
void Session::loadReplay(std::filesystem::path const& path) {
    if (m_analyzer && m_analyzer->active()) throw std::runtime_error("stop the running analysis first");
    auto loaded = importReplay(path);
    if (m_analyzer) release(false);
    replay = std::move(loaded);
    m_results.clear();
    m_endTick = 0;
}

std::string Session::replayWarning() const {
    if (!replay) return {};
    std::string w;
    if (std::abs(replay->tps - 240.0) > 0.01) {
        w += fmt::format("Replay TPS is {:.0f}; only 240 TPS is supported. ", replay->tps);
    }
    if (auto layer = PlayLayer::get()) {
        auto id = static_cast<std::uint32_t>(layer->m_level->m_levelID.value());
        if (replay->levelId != 0 && id != 0 && replay->levelId != id) {
            w += fmt::format("Replay level ID {} differs from the open level ({}). ", replay->levelId, id);
        }
        if (replay->ldm != layer->m_lowDetailMode) {
            w += replay->ldm ? "Replay was recorded with LDM on. " : "Replay was recorded with LDM off. ";
        }
    }
    return w;
}

// ---------------------------------------------------------------------------
// Analysis control
// ---------------------------------------------------------------------------
static bool looksLikeMegaHack(std::string const& bot) {
    std::string s = bot;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s.find("mega") != std::string::npos;
}

bool Session::start(bool verifyOnly, std::string& error) {
    auto layer = PlayLayer::get();
    if (!layer) {
        error = "Open a level first (the analysis runs inside a level).";
        return false;
    }
    if (!replay) {
        error = "Import a replay (.gdr / .gdr2) first.";
        return false;
    }
    if (std::abs(replay->tps - 240.0) > 0.01) {
        error = fmt::format("This replay was recorded at {:.0f} TPS. Only 240 TPS replays are supported.", replay->tps);
        return false;
    }
    if (layer->m_isPlatformer) {
        error = "Platformer levels are not supported.";
        return false;
    }
    if (layer->m_isPracticeMode || layer->m_startPosObject) {
        error = "Use normal mode from 0%: no practice mode and no start pos.";
        return false;
    }
    if (layer->m_isIgnoreDamageEnabled || layer->m_ignoreDamage) {
        error = "Turn off noclip / ignore damage.";
        return false;
    }
    if (m_analyzer && m_analyzer->active()) {
        error = "An analysis is already running.";
        return false;
    }

    // Drop a previous (finished/stopped) analysis, but keep the layer bound.
    if (m_analyzer) release(false);

    m_cfg = readConfig();
    bool after = m_cfg.inputPhase == 2 || (m_cfg.inputPhase == 0 && looksLikeMegaHack(replay->bot));

    m_layer = layer;
    m_runner = std::make_unique<GameRunner>(layer, after);
    m_analyzer = std::make_unique<Analyzer>(replay, m_cfg.analysis, *m_runner);
    m_acc = 0.0;
    m_announced = false;
    m_analyzer->start(verifyOnly);
    setMuted(m_cfg.mute);
    ensureOverlay();
    refreshOverlay(true);
    return true;
}

void Session::pauseAnalysis() {
    if (!m_analyzer) return;
    m_analyzer->pause();
    setMuted(false);
    refreshOverlay(true);
}

void Session::resumeAnalysis() {
    if (!m_analyzer || m_analyzer->state() != State::Paused) return;
    m_acc = 0.0;
    m_analyzer->resume();
    setMuted(m_cfg.mute);
    refreshOverlay(true);
}

void Session::stopAnalysis() {
    if (!m_analyzer) return;
    m_analyzer->stop();
    setMuted(false);
    m_announced = true;
    refreshOverlay(true);
}

void Session::release(bool restartLevel) {
    if (m_analyzer) {
        m_results = m_analyzer->results();
        m_endTick = m_analyzer->endTick();
        m_analyzer->stop();
    }
    setMuted(false);
    m_analyzer.reset();  // the analyzer references the runner: destroy it first
    auto layer = m_layer;
    if (m_runner) {
        m_runner->restoreLayer();
        m_runner.reset();
    }
    removeOverlay();
    m_layer = nullptr;
    if (restartLevel && layer) {
        Ref<PlayLayer> keep(layer);
        queueInMainThread([keep]() { keep->resetLevelFromStart(); });
    }
}

void Session::layerGone(PlayLayer* layer) {
    if (layer != m_layer) return;
    if (m_runner) m_runner->abandon();  // the layer is being destroyed: do not touch it
    release(false);
}

void Session::onPauseOpened() {
    if (m_analyzer && m_analyzer->active()) pauseAnalysis();
}

GameRunner* Session::runnerFor(GJBaseGameLayer* layer) {
    if (!m_runner || !m_layer) return nullptr;
    return static_cast<GJBaseGameLayer*>(m_layer) == layer ? m_runner.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Frame loop (called from the GJBaseGameLayer::update hook)
// ---------------------------------------------------------------------------
void Session::runFrame(float dt, std::function<void(float)> const& physicsStep) {
    if (!m_analyzer || !m_runner) return;

    if (m_analyzer->active()) {
        constexpr double kStep = 1.0 / 240.0;
        if (m_cfg.speed > 0) m_acc = std::min(m_acc + static_cast<double>(dt) * m_cfg.speed, 0.25 * m_cfg.speed + kStep);
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
        for (int guard = 0; guard < 400000 && m_analyzer->active(); ++guard) {
            if (std::chrono::steady_clock::now() >= until) break;
            m_analyzer->advance();
            if (!m_analyzer->active()) break;
            if (!m_runner->hasTrial()) continue;  // the analyzer polls the result / starts the next trial
            m_runner->prepareStep();
            if (!m_runner->running()) continue;
            if (m_cfg.speed > 0) {
                if (m_acc < kStep) break;
                m_acc -= kStep;
            }
            physicsStep(static_cast<float>(kStep));
            m_runner->afterStep();
        }
    }

    refreshOverlay(false);
    if (!m_announced && !m_analyzer->active() && m_analyzer->state() != State::Paused) announceFinished();
}

void Session::announceFinished() {
    m_announced = true;
    setMuted(false);
    refreshOverlay(true);
    auto st = m_analyzer->state();
    if (st == State::Failed) {
        std::string msg = m_analyzer->error();
        queueInMainThread([msg]() {
            FLAlertLayer::create("Frame Window Mod", msg.c_str(), "OK")->show();
        });
        return;
    }
    if (st == State::Completed) {
        queueInMainThread([]() {
            Notification::create("Frame windows ready", NotificationIcon::Success)->show();
            if (PlayLayer::get()) FrameWindowPopup::open();
        });
    }
}

// ---------------------------------------------------------------------------
// Status overlay on the level
// ---------------------------------------------------------------------------
std::string Session::status() const {
    if (!m_analyzer) return replay ? fmt::format("{} inputs loaded", replay->inputs.size()) : "No replay loaded";
    return m_analyzer->statusLine();
}

float Session::progress() const {
    if (!m_analyzer) return 0.f;
    int total = m_analyzer->selectedCount();
    if (m_analyzer->state() == State::Completed) return 1.f;
    if (total <= 0) return 0.f;
    return std::clamp(static_cast<float>(m_analyzer->doneCount()) / static_cast<float>(total), 0.f, 1.f);
}

void Session::ensureOverlay() {
    if (!m_layer || m_label) return;
    auto size = CCDirector::sharedDirector()->getWinSize();
    auto label = CCLabelBMFont::create("", "chatFont.fnt");
    label->setScale(0.45f);
    label->setAnchorPoint({0.5f, 1.f});
    label->setPosition({size.width / 2, size.height - 6.f});
    label->setZOrder(1000);
    m_layer->addChild(label);
    m_label = label;

    auto bar = CCLayerColor::create({90, 220, 130, 220}, 1.f, 3.f);
    bar->setPosition({size.width * 0.2f, size.height - 22.f});
    bar->setZOrder(1000);
    m_layer->addChild(bar);
    m_bar = bar;
}

void Session::removeOverlay() {
    if (m_label) m_label->removeFromParent();
    if (m_bar) m_bar->removeFromParent();
    m_label = nullptr;
    m_bar = nullptr;
}

void Session::refreshOverlay(bool force) {
    if (!m_label || !m_bar) return;
    auto now = std::chrono::steady_clock::now();
    if (!force && now - m_lastOverlay < std::chrono::milliseconds(150)) return;
    m_lastOverlay = now;
    m_label->setString(status().c_str());
    auto size = CCDirector::sharedDirector()->getWinSize();
    float width = std::max(1.f, size.width * 0.6f * progress());
    m_bar->setContentSize({width, 3.f});
}

void Session::setMuted(bool mute) {
    auto fmod = FMODAudioEngine::sharedEngine();
    if (!fmod) return;
    if (mute && !m_muted) {
        m_oldMusic = fmod->getBackgroundMusicVolume();
        m_oldSfx = fmod->getEffectsVolume();
        fmod->setBackgroundMusicVolume(0.f);
        fmod->setEffectsVolume(0.f);
        m_muted = true;
    } else if (!mute && m_muted) {
        fmod->setBackgroundMusicVolume(m_oldMusic);
        fmod->setEffectsVolume(m_oldSfx);
        m_muted = false;
    }
}

// ---------------------------------------------------------------------------
// Precision / export
// ---------------------------------------------------------------------------
PrecisionResult Session::computePrecision(int* usedInputs) const {
    auto cfg = readConfig();
    PrecisionParams p = cfg.precision;
    p.tps = replay ? replay->tps : 240.0;
    auto inputs = toPrecisionInputs(results(), cfg.precisionReleases);
    if (usedInputs) *usedInputs = static_cast<int>(inputs.size());
    return solveAll(inputs, p);
}

static std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    }
    return s.empty() ? std::string("replay") : s;
}

std::filesystem::path Session::exportAll(std::string& baseName) {
    auto dir = Mod::get()->getSaveDir() / "exports";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string stem = replay ? std::filesystem::path(replay->name).stem().string() : std::string("replay");
    char stamp[32] = {0};
    std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
    baseName = sanitize(stem) + "_" + stamp;

    auto cfg = readConfig();
    double tps = replay ? replay->tps : 240.0;
    auto const& rs = results();
    bool partialRun = m_analyzer && m_analyzer->state() != State::Completed;

    auto writeText = [&](std::string const& ext, std::string const& content) {
        std::ofstream f(dir / (baseName + ext), std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    };
    if (replay) writeText(".json", toResultsJson(*replay, cfg.analysis, rs, endTick(), partialRun));
    writeText(".nandl.json", toNandlJson(rs, tps, cfg.precision.respawnSeconds, cfg.precisionReleases));
    writeText(".csv", toCsv(rs));
    auto fwc = toFwc(rs, tps, cfg.precisionReleases);
    std::ofstream f(dir / (baseName + ".fwc"), std::ios::binary);
    f.write(reinterpret_cast<char const*>(fwc.data()), static_cast<std::streamsize>(fwc.size()));
    return dir;
}

}  // namespace fwm
