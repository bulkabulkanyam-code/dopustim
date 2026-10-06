#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include "../ui/FrameWindowPopup.hpp"
#include "Session.hpp"

using namespace geode::prelude;
using namespace fwm;

// ---------------------------------------------------------------------------
// PlayLayer: death / completion / restart are the trial outcomes
// ---------------------------------------------------------------------------
class $modify(FWMPlayLayer, PlayLayer) {
    struct Fields {
        PlayLayer* self = nullptr;
        ~Fields() {
            if (self) Session::get().layerGone(self);
        }
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_fields->self = this;
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto runner = Session::get().runnerFor(this);
        if (!runner) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        // GD's internal anti-cheat collision probe also goes through here; the
        // original handles it without killing the player.
        if (object && m_anticheatSpike && object->m_uniqueID == m_anticheatSpike->m_uniqueID) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        // A real collision is the failure signal of the trial: skip the death pipeline.
        runner->onDeath();
    }

    void levelComplete() {
        auto runner = Session::get().runnerFor(this);
        if (!runner) {
            PlayLayer::levelComplete();
            return;
        }
        runner->onComplete();
    }

    void resetLevel() {
        auto runner = Session::get().runnerFor(this);
        if (!runner) {
            PlayLayer::resetLevel();
            return;
        }
        if (runner->resetting()) {
            PlayLayer::resetLevel();
        } else {
            runner->onExternalReset();  // ignore restarts while the analysis owns the level
        }
    }

    void updateAttempts() {
        if (Session::get().runnerFor(this)) return;
        PlayLayer::updateAttempts();
    }

    void onQuit() {
        Session::get().layerGone(this);
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// GJBaseGameLayer: fixed-step simulation and input injection
// ---------------------------------------------------------------------------
class $modify(FWMBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        if (!Session::get().runnerFor(this)) {
            GJBaseGameLayer::update(dt);
            return;
        }
        // The analysis owns the simulation: step it ourselves at exactly 1/240 s.
        Session::get().runFrame(dt, [this](float step) { GJBaseGameLayer::update(step); });
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto runner = Session::get().runnerFor(this);
        if (!runner || !runner->running()) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        runner->beforeCommands(isHalfTick);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        runner->afterCommands(isHalfTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto runner = Session::get().runnerFor(this);
        // While the analysis owns the level, real user input is ignored.
        if (runner && !runner->injecting()) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// ---------------------------------------------------------------------------
// Keep analysis trials out of the player's saved stats
// ---------------------------------------------------------------------------
class $modify(FWMLevel, GJGameLevel) {
    void savePercentage(int percent, bool practice, int clicks, int attempts, bool checkpointValid) {
        if (Session::get().bound()) return;
        GJGameLevel::savePercentage(percent, practice, clicks, attempts, checkpointValid);
    }
};

class $modify(FWMPlayer, PlayerObject) {
    void incrementJumps() {
        if (Session::get().bound()) return;
        PlayerObject::incrementJumps();
    }
};

// ---------------------------------------------------------------------------
// Pause menu entry
// ---------------------------------------------------------------------------
class $modify(FWMPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        Session::get().onPauseOpened();

        auto spr = CCSprite::createWithSpriteFrameName("GJ_timeIcon_001.png");
        spr->setScale(0.8f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FWMPauseLayer::onFrameWindows));
        btn->setID("frame-window-button"_spr);

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            // Fallback when another mod changed the layout of the pause menu.
            auto size = CCDirector::sharedDirector()->getWinSize();
            auto menu2 = CCMenu::create();
            menu2->setPosition({size.width - 28.f, 28.f});
            menu2->addChild(btn);
            btn->setPosition({0.f, 0.f});
            this->addChild(menu2, 100);
        }
    }

    void onResume(CCObject* sender) {
        // The native "play" button also continues an analysis that was paused by opening the menu.
        if (Session::get().state() == State::Paused) Session::get().resumeAnalysis();
        PauseLayer::onResume(sender);
    }

    void onFrameWindows(CCObject*) {
        FrameWindowPopup::open();
    }
};
