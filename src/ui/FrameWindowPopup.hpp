// Frame Window Mod - main window (opened from the pause menu).
#pragma once
#include <Geode/Geode.hpp>

#include <string>

namespace fwm {

class FrameWindowPopup : public geode::Popup {
public:
    static void open();  // shows the window (only one instance at a time)
    ~FrameWindowPopup() override;

protected:
    static FrameWindowPopup* create();
    bool init();

    void refresh();
    void rebuildList();
    void closeAndResumeGame();
    void begin(bool verifyOnly);

    void onImport(cocos2d::CCObject*);
    void onVerify(cocos2d::CCObject*);
    void onStart(cocos2d::CCObject*);
    void onResumeAnalysis(cocos2d::CCObject*);
    void onStop(cocos2d::CCObject*);
    void onRelease(cocos2d::CCObject*);
    void onPrecision(cocos2d::CCObject*);
    void onExport(cocos2d::CCObject*);
    void onSettings(cocos2d::CCObject*);
    void onPrev(cocos2d::CCObject*);
    void onNext(cocos2d::CCObject*);
    void onHelp(cocos2d::CCObject*);
    void onError(cocos2d::CCObject*);

    cocos2d::CCMenu* m_menu = nullptr;
    cocos2d::CCLabelBMFont* m_info = nullptr;
    cocos2d::CCLabelBMFont* m_warn = nullptr;
    cocos2d::CCLabelBMFont* m_status = nullptr;
    cocos2d::CCLabelBMFont* m_summary = nullptr;
    cocos2d::CCLabelBMFont* m_pageLabel = nullptr;
    cocos2d::CCLabelBMFont* m_empty = nullptr;
    geode::ScrollLayer* m_scroll = nullptr;

    CCMenuItemSpriteExtra* m_startBtn = nullptr;
    CCMenuItemSpriteExtra* m_verifyBtn = nullptr;
    CCMenuItemSpriteExtra* m_resumeBtn = nullptr;
    CCMenuItemSpriteExtra* m_stopBtn = nullptr;
    CCMenuItemSpriteExtra* m_releaseBtn = nullptr;
    CCMenuItemSpriteExtra* m_errBtn = nullptr;

    int m_page = 0;
};

}  // namespace fwm
