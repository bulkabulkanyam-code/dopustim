// Frame Window Mod - in-game replay list (used on mobile, where no file dialog exists).
#pragma once
#include <Geode/Geode.hpp>

#include <filesystem>
#include <functional>
#include <vector>

namespace fwm::ui {

class ImportPopup : public geode::Popup {
public:
    static ImportPopup* create(std::function<void(std::filesystem::path const&)> onPicked);

protected:
    struct Entry {
        std::filesystem::path path;
        std::string label;   // file name
        std::string source;  // where it was found
    };

    bool init(std::function<void(std::filesystem::path const&)> onPicked);
    void scan();
    void build();
    void onPick(cocos2d::CCObject* sender);
    void onRefresh(cocos2d::CCObject*);
    void onInfo(cocos2d::CCObject*);

    std::function<void(std::filesystem::path const&)> m_onPicked;
    std::vector<Entry> m_entries;
    geode::ScrollLayer* m_scroll = nullptr;
    cocos2d::CCLabelBMFont* m_empty = nullptr;
};

}  // namespace fwm::ui
