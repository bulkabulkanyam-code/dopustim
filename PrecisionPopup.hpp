// Frame Window Mod - Precision (L*) window.
#pragma once
#include <Geode/Geode.hpp>

#include <array>
#include <string>

namespace fwm {

class PrecisionPopup : public geode::Popup {
public:
    static void open();
    ~PrecisionPopup() override;

protected:
    static PrecisionPopup* create();
    bool init();
    void calculate();
    void onRecalc(cocos2d::CCObject*);
    void onSettings(cocos2d::CCObject*);
    void onCopy(cocos2d::CCObject*);

    cocos2d::CCLabelBMFont* m_header = nullptr;
    cocos2d::CCLabelBMFont* m_note = nullptr;
    std::array<cocos2d::CCLabelBMFont*, 8> m_values{};
    std::string m_clipboard;
};

}  // namespace fwm
