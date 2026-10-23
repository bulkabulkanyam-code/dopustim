#include "ImportPopup.hpp"

#include <algorithm>

#include "FileDialogs.hpp"

using namespace geode::prelude;

namespace fwm::ui {

namespace fs = std::filesystem;

namespace {
constexpr float kW = 400.f;
constexpr float kH = 270.f;
constexpr float kListW = 360.f;
constexpr float kListH = 166.f;
constexpr int kPeek = 25;  // how many of the newest files are opened to read the level id
}  // namespace

ImportPopup* ImportPopup::create(std::function<void(fs::path const&)> onPicked) {
    auto ret = new ImportPopup();
    if (ret->init(std::move(onPicked))) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool ImportPopup::init(std::function<void(fs::path const&)> onPicked) {
    if (!Popup::init(kW, kH)) return false;
    this->setTitle("Select a replay");
    m_onPicked = std::move(onPicked);

    auto size = m_mainLayer->getContentSize();

    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(menu);

    auto infoSpr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
    infoSpr->setScale(0.6f);
    auto infoBtn = CCMenuItemSpriteExtra::create(infoSpr, this, menu_selector(ImportPopup::onInfo));
    infoBtn->setPosition({size.width - 24.f, size.height - 24.f});
    menu->addChild(infoBtn);

    auto refreshSpr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png");
    refreshSpr->setScale(0.6f);
    auto refreshBtn = CCMenuItemSpriteExtra::create(refreshSpr, this, menu_selector(ImportPopup::onRefresh));
    refreshBtn->setPosition({size.width - 54.f, size.height - 24.f});
    menu->addChild(refreshBtn);

#if !defined(GEODE_IS_MOBILE)
    auto browseSpr = ButtonSprite::create("Browse...", "bigFont.fnt", "GJ_button_04.png", 0.8f);
    browseSpr->setScale(0.55f);
    auto browseBtn = CCMenuItemSpriteExtra::create(browseSpr, this, menu_selector(ImportPopup::onBrowse));
    browseBtn->setPosition({size.width / 2.f, 20.f});
    menu->addChild(browseBtn);
#endif

    float listY = 40.f;
    m_scroll = ScrollLayer::create({kListW, kListH});
    m_scroll->setPosition({(size.width - kListW) / 2.f, listY});
    m_mainLayer->addChild(m_scroll);

    m_empty = CCLabelBMFont::create(
        "No replays found.\nSave a macro in GDR format with Eclipse,\nMega Hack or xdBot, or press the info button.",
        "bigFont.fnt");
    m_empty->setScale(0.3f);
    m_empty->setAlignment(kCCTextAlignmentCenter);
    m_empty->setPosition({size.width / 2.f, listY + kListH / 2.f});
    m_empty->setVisible(false);
    m_mainLayer->addChild(m_empty);

    scan();
    build();
    return true;
}

void ImportPopup::scan() {
    m_entries.clear();
    auto found = findBotReplays();

    std::uint32_t openLevel = 0;
    if (auto layer = PlayLayer::get()) openLevel = static_cast<std::uint32_t>(layer->m_level->m_levelID.value());

    int peeked = 0;
    for (auto& f : found) {
        Entry e;
        e.file = std::move(f);
        if (peeked++ < kPeek) {
            e.info = peekReplay(e.file.path);
            e.thisLevel = openLevel != 0 && e.info.ok && e.info.levelId == openLevel;
        }
        m_entries.push_back(std::move(e));
    }
    // Replays of the open level first, otherwise newest first (the list is already sorted by time).
    std::stable_sort(m_entries.begin(), m_entries.end(),
                     [](Entry const& a, Entry const& b) { return a.thisLevel && !b.thisLevel; });
}

void ImportPopup::build() {
    if (!m_scroll || !m_scroll->m_contentLayer) return;
    m_scroll->m_contentLayer->removeAllChildrenWithCleanup(true);

    if (m_entries.empty()) {
        m_empty->setVisible(true);
        return;
    }
    m_empty->setVisible(false);

    float rowH = 34.f;
    float contentH = std::max(kListH, rowH * static_cast<float>(m_entries.size()) + 8.f);
    m_scroll->m_contentLayer->setContentSize({kListW, contentH});

    float y = contentH - 4.f - rowH / 2.f;
    for (auto const& entry : m_entries) {
        auto bg = CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({352.f, 30.f});
        bg->setOpacity(entry.thisLevel ? 120 : 70);
        if (entry.thisLevel) bg->setColor({60, 160, 90});
        bg->setPosition({kListW / 2.f, y});
        m_scroll->m_contentLayer->addChild(bg);

        std::string name = entry.file.path.filename().string();
        if (name.size() > 34) name = name.substr(0, 31) + "...";
        auto nameLbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
        nameLbl->setScale(0.6f);
        nameLbl->setAnchorPoint({0.f, 0.5f});
        nameLbl->setPosition({14.f, y + 5.f});
        m_scroll->m_contentLayer->addChild(nameLbl);

        std::string detail = fmt::format("{}  |  {}", entry.file.source, ageText(entry.file.time));
        if (entry.info.ok) detail += fmt::format("  |  {} inputs", entry.info.inputs);
        if (entry.thisLevel) detail += "  |  this level";
        auto srcLbl = CCLabelBMFont::create(detail.c_str(), "chatFont.fnt");
        srcLbl->setScale(0.42f);
        srcLbl->setAnchorPoint({0.f, 0.5f});
        srcLbl->setColor(entry.thisLevel ? ccColor3B{150, 255, 170} : ccColor3B{160, 160, 160});
        srcLbl->setPosition({14.f, y - 8.f});
        m_scroll->m_contentLayer->addChild(srcLbl);

        auto rowMenu = CCMenu::create();
        rowMenu->setPosition({0.f, 0.f});
        rowMenu->setContentSize({kListW, contentH});
        m_scroll->m_contentLayer->addChild(rowMenu);

        auto btnSpr = ButtonSprite::create("Load");
        btnSpr->setScale(0.5f);
        auto btn = CCMenuItemSpriteExtra::create(btnSpr, this, menu_selector(ImportPopup::onPick));
        btn->setUserObject(CCString::create(entry.file.path.string()));
        btn->setPosition({kListW - 36.f, y});
        rowMenu->addChild(btn);

        y -= rowH;
    }
    m_scroll->moveToTop();
}

void ImportPopup::onPick(CCObject* sender) {
    auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(sender);
    if (!btn) return;
    auto str = static_cast<CCString*>(btn->getUserObject());
    if (!str) return;
    fs::path path = str->getCString();
    auto cb = m_onPicked;
    this->onClose(nullptr);
    if (cb) cb(path);
}

void ImportPopup::onRefresh(CCObject*) {
    scan();
    build();
}

void ImportPopup::onBrowse(CCObject*) {
    auto cb = m_onPicked;
    this->onClose(nullptr);
    browseReplay([cb](fs::path const& path) {
        if (cb) cb(path);
    });
}

void ImportPopup::onInfo(CCObject*) {
    FLAlertLayer::create(
        "Where replays are found",
        fmt::format("The list shows .gdr / .gdr2 macros saved by <cy>Eclipse</c>, <cy>Mega Hack</c> and <cy>xdBot</c> "
                    "(their folders under geode/config, geode/mods and the game folder), newest first. "
                    "Replays recorded on the open level are on top.\n\n"
                    "Other files: copy them into\n<cy>{}</c>\nor use Browse (desktop).\n\n"
                    "Save macros in <cg>GDR format</c>; native .xd / .mhr files cannot be read.",
                    importsDir().string())
            .c_str(),
        "OK")
        ->show();
}

}  // namespace fwm::ui
