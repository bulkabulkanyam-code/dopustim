#include "Session.hpp"

#include <Geode/ui/Notification.hpp>
#include <Geode/utils/cocos.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "../ui/FrameWindowPopup.hpp"
#include "../ui/WindowColor.hpp"
#include "ReplayImport.hpp"

using namespace geode::prelude;

namespace fwm {

namespace {
// A filled circle with an outline. On Android CCDrawNode::drawDot is drawn as a square on many devices, so the
// markers are sprites of resources/fw_circle.png (a white disc, tinted). nullptr when the texture is missing:
// the caller then falls back to CCDrawNode (desktop).
CCNode* makeDisc(float radius, ccColor3B fill, ccColor3B outline, float outlineWidth) {
    if (!geode::cocos::fileExistsInSearchPaths("fw_circle.png"_spr)) return nullptr;
    auto node = CCNode::create();
    for (int layer = 0; layer < 2; ++layer) {
        auto spr = CCSprite::create("fw_circle.png"_spr);
        if (!spr) return nullptr;
        float r = layer == 0 ? radius + outlineWidth : radius;
        spr->setColor(layer == 0 ? outline : fill);
        spr->setOpacity(layer == 0 ? 235 : 245);
        spr->setScale(r * 2.f / std::max(1.f, spr->getContentSize().width));
        node->addChild(spr, layer);
    }
    return node;
}
}  // namespace

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
    auto hold = text("hold-mode");
    c.analysis.holdMode = hold == "Press only" ? HoldMode::PressOnly : hold == "Keep hold" ? HoldMode::KeepHold : HoldMode::Auto;
    c.analysis.snapshots = flag("snapshots");
    c.analysis.convergeTicks = flag("fast-validate") ? 120 : 0;
    c.analysis.controlEvery = integer("control-every");
    c.analysis.fullCompletion = flag("full-completion");
    c.analysis.horizonSeconds = integer("horizon");
    c.analysis.maxWindow = integer("max-window");
    c.analysis.repeats = integer("repeats");
    c.analysis.fromInput = integer("from-input");
    c.analysis.toInput = integer("to-input");

    // S&Q: decides the speed-ups and accuracy extras (overrides the individual speed settings above).
    auto sq = text("speed-quality");
    c.analysis.mode = sq == "Slow & Accurate" ? SpeedMode::Accurate : sq == "Ultra Fast & Inaccurate" ? SpeedMode::Ultra : SpeedMode::Fast;
    c.analysis = applySpeedMode(c.analysis);

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

    c.budgetMs = std::clamp(integer("time-budget"), 4, 200);
    c.viewerTick = flag("viewer-tick");
    c.viewerKeep = std::clamp(real("viewer-keep"), 0.5, 20.0);
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

std::filesystem::path Session::autoImport(std::string& note) {
    if (m_analyzer && m_analyzer->active()) throw std::runtime_error("stop the running analysis first");
    auto found = findBotReplays();
    if (found.empty()) {
        throw std::runtime_error(
            "No replays found in the Eclipse, Mega Hack or xdBot folders.\nSave a macro there (GDR format) or use Import.");
    }
    std::uint32_t openLevel = 0;
    if (auto layer = PlayLayer::get()) openLevel = static_cast<std::uint32_t>(layer->m_level->m_levelID.value());

    // 1) newest replay that was recorded on the level that is open now
    if (openLevel != 0) {
        int checked = 0;
        for (auto const& f : found) {
            if (checked++ >= 25) break;
            auto info = peekReplay(f.path);
            if (info.ok && info.levelId == openLevel) {
                loadReplay(f.path);
                note = fmt::format("{} - newest replay of this level ({})", f.source, ageText(f.time));
                return f.path;
            }
        }
    }
    // 2) otherwise the newest readable one
    for (auto const& f : found) {
        try {
            loadReplay(f.path);
            note = fmt::format("{} - newest replay ({}){}", f.source, ageText(f.time),
                               openLevel != 0 ? ", none was recorded on this level" : "");
            return f.path;
        } catch (std::exception const&) {
        }
    }
    throw std::runtime_error("Replays were found but none of them could be read (platformer / unsupported files).");
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

    // Drop a previous (finished/stopped) analysis or replay view, but keep the layer bound.
    if (m_analyzer || m_viewing || m_viewFinishing) release(false);

    m_cfg = readConfig();
    bool after = m_cfg.inputPhase == 2 || (m_cfg.inputPhase == 0 && looksLikeMegaHack(replay->bot));

    m_layer = layer;
    m_runner = std::make_unique<GameRunner>(layer, after, m_cfg.analysis.snapshots);
    m_analyzer = std::make_unique<Analyzer>(replay, m_cfg.analysis, *m_runner);
    m_acc = 0.0;
    m_announced = false;
    m_startedAt = std::chrono::steady_clock::now();
    m_lastSnapNote.clear();
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
    clearMarkers();
    if (m_precLabel) m_precLabel->removeFromParent();
    m_precLabel = nullptr;
    m_viewing = false;
    m_viewFinishing = false;
    m_viewPaused = false;
    if (m_analyzer) {
        m_results = m_analyzer->results();
        m_endTick = m_analyzer->endTick();
        m_lastSnapNote = m_analyzer->snapshotNote();
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
    if (m_viewing) m_viewPaused = true;
}

void Session::onResumePressed() {
    m_viewPaused = false;
    m_acc = 0.0;
}

GameRunner* Session::runnerFor(GJBaseGameLayer* layer) {
    if (!m_runner || !m_layer) return nullptr;
    return static_cast<GJBaseGameLayer*>(m_layer) == layer ? m_runner.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Frame loop (called from the GJBaseGameLayer::update hook)
// ---------------------------------------------------------------------------
void Session::runFrame(float dt, std::function<void(float)> const& physicsStep) {
    if (m_viewing || m_viewFinishing) {
        runViewerFrame(dt, physicsStep);
        return;
    }
    if (!m_analyzer || !m_runner) return;

    if (m_analyzer->active()) {
        constexpr double kStep = 1.0 / 240.0;
        if (m_cfg.speed > 0) m_acc = std::min(m_acc + static_cast<double>(dt) * m_cfg.speed, 0.25 * m_cfg.speed + kStep);
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_cfg.budgetMs);
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
std::string Session::etaText() const {
    if (!m_analyzer || m_analyzer->state() != State::Running) return {};
    int done = m_analyzer->doneCount(), total = m_analyzer->selectedCount();
    if (done < 3 || total <= done) return {};
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startedAt).count();
    int secs = static_cast<int>(elapsed * static_cast<double>(total - done) / static_cast<double>(done));
    if (secs >= 3600) return fmt::format(" | ETA {}h {:02}m", secs / 3600, (secs / 60) % 60);
    return fmt::format(" | ETA {}m {:02}s", secs / 60, secs % 60);
}

std::string Session::status() const {
    if (m_viewing) {
        return fmt::format("Replay with frame windows | tick {} / {}", m_runner ? m_runner->ticks() : 0, endTick());
    }
    if (m_viewFinishing) return "Replay finished";
    if (!m_analyzer) return replay ? fmt::format("{} inputs loaded", replay->inputs.size()) : "No replay loaded";
    std::string line = m_analyzer->statusLine();
    if (m_analyzer->state() == State::Running) {
        line += etaText();
        auto mode = m_analyzer->settings().mode;
        line += mode == SpeedMode::Ultra ? " | Ultra" : mode == SpeedMode::Accurate ? " | Accurate" : "";
        if (m_runner && m_runner->snapshotsEnabled()) line += " | saved states";
    }
    return line;
}

float Session::progress() const {
    if (m_viewing) {
        auto end = endTick();
        return end > 0 && m_runner ? std::clamp(static_cast<float>(m_runner->ticks()) / static_cast<float>(end), 0.f, 1.f) : 0.f;
    }
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
// Replay with frame windows
// ---------------------------------------------------------------------------
bool Session::startViewer(std::string& error) {
    auto layer = PlayLayer::get();
    if (!layer) {
        error = "Open the level first (the replay is played inside the level).";
        return false;
    }
    if (!replay) {
        error = "Import a replay first.";
        return false;
    }
    if (!hasResults()) {
        error = "Measure the frame windows first (Start), then watch the replay with them.";
        return false;
    }
    if (std::abs(replay->tps - 240.0) > 0.01) {
        error = "Only 240 TPS replays are supported.";
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
        error = "An analysis is running.";
        return false;
    }
    if (m_analyzer || m_viewing || m_viewFinishing) release(false);  // keeps the results

    m_cfg = readConfig();
    bool after = m_cfg.inputPhase == 2 || (m_cfg.inputPhase == 0 && looksLikeMegaHack(replay->bot));

    m_layer = layer;
    m_runner = std::make_unique<GameRunner>(layer, after, false);

    m_viewResults = results();
    m_resultOf.assign(replay->inputs.size(), -1);
    for (std::size_t i = 0; i < m_viewResults.size(); ++i) {
        int id = m_viewResults[i].id;
        if (id >= 0 && id < static_cast<int>(m_resultOf.size())) m_resultOf[id] = static_cast<int>(i);
    }
    m_live.clear();
    m_liveBase = m_liveFull = -1.0;
    m_liveSkip = 0;
    m_markers.clear();
    m_runner->setInputCallback([this](Input const& in) { onViewerInput(in); });

    TrialRequest r;
    r.plan = replay->inputs;
    r.baseline = true;
    r.requireCompletion = true;
    r.validateUntil = std::numeric_limits<Tick>::max();
    r.timeoutTick = replay->lastInputTick() + static_cast<Tick>(60.0 * replay->tps);
    m_runner->begin(std::move(r));

    m_viewing = true;
    m_viewFinishing = false;
    m_viewPaused = false;
    m_acc = 0.0;
    ensureOverlay();

    auto size = CCDirector::sharedDirector()->getWinSize();
    auto label = CCLabelBMFont::create("L* -", "chatFont.fnt");
    label->setScale(0.55f);
    label->setAnchorPoint({0.f, 1.f});
    label->setPosition({6.f, size.height - 6.f});
    label->setZOrder(1000);
    m_layer->addChild(label);
    m_precLabel = label;
    refreshOverlay(true);
    return true;
}

void Session::runViewerFrame(float dt, std::function<void(float)> const& physicsStep) {
    constexpr double kStep = 1.0 / 240.0;
    if (m_viewFinishing || !m_runner) return;
    if (m_viewPaused) {
        m_acc = 0.0;
        return;
    }
    if (m_runner->hasTrial() && !m_runner->running()) {
        m_runner->prepareStep();  // resets the level on the first frame
        m_acc = 0.0;
    } else {
        // Real time: the replay plays at normal speed.
        m_acc = std::min(m_acc + static_cast<double>(dt), 0.25);
        for (int guard = 0; guard < 60 && m_acc >= kStep && m_runner->running(); ++guard) {
            m_acc -= kStep;
            physicsStep(static_cast<float>(kStep));
            m_runner->afterStep();
        }
    }

    // Old markers disappear.
    Tick now = m_runner->ticks();
    Tick keep = static_cast<Tick>(m_cfg.viewerKeep * 240.0);
    std::erase_if(m_markers, [&](Marker& m) {
        if (m.tick + keep >= now) return false;
        if (m.node && m.node->getParent()) m.node->removeFromParent();
        return true;
    });

    if (auto r = m_runner->poll()) {
        finishViewer(*r);
        return;
    }
    refreshOverlay(false);
}

void Session::onViewerInput(Input const& in) {
    if (!m_layer || !m_viewing) return;
    int index = (in.id >= 0 && in.id < static_cast<int>(m_resultOf.size())) ? m_resultOf[in.id] : -1;
    InputResult const* r = index >= 0 ? &m_viewResults[index] : nullptr;

    // Precision so far: every measured input that already happened.
    if (r && !r->unstable && (r->down || m_cfg.precisionReleases)) {
        m_live.push_back({static_cast<double>(r->tick), static_cast<double>(r->window())});
        updateLivePrecision();
    }

    if (!in.down && !r) return;  // releases that were not measured are not drawn
    PlayerObject* player = in.player2 ? m_layer->m_player2 : m_layer->m_player1;
    if (!player || !player->getParent()) return;

    bool press = in.down;
    ccColor3B rgb = r ? windowColor(r->window(), r->capped, r->unstable) : ccColor3B{150, 150, 150};
    float radius = press ? 7.5f : 5.f;

    auto node = CCNode::create();
    node->setPosition(player->getPosition());

    ccColor3B outlineRgb = press ? ccColor3B{0, 0, 0} : ccColor3B{255, 255, 255};
    if (auto disc = makeDisc(radius, rgb, outlineRgb, 1.6f)) {
        node->addChild(disc);
    } else {
        auto dots = CCDrawNode::create();
        ccColor4F outline = press ? ccColor4F{0.f, 0.f, 0.f, 0.9f} : ccColor4F{1.f, 1.f, 1.f, 0.95f};
        ccColor4F fill{static_cast<float>(rgb.r) / 255.f, static_cast<float>(rgb.g) / 255.f, static_cast<float>(rgb.b) / 255.f, 0.95f};
        dots->drawDot(CCPoint(0.f, 0.f), radius + 1.6f, outline);
        dots->drawDot(CCPoint(0.f, 0.f), radius, fill);
        node->addChild(dots);
    }

    std::string text = r ? fmt::format("{}{}", r->window(), r->capped ? "+" : "") : std::string("?");
    auto big = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    big->setScale(press ? 0.34f : 0.26f);
    big->setColor(rgb);
    big->setPosition({0.f, radius + 9.f});
    node->addChild(big);

    float below = -(radius + 7.f);
    if (r) {
        auto sub = CCLabelBMFont::create(fmt::format("-{}/+{}", r->early, r->late).c_str(), "chatFont.fnt");
        sub->setScale(0.42f);
        sub->setPosition({0.f, below});
        node->addChild(sub);
        below -= 9.f;
    }
    if (m_cfg.viewerTick) {
        auto frame = CCLabelBMFont::create(fmt::format("f{}", in.tick).c_str(), "chatFont.fnt");
        frame->setScale(0.38f);
        frame->setColor({190, 190, 190});
        frame->setPosition({0.f, below});
        node->addChild(frame);
    }

    player->getParent()->addChild(node, 1000);
    m_markers.push_back({node, in.tick});
    if (m_markers.size() > 300) {
        auto& oldest = m_markers.front();
        if (oldest.node && oldest.node->getParent()) oldest.node->removeFromParent();
        m_markers.erase(m_markers.begin());
    }
}

void Session::updateLivePrecision() {
    std::size_t n = m_live.size();
    if (n == 0 || !m_precLabel) return;
    // Long replays: the solver costs O(n); refresh less often (the value moves slowly then).
    if (n > 800 && (++m_liveSkip % static_cast<int>(n / 800 + 1)) != 0) return;

    PrecisionParams p = m_cfg.precision;
    p.tps = replay ? replay->tps : 240.0;
    double base = solvePrecision(m_live.data(), n, p, false, false, false, m_liveBase > 0 ? m_liveBase : 0.0, 1e-6);
    double full = solvePrecision(m_live.data(), n, p, true, true, true, m_liveFull > 0 ? m_liveFull : 0.0, 1e-6);
    if (std::isfinite(base)) m_liveBase = base;
    if (std::isfinite(full)) m_liveFull = full;
    auto show = [](double v) { return v > 0 ? fmt::format("{:.2f}", v) : std::string("n/a"); };
    m_precLabel->setString(fmt::format("L* {}  |  N+F+C {}  |  {} inputs", show(m_liveBase), show(m_liveFull), n).c_str());
}

void Session::clearMarkers() {
    for (auto& m : m_markers) {
        if (m.node && m.node->getParent()) m.node->removeFromParent();
    }
    m_markers.clear();
}

void Session::finishViewer(TrialResult const& r) {
    m_viewing = false;
    m_viewFinishing = true;
    bool ok = r.pass;
    std::string msg;
    auto show = [](double v) { return v > 0 ? fmt::format("{:.2f}", v) : std::string("n/a"); };
    if (ok) {
        msg = fmt::format("Replay finished | L* {} | N+F+C {} | {} inputs", show(m_liveBase), show(m_liveFull), m_live.size());
    } else {
        msg = fmt::format("The replay stopped: {} at tick {}.{}", toString(r.reason), r.tick,
                          r.detail.empty() ? std::string() : " " + r.detail);
    }
    queueInMainThread([ok, msg]() {
        Session::get().release(true);
        if (ok) Notification::create(msg, NotificationIcon::Success)->show();
        else FLAlertLayer::create("Replay with frame windows", msg.c_str(), "OK")->show();
    });
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
