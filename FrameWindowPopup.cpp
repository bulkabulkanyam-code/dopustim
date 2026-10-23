#include "FrameWindowPopup.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/binding/PauseLayer.hpp>

#include <algorithm>

#include "../core/Export.hpp"
#include "../game/Session.hpp"
#include "FileDialogs.hpp"
#include "PrecisionPopup.hpp"
#include "WindowColor.hpp"

using namespace geode::prelude;

namespace fwm {

namespace {

constexpr float kW = 440.f;
constexpr float kH = 290.f;
constexpr float kRowH = 16.f;
constexpr int kPageSize = 60;

FrameWindowPopup* s_instance = nullptr;

}  // namespace

FrameWindowPopup::~FrameWindowPopup() {
    if (s_instance == this) s_instance = nullptr;
}

void FrameWindowPopup::open() {
    if (s_instance) return;
    if (auto popup = create()) {
        s_instance = popup;
        popup->show();
    }
}

FrameWindowPopup* FrameWindowPopup::create() {
    auto ret = new FrameWindowPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool FrameWindowPopup::init() {
    if (!Popup::init(kW, kH)) return false;
    this->setTitle("Frame Windows");

    m_menu = CCMenu::create();
    m_menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(m_menu);

    auto addLabel = [&](char const* font, float scale, CCPoint pos, CCPoint anchor, ccColor3B color) {
        auto label = CCLabelBMFont::create("", font);
        label->setScale(scale);
        label->setAnchorPoint(anchor);
        label->setPosition(pos);
        label->setColor(color);
        m_mainLayer->addChild(label);
        return label;
    };
    auto addButton = [&](char const* text, float x, float y, float scale, SEL_MenuHandler cb,
                         char const* texture = "GJ_button_01.png") {
        auto spr = ButtonSprite::create(text, "bigFont.fnt", texture, 0.8f);
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        btn->setPosition({x, y});
        m_menu->addChild(btn);
        return btn;
    };

    // --- header ---
    m_info = addLabel("chatFont.fnt", 0.5f, {kW / 2, 246.f}, {0.5f, 0.5f}, {220, 220, 220});
    m_warn = addLabel("chatFont.fnt", 0.45f, {kW / 2, 234.f}, {0.5f, 0.5f}, {255, 170, 70});

    // help button (top right)
    auto helpSpr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
    helpSpr->setScale(0.6f);
    auto helpBtn = CCMenuItemSpriteExtra::create(helpSpr, this, menu_selector(FrameWindowPopup::onHelp));
    helpBtn->setPosition({kW - 22.f, kH - 22.f});
    m_menu->addChild(helpBtn);

    // --- action buttons ---
    float by = 212.f;
    addButton("Import", 38.f, by, 0.55f, menu_selector(FrameWindowPopup::onImport), "GJ_button_04.png");
    addButton("Auto", 104.f, by, 0.55f, menu_selector(FrameWindowPopup::onAuto), "GJ_button_03.png");
    m_verifyBtn = addButton("Verify", 170.f, by, 0.55f, menu_selector(FrameWindowPopup::onVerify), "GJ_button_02.png");
    m_startBtn = addButton("Start", 236.f, by, 0.55f, menu_selector(FrameWindowPopup::onStart));
    m_resumeBtn = addButton("Resume", 236.f, by, 0.55f, menu_selector(FrameWindowPopup::onResumeAnalysis));
    m_stopBtn = addButton("Stop", 302.f, by, 0.55f, menu_selector(FrameWindowPopup::onStop), "GJ_button_06.png");
    m_releaseBtn = addButton("Release", 368.f, by, 0.55f, menu_selector(FrameWindowPopup::onRelease), "GJ_button_05.png");

    auto errSpr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
    errSpr->setScale(0.6f);
    errSpr->setColor({255, 90, 90});
    m_errBtn = CCMenuItemSpriteExtra::create(errSpr, this, menu_selector(FrameWindowPopup::onError));
    m_errBtn->setPosition({414.f, by + 16.f});
    m_menu->addChild(m_errBtn);

    // --- status / summary / table header ---
    m_status = addLabel("chatFont.fnt", 0.5f, {kW / 2, 192.f}, {0.5f, 0.5f}, {255, 255, 255});
    m_summary = addLabel("chatFont.fnt", 0.5f, {kW / 2, 178.f}, {0.5f, 0.5f}, {160, 220, 255});

    struct Col {
        char const* text;
        float x;
    };
    for (auto col : {Col{"#", 24.f}, Col{"Tick", 62.f}, Col{"Type", 112.f}, Col{"P", 156.f}, Col{"Window", 182.f},
                     Col{"Early / Late", 232.f}, Col{"Dies after", 296.f}, Col{"Notes", 366.f}}) {
        auto h = addLabel("chatFont.fnt", 0.5f, {col.x, 164.f}, {0.f, 0.5f}, {255, 215, 120});
        h->setString(col.text);
    }

    // --- list ---
    auto bg = CCScale9Sprite::create("square02_small.png");
    bg->setContentSize({404.f, 118.f});
    bg->setOpacity(70);
    bg->setPosition({kW / 2, 98.f});
    m_mainLayer->addChild(bg);

    m_scroll = ScrollLayer::create({400.f, 114.f});
    m_scroll->setPosition({20.f, 41.f});
    m_mainLayer->addChild(m_scroll);

    m_empty = addLabel("chatFont.fnt", 0.55f, {kW / 2, 98.f}, {0.5f, 0.5f}, {170, 170, 170});
    m_empty->setAlignment(kCCTextAlignmentCenter);

    // --- bottom row ---
    float y = 20.f;
    addButton("<", 36.f, y, 0.5f, menu_selector(FrameWindowPopup::onPrev), "GJ_button_04.png");
    m_pageLabel = addLabel("chatFont.fnt", 0.5f, {92.f, y}, {0.5f, 0.5f}, {200, 200, 200});
    addButton(">", 148.f, y, 0.5f, menu_selector(FrameWindowPopup::onNext), "GJ_button_04.png");
    addButton("L* Precision", 220.f, y, 0.5f, menu_selector(FrameWindowPopup::onPrecision), "GJ_button_02.png");
    addButton("Watch", 290.f, y, 0.5f, menu_selector(FrameWindowPopup::onWatch), "GJ_button_03.png");
    addButton("Export", 345.f, y, 0.5f, menu_selector(FrameWindowPopup::onExport));
    addButton("Settings", 405.f, y, 0.5f, menu_selector(FrameWindowPopup::onSettings), "GJ_button_04.png");

    auto credit = addLabel("chatFont.fnt", 0.4f, {kW / 2, 6.f}, {0.5f, 0.5f}, {120, 120, 120});
    credit->setString("Frame Window Mod by Seby");

    refresh();
    return true;
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
void FrameWindowPopup::refresh() {
    auto& s = Session::get();
    auto state = s.state();
    bool exists = s.analyzerExists();

    // header
    if (s.replay) {
        int presses = 0;
        for (auto const& in : s.replay->inputs) presses += in.down ? 1 : 0;
        std::string name = s.replay->name;
        if (name.size() > 34) name = name.substr(0, 31) + "...";
        m_info->setString(fmt::format("{}  |  {} inputs ({} presses)  |  {:.0f} TPS{}", name, s.replay->inputs.size(),
                                      presses, s.replay->tps,
                                      s.replay->bot.empty() ? "" : "  |  " + s.replay->bot)
                              .c_str());
    } else {
        m_info->setString("No replay loaded - tap Import");
    }
    {
        auto warn = s.replayWarning();
        auto note = s.snapshotNote();
        if (!warn.empty()) {
            m_warn->setColor({255, 170, 70});
            m_warn->setString(warn.c_str());
        } else {
            if (note.size() > 110) note = note.substr(0, 107) + "...";
            m_warn->setColor({140, 190, 255});
            m_warn->setString(note.c_str());
        }
    }

    // status
    std::string status = exists ? s.status() : (s.hasResults() ? "Previous results (analysis released)" : "Ready");
    if (status.size() > 96) status = status.substr(0, 93) + "...";
    m_status->setString(status.c_str());
    m_status->setColor(state == State::Failed ? ccColor3B{255, 110, 110} : ccColor3B{255, 255, 255});

    // buttons
    bool running = state == State::Verifying || state == State::Running;
    bool paused = state == State::Paused;
    bool finished = state == State::Completed || state == State::Failed || state == State::Stopped;
    (void)finished;
    // Start / Verify stay tappable: Session::start() explains what is missing (no replay, no level, ...).
    m_startBtn->setVisible(!paused);
    m_verifyBtn->setVisible(!paused);
    m_resumeBtn->setVisible(paused);
    m_resumeBtn->setEnabled(paused);
    m_stopBtn->setVisible(running || paused);
    m_stopBtn->setEnabled(running || paused);
    m_releaseBtn->setVisible(exists);
    m_releaseBtn->setEnabled(exists);
    m_errBtn->setVisible(state == State::Failed);
    m_errBtn->setEnabled(state == State::Failed);

    // summary
    auto const& results = s.results();
    auto stats = computeStats(results);
    if (stats.count > 0) {
        m_summary->setString(fmt::format("{} inputs  |  avg {:.2f}  |  median {:.1f}  |  tightest {} (#{}){}{}",
                                         stats.count, stats.average, stats.median, stats.tightest, stats.tightestNumber,
                                         stats.capped ? fmt::format("  |  {} capped", stats.capped) : "",
                                         stats.unstable ? fmt::format("  |  {} unstable", stats.unstable) : "")
                                 .c_str());
    } else {
        m_summary->setString("No results yet");
    }

    rebuildList();
}

void FrameWindowPopup::rebuildList() {
    auto const& results = Session::get().results();
    int total = static_cast<int>(results.size());
    int pages = std::max(1, (total + kPageSize - 1) / kPageSize);
    m_page = std::clamp(m_page, 0, pages - 1);
    m_pageLabel->setString(fmt::format("{}/{}", m_page + 1, pages).c_str());

    m_scroll->m_contentLayer->removeAllChildrenWithCleanup(true);
    m_empty->setString(Session::get().replay ? "Press Start to measure every input.\n(Verify checks the replay first.)"
                                             : "Import a successful replay\n(.gdr / .gdr2) to begin.");
    m_empty->setVisible(total == 0);
    if (total == 0) {
        m_scroll->m_contentLayer->setContentSize({400.f, 114.f});
        return;
    }

    int first = m_page * kPageSize;
    int count = std::min(kPageSize, total - first);
    float contentH = std::max(114.f, kRowH * static_cast<float>(count) + 8.f);
    m_scroll->m_contentLayer->setContentSize({400.f, contentH});

    auto add = [&](std::string const& text, float x, float y, ccColor3B color) {
        auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        label->setScale(0.5f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({x, y});
        label->setColor(color);
        m_scroll->m_contentLayer->addChild(label);
    };

    for (int i = 0; i < count; ++i) {
        auto const& r = results[first + i];
        float y = contentH - 8.f - kRowH * static_cast<float>(i);
        ccColor3B white{235, 235, 235};
        add(std::to_string(r.id + 1), 4.f, y, white);
        add(std::to_string(r.tick), 42.f, y, white);
        add(r.down ? "press" : "release", 92.f, y, white);
        add(r.player2 ? "2" : "1", 136.f, y, white);
        std::string w = std::to_string(r.window()) + (r.capped ? "+" : "");
        add(w, 162.f, y, windowColor(r.window(), r.capped, r.unstable));
        add(fmt::format("-{} / +{}", r.early, r.late), 212.f, y, white);
        // how many ticks after the moved input the player died (earlier side / later side)
        auto dies = [](int v) { return v >= 0 ? std::to_string(v) : std::string("-"); };
        add(fmt::format("{} / {}", dies(r.earlyDeath), dies(r.lateDeath)), 276.f, y, {170, 170, 170});
        std::string notes;
        if (r.unstable) notes += "unstable ";
        else if (r.capped) notes += "capped ";
        if (r.holes > 0) notes += fmt::format("holes +{} ", r.holes);
        if (r.earlyBound || r.lateBound) notes += "bounded";
        add(notes, 346.f, y, {170, 170, 170});
    }
    m_scroll->moveToTop();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void FrameWindowPopup::closeAndResumeGame() {
    // Close this window and the pause menu so the analysis can run.
    this->onClose(nullptr);
    if (auto scene = CCDirector::sharedDirector()->getRunningScene()) {
        if (auto pause = scene->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
    }
}

void FrameWindowPopup::begin(bool verifyOnly) {
    std::string error;
    if (!Session::get().start(verifyOnly, error)) {
        FLAlertLayer::create("Cannot start", error.c_str(), "OK")->show();
        return;
    }
    closeAndResumeGame();
}

void FrameWindowPopup::onImport(CCObject*) {
    if (Session::get().analysisActive()) return;
    Ref<FrameWindowPopup> self(this);
    ui::pickReplay([self](std::filesystem::path const& path) {
        try {
            Session::get().loadReplay(path);
        } catch (std::exception const& e) {
            FLAlertLayer::create("Import failed", e.what(), "OK")->show();
        }
        self->m_page = 0;
        self->refresh();
    });
}

void FrameWindowPopup::onAuto(CCObject*) {
    if (Session::get().analysisActive()) return;
    try {
        std::string note;
        auto path = Session::get().autoImport(note);
        Notification::create("Loaded " + path.filename().string(), NotificationIcon::Success)->show();
        FLAlertLayer::create("Replay imported",
                             fmt::format("<cg>{}</c>\n{}", path.filename().string(), note).c_str(), "OK")
            ->show();
    } catch (std::exception const& e) {
        FLAlertLayer::create("Auto import", e.what(), "OK")->show();
    }
    m_page = 0;
    refresh();
}

void FrameWindowPopup::onWatch(CCObject*) {
    std::string error;
    if (!Session::get().startViewer(error)) {
        FLAlertLayer::create("Cannot start", error.c_str(), "OK")->show();
        return;
    }
    closeAndResumeGame();
}

void FrameWindowPopup::onVerify(CCObject*) { begin(true); }
void FrameWindowPopup::onStart(CCObject*) { begin(false); }

void FrameWindowPopup::onResumeAnalysis(CCObject*) {
    Session::get().resumeAnalysis();
    closeAndResumeGame();
}

void FrameWindowPopup::onStop(CCObject*) {
    Session::get().stopAnalysis();
    refresh();
}

void FrameWindowPopup::onRelease(CCObject*) {
    Session::get().release(false);
    Notification::create("Level released - press Restart to play normally", NotificationIcon::Info)->show();
    refresh();
}

void FrameWindowPopup::onPrecision(CCObject*) {
    if (!Session::get().hasResults()) {
        FLAlertLayer::create("Precision", "Measure frame windows first (Start).", "OK")->show();
        return;
    }
    PrecisionPopup::open();
}

void FrameWindowPopup::onExport(CCObject*) {
    auto& s = Session::get();
    if (!s.hasResults()) {
        FLAlertLayer::create("Export", "Nothing to export yet.", "OK")->show();
        return;
    }
    std::string base;
    auto dir = s.exportAll(base);
    FLAlertLayer::create("Exported",
                         fmt::format("Saved <cg>{}</c>\n(.json, .fwc, .nandl.json, .csv)\n\nFolder:\n<cy>{}</c>\n\n"
                                     ".fwc opens in Frame Window Counter, .nandl.json in the NaNDL calculator.",
                                     base, dir.string())
                             .c_str(),
                         "OK")
        ->show();
}

void FrameWindowPopup::onSettings(CCObject*) {
    openSettingsPopup(Mod::get(), false);
}

void FrameWindowPopup::onPrev(CCObject*) {
    --m_page;
    rebuildList();
}

void FrameWindowPopup::onNext(CCObject*) {
    ++m_page;
    rebuildList();
}

void FrameWindowPopup::onError(CCObject*) {
    auto err = Session::get().analyzerError();
    if (!err.empty()) FLAlertLayer::create("Analysis failed", err.c_str(), "OK")->show();
}

void FrameWindowPopup::onHelp(CCObject*) {
    FLAlertLayer::create(
        "How it works",
        "1. Open the level in <cy>normal mode from 0%</c> (no noclip, no other bots / TPS mods).\n"
        "2. Pause, open this window, <cg>Import</c> a replay (or <cg>Auto</c>: the newest macro of this level from "
        "Eclipse / Mega Hack / xdBot).\n"
        "3. <cg>Verify</c> checks that the replay completes the level, <cg>Start</c> measures every input.\n"
        "4. For each input the mod moves only that input earlier / later by 1, 2, 3... ticks and checks "
        "with the real game physics that the run still works. The search stops at the first shift that fails.\n"
        "5. <cg>Watch</c> plays the replay with a coloured circle on every click (red = 1 frame ... green = wide) and "
        "the precision so far.\n\n"
        "<cy>Window</c> = number of consecutive physics ticks (240/s) that work. "
        "<cy>Dies after</c> = ticks after the moved input at which the failing shift died (earlier / later side). "
        "<cy>L* Precision</c> turns the windows into the NaNDL precision value.",
        "OK")
        ->show();
}

}  // namespace fwm
