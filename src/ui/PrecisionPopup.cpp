#include "PrecisionPopup.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>

#include <cmath>

#include "../core/Export.hpp"
#include "../core/Precision.hpp"
#include "../game/Session.hpp"

using namespace geode::prelude;

namespace fwm {

namespace {
PrecisionPopup* s_instance = nullptr;

std::string formatDuration(double seconds) {
    if (seconds >= 3600.0 && std::fmod(seconds, 3600.0) == 0.0) return fmt::format("{:.0f} h", seconds / 3600.0);
    if (seconds >= 3600.0) return fmt::format("{:.2f} h", seconds / 3600.0);
    return fmt::format("{:.0f} s", seconds);
}
}  // namespace

PrecisionPopup::~PrecisionPopup() {
    if (s_instance == this) s_instance = nullptr;
}

void PrecisionPopup::open() {
    if (s_instance) return;
    if (auto popup = create()) {
        s_instance = popup;
        popup->show();
    }
}

PrecisionPopup* PrecisionPopup::create() {
    auto ret = new PrecisionPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool PrecisionPopup::init() {
    if (!Popup::init(360.f, 290.f)) return false;
    this->setTitle("Precision (L*)");

    auto size = m_mainLayer->getContentSize();
    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(menu);

    m_header = CCLabelBMFont::create("", "chatFont.fnt");
    m_header->setScale(0.5f);
    m_header->setColor({200, 200, 200});
    m_header->setPosition({size.width / 2, 246.f});
    m_mainLayer->addChild(m_header);

    auto bg = CCScale9Sprite::create("square02_small.png");
    bg->setContentSize({320.f, 170.f});
    bg->setOpacity(70);
    bg->setPosition({size.width / 2, 148.f});
    m_mainLayer->addChild(bg);

    for (int i = 0; i < kVariantCount; ++i) {
        float y = 216.f - 20.f * static_cast<float>(i);
        auto name = CCLabelBMFont::create(variantName(static_cast<PVariant>(i)), "chatFont.fnt");
        name->setScale(0.62f);
        name->setAnchorPoint({0.f, 0.5f});
        name->setPosition({32.f, y});
        m_mainLayer->addChild(name);

        auto value = CCLabelBMFont::create("", "chatFont.fnt");
        value->setScale(0.7f);
        value->setAnchorPoint({1.f, 0.5f});
        value->setPosition({size.width - 32.f, y});
        m_mainLayer->addChild(value);
        m_values[i] = value;
    }

    m_note = CCLabelBMFont::create("", "chatFont.fnt");
    m_note->setScale(0.42f);
    m_note->setColor({255, 170, 70});
    m_note->setAlignment(kCCTextAlignmentCenter);
    m_note->setPosition({size.width / 2, 52.f});
    m_mainLayer->addChild(m_note);

    auto addButton = [&](char const* text, float x, SEL_MenuHandler cb, char const* texture) {
        auto spr = ButtonSprite::create(text, "bigFont.fnt", texture, 0.8f);
        spr->setScale(0.5f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        btn->setPosition({x, 22.f});
        menu->addChild(btn);
    };
    addButton("Recalculate", 70.f, menu_selector(PrecisionPopup::onRecalc), "GJ_button_01.png");
    addButton("Copy", 160.f, menu_selector(PrecisionPopup::onCopy), "GJ_button_02.png");
    addButton("Model settings", 262.f, menu_selector(PrecisionPopup::onSettings), "GJ_button_04.png");

    calculate();
    return true;
}

void PrecisionPopup::calculate() {
    auto& session = Session::get();
    auto cfg = readConfig();
    int used = 0;
    auto result = session.computePrecision(&used);

    double tps = session.replay ? session.replay->tps : 240.0;
    m_header->setString(fmt::format("{} inputs  |  target {}  |  respawn {:.1f} s  |  {:.0f} TPS{}", used,
                                    formatDuration(cfg.precision.targetSeconds), cfg.precision.respawnSeconds, tps,
                                    cfg.precisionReleases ? "  |  releases included" : "")
                            .c_str());

    m_clipboard.clear();
    for (int i = 0; i < kVariantCount; ++i) {
        std::string text = result.ok[i] ? fmt::format("{:.4f}", result.value[i]) : std::string("n/a");
        m_values[i]->setString(text.c_str());
        m_values[i]->setColor(result.ok[i] ? ccColor3B{130, 235, 140} : ccColor3B{200, 120, 120});
        m_clipboard += fmt::format("{}: {}\n", variantName(static_cast<PVariant>(i)), text);
    }

    auto stats = computeStats(session.results());
    std::string note;
    if (used == 0) note = "No usable inputs. Enable presses/releases in the settings and measure again.";
    else if (stats.capped > 0)
        note = fmt::format("{} windows hit the search limit (lower bounds), so L* may be a bit high.\n"
                           "Raise \"Max window\" in the settings to measure them fully.", stats.capped);
    else if (stats.unstable > 0)
        note = fmt::format("{} unstable inputs (repeated trials disagreed) are not included.", stats.unstable);
    else
        note = "L* = sigma-precision needed to clear the level in the target time (NaNDL model).";
    m_note->setString(note.c_str());
}

void PrecisionPopup::onRecalc(CCObject*) { calculate(); }

void PrecisionPopup::onSettings(CCObject*) { openSettingsPopup(Mod::get(), false); }

void PrecisionPopup::onCopy(CCObject*) {
    geode::utils::clipboard::write(m_clipboard);
    Notification::create("Copied", NotificationIcon::Success)->show();
}

}  // namespace fwm
