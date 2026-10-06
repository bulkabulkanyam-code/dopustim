#include "ImportPopup.hpp"

#include <algorithm>
#include <cctype>

using namespace geode::prelude;

namespace fwm::ui {

namespace fs = std::filesystem;

static std::string lowerExt(fs::path const& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

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
    if (!Popup::init(380.f, 250.f)) return false;
    this->setTitle("Select a replay (.gdr / .gdr2)");
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

    m_scroll = ScrollLayer::create({340.f, 170.f});
    m_scroll->setPosition({(size.width - 340.f) / 2.f, 24.f});
    m_mainLayer->addChild(m_scroll);

    m_empty = CCLabelBMFont::create("No replays found.\nCopy .gdr / .gdr2 files into the\n'imports' folder (tap the info button).",
                                    "bigFont.fnt");
    m_empty->setScale(0.32f);
    m_empty->setAlignment(kCCTextAlignmentCenter);
    m_empty->setPosition({size.width / 2.f, 110.f});
    m_empty->setVisible(false);
    m_mainLayer->addChild(m_empty);

    scan();
    build();
    return true;
}

void ImportPopup::scan() {
    m_entries.clear();
    std::error_code ec;

    auto addDir = [&](fs::path const& dir, std::string const& source) {
        if (!fs::is_directory(dir, ec)) return;
        for (auto const& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            auto ext = lowerExt(e.path());
            if (ext != ".gdr" && ext != ".gdr2") continue;
            m_entries.push_back({e.path(), e.path().filename().string(), source});
        }
    };

    auto imports = Mod::get()->getSaveDir() / "imports";
    fs::create_directories(imports, ec);
    addDir(imports, "imports");

    // Macro folders of other bots: geode/config/<mod-id>/{macros,replays}
    auto cfg = dirs::getModConfigDir();
    if (fs::is_directory(cfg, ec)) {
        for (auto const& modDir : fs::directory_iterator(cfg, ec)) {
            if (!modDir.is_directory(ec)) continue;
            for (char const* sub : {"macros", "replays", "replay"}) {
                addDir(modDir.path() / sub, modDir.path().filename().string());
            }
        }
    }

    std::sort(m_entries.begin(), m_entries.end(), [](Entry const& a, Entry const& b) {
        auto la = a.label, lb = b.label;
        std::transform(la.begin(), la.end(), la.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lb.begin(), lb.end(), lb.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return la < lb;
    });
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
    float contentH = std::max(170.f, rowH * static_cast<float>(m_entries.size()) + 8.f);
    m_scroll->m_contentLayer->setContentSize({340.f, contentH});

    float y = contentH - 4.f - rowH / 2.f;
    for (auto const& entry : m_entries) {
        auto bg = CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({332.f, 30.f});
        bg->setOpacity(80);
        bg->setPosition({170.f, y});
        m_scroll->m_contentLayer->addChild(bg);

        std::string name = entry.label;
        if (name.size() > 30) name = name.substr(0, 27) + "...";
        auto nameLbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
        nameLbl->setScale(0.6f);
        nameLbl->setAnchorPoint({0.f, 0.5f});
        nameLbl->setPosition({14.f, y + 5.f});
        m_scroll->m_contentLayer->addChild(nameLbl);

        auto srcLbl = CCLabelBMFont::create(entry.source.c_str(), "chatFont.fnt");
        srcLbl->setScale(0.42f);
        srcLbl->setAnchorPoint({0.f, 0.5f});
        srcLbl->setColor({160, 160, 160});
        srcLbl->setPosition({14.f, y - 8.f});
        m_scroll->m_contentLayer->addChild(srcLbl);

        auto rowMenu = CCMenu::create();
        rowMenu->setPosition({0.f, 0.f});
        rowMenu->setContentSize({340.f, contentH});
        m_scroll->m_contentLayer->addChild(rowMenu);

        auto btnSpr = ButtonSprite::create("Load");
        btnSpr->setScale(0.5f);
        auto btn = CCMenuItemSpriteExtra::create(btnSpr, this, menu_selector(ImportPopup::onPick));
        btn->setUserObject(CCString::create(entry.path.string()));
        btn->setPosition({296.f, y});
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

void ImportPopup::onInfo(CCObject*) {
    auto imports = Mod::get()->getSaveDir() / "imports";
    FLAlertLayer::create(
        "Where to put replays",
        fmt::format("Copy .gdr / .gdr2 files into:\n<cy>{}</c>\n\nReplays inside other bots' macro folders "
                    "(geode/config/<mod>/macros) are listed automatically.",
                    imports.string())
            .c_str(),
        "OK")
        ->show();
}

}  // namespace fwm::ui
