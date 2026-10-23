// Frame Window Mod - replay list: macros of Eclipse, Mega Hack and xdBot, newest first.
#pragma once
#include <Geode/Geode.hpp>

#include <filesystem>
#include <functional>
#include <vector>

#include "../game/ReplayImport.hpp"

namespace fwm::ui {

class ImportPopup : public geode::Popup {
public:
    static ImportPopup* create(std::function<void(std::filesystem::path const&)> onPicked);

protected:
    struct Entry {
        FoundReplay file;
        ReplayInfo info;      // filled for the newest files only
        bool thisLevel = false;
    };

    bool init(std::function<void(std::filesystem::path const&)> onPicked);
    void scan();
    void build();
    void onPick(cocos2d::CCObject* sender);
    void onRefresh(cocos2d::CCObject*);
    void onInfo(cocos2d::CCObject*);
    void onBrowse(cocos2d::CCObject*);

    std::function<void(std::filesystem::path const&)> m_onPicked;
    std::vector<Entry> m_entries;
    geode::ScrollLayer* m_scroll = nullptr;
    cocos2d::CCLabelBMFont* m_empty = nullptr;
};

}  // namespace fwm::ui
