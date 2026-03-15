#include "ui.hpp"
#include <Geode/Geode.hpp>
#include <Geode/cocos/extensions/GUI/CCScrollView/CCScrollView.h>
#include <filesystem>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

using namespace geode::prelude;

ClipCardNode* ClipCardNode::create(const ClipInfo& info, float cw, float ch) {
    auto* n = new ClipCardNode();
    if (n && n->init(info, cw, ch)) {
        n->autorelease();
        return n;
    }
    delete n;
    return nullptr;
}

bool ClipCardNode::init(const ClipInfo& info, float cw, float ch) {
    if (!CCNode::init()) return false;

    m_info = info;
    setContentSize({cw, ch});

    m_bgBase = CCLayerColor::create({36, 36, 42, 245}, cw, ch);
    m_bgBase->setAnchorPoint({0.f, 0.f});
    m_bgBase->setPosition({0.f, 0.f});
    addChild(m_bgBase, 0);

    auto borderL = CCLayerColor::create({72, 106, 190, 230}, 2.f, ch);
    borderL->setAnchorPoint({0.f, 0.f});
    borderL->setPosition({0.f, 0.f});
    addChild(borderL, 1);

    auto borderTop = CCLayerColor::create({52, 52, 62, 180}, cw, 1.f);
    borderTop->setAnchorPoint({0.f, 1.f});
    borderTop->setPosition({0.f, ch});
    addChild(borderTop, 1);

    auto borderBot = CCLayerColor::create({52, 52, 62, 180}, cw, 1.f);
    borderBot->setAnchorPoint({0.f, 0.f});
    borderBot->setPosition({0.f, 0.f});
    addChild(borderBot, 1);

    auto borderR = CCLayerColor::create({52, 52, 62, 180}, 1.f, ch);
    borderR->setAnchorPoint({0.f, 0.f});
    borderR->setPosition({cw - 1.f, 0.f});
    addChild(borderR, 1);

    float infoH = 38.f;
    float thumbH = ch - infoH;
    float thumbW = cw - 2.f;

    auto thumbBg = CCLayerColor::create({14, 14, 18, 255}, thumbW, thumbH);
    thumbBg->setAnchorPoint({0.f, 0.f});
    thumbBg->setPosition({2.f, infoH});
    addChild(thumbBg, 1);

    auto scanA = CCLayerColor::create({255, 255, 255, 5}, thumbW, 1.f);
    scanA->setAnchorPoint({0.f, 0.f});
    scanA->setPosition({2.f, infoH + thumbH * 0.33f});
    addChild(scanA, 2);

    auto scanB = CCLayerColor::create({255, 255, 255, 5}, thumbW, 1.f);
    scanB->setAnchorPoint({0.f, 0.f});
    scanB->setPosition({2.f, infoH + thumbH * 0.66f});
    addChild(scanB, 2);

    auto playLbl = CCLabelBMFont::create("PLAY", "chatFont.fnt");
    playLbl->setScale(0.22f);
    playLbl->setColor({55, 85, 145});
    playLbl->setAnchorPoint({0.5f, 0.5f});
    playLbl->setPosition({2.f + thumbW * 0.5f, infoH + thumbH * 0.5f});
    addChild(playLbl, 3);

    auto name = info.levelName;
    if (name.size() > 13) name = name.substr(0, 13);

    auto nl = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
    nl->setScale(0.28f);
    nl->setColor({205, 205, 220});
    nl->setAnchorPoint({0.f, 0.5f});
    nl->setPosition({6.f, infoH * 0.65f});
    addChild(nl, 2);

    std::string attStr = "att " + std::to_string(info.attempt);
    auto al = CCLabelBMFont::create(attStr.c_str(), "chatFont.fnt");
    al->setScale(0.26f);
    al->setColor({165, 145, 85});
    al->setAnchorPoint({0.f, 0.5f});
    al->setPosition({6.f, infoH * 0.28f});
    addChild(al, 2);

    auto tl = CCLabelBMFont::create(info.displayTime.c_str(), "chatFont.fnt");
    tl->setScale(0.24f);
    tl->setColor({95, 95, 110});
    tl->setAnchorPoint({1.f, 0.5f});
    tl->setPosition({cw - 6.f, infoH * 0.65f});
    addChild(tl, 2);

    auto sl = CCLabelBMFont::create(info.displaySize.c_str(), "chatFont.fnt");
    sl->setScale(0.24f);
    sl->setColor({108, 108, 124});
    sl->setAnchorPoint({1.f, 0.5f});
    sl->setPosition({cw - 6.f, infoH * 0.28f});
    addChild(sl, 2);

    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    addChild(menu, 3);

    auto pspr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
    if (!pspr) pspr = CCSprite::createWithSpriteFrameName("GJ_playBtn_001.png");
    if (pspr) {
        pspr->setScale(0.42f);
        pspr->setColor({145, 190, 145});
        auto pb = CCMenuItemSpriteExtra::create(pspr, this, menu_selector(ClipCardNode::onPlay));
        pb->setPosition({cw - 13.f, ch - 10.f});
        menu->addChild(pb);
    }

    auto dspr = CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png");
    if (!dspr) dspr = CCSprite::createWithSpriteFrameName("GJ_trashBtn_001.png");
    if (dspr) {
        dspr->setScale(0.32f);
        dspr->setColor({200, 105, 105});
        auto db = CCMenuItemSpriteExtra::create(dspr, this, menu_selector(ClipCardNode::onDelete));
        db->setPosition({cw - 13.f, ch - 27.f});
        menu->addChild(db);
    }

    return true;
}

void ClipCardNode::setHighlight(bool on) {
    if (m_bgBase) {
        m_bgBase->setColor(on ? ccColor3B{50, 50, 58} : ccColor3B{36, 36, 42});
    }
}

void ClipCardNode::onPlay(CCObject*) {
#ifdef GEODE_IS_WINDOWS
    ShellExecuteW(nullptr, L"open", m_info.path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void ClipCardNode::onDelete(CCObject*) {
    auto p = m_info.path;
    auto ln = m_info.levelName;
    auto att = m_info.attempt;

    geode::createQuickPopup("Delete",
        "Delete <cy>" + ln + "</c> att " + std::to_string(att) + "?",
        "Nah", "Delete",
        [p](auto, bool b2) {
            if (!b2) return;
            std::error_code ec;
            std::filesystem::remove(p, ec);
            if (ec) {
                Notification::create("failed to delete", NotificationIcon::Error)->show();
            } else {
                Notification::create("clip deleted", NotificationIcon::Success)->show();
            }
        }
    );
}

bool EchoClipGallery::init() {
    auto winSize = CCDirector::get()->getWinSize();
    float pw = winSize.width;
    float ph = winSize.height;

    if (!CCLayerColor::initWithColor({10, 10, 14, 220}, pw, ph)) return false;

    setPosition({0.f, 0.f});
    setTouchEnabled(true);
    setKeyboardEnabled(true);

    float panelW = std::min(pw - 40.f, 860.f);
    float panelH = std::min(ph - 40.f, 540.f);
    float panelX = (pw - panelW) * 0.5f;
    float panelY = (ph - panelH) * 0.5f;

    auto panel = CCLayerColor::create({20, 20, 26, 255}, panelW, panelH);
    panel->setAnchorPoint({0.f, 0.f});
    panel->setPosition({panelX, panelY});
    addChild(panel, 1);

    auto accentTop = CCLayerColor::create({65, 105, 195, 255}, panelW, 3.f);
    accentTop->setAnchorPoint({0.f, 1.f});
    accentTop->setPosition({0.f, panelH});
    panel->addChild(accentTop, 2);

    auto borderL = CCLayerColor::create({46, 46, 56, 255}, 1.f, panelH);
    borderL->setAnchorPoint({0.f, 0.f});
    borderL->setPosition({0.f, 0.f});
    panel->addChild(borderL, 2);

    auto borderR = CCLayerColor::create({46, 46, 56, 255}, 1.f, panelH);
    borderR->setAnchorPoint({1.f, 0.f});
    borderR->setPosition({panelW, 0.f});
    panel->addChild(borderR, 2);

    auto borderB = CCLayerColor::create({46, 46, 56, 255}, panelW, 1.f);
    borderB->setAnchorPoint({0.f, 0.f});
    borderB->setPosition({0.f, 0.f});
    panel->addChild(borderB, 2);

    auto headerBg = CCLayerColor::create({27, 27, 35, 255}, panelW, 44.f);
    headerBg->setAnchorPoint({0.f, 1.f});
    headerBg->setPosition({0.f, panelH});
    panel->addChild(headerBg, 1);

    auto headerBorder = CCLayerColor::create({40, 40, 52, 255}, panelW, 1.f);
    headerBorder->setAnchorPoint({0.f, 1.f});
    headerBorder->setPosition({0.f, panelH - 44.f});
    panel->addChild(headerBorder, 2);

    auto echoLabel = CCLabelBMFont::create("echo", "bigFont.fnt");
    echoLabel->setScale(0.48f);
    echoLabel->setColor({92, 132, 210});
    echoLabel->setAnchorPoint({1.f, 0.5f});
    echoLabel->setPosition({panelW * 0.5f - 1.f, panelH - 22.f});
    panel->addChild(echoLabel, 3);

    auto clipLabel = CCLabelBMFont::create("clip", "bigFont.fnt");
    clipLabel->setScale(0.48f);
    clipLabel->setColor({192, 192, 208});
    clipLabel->setAnchorPoint({0.f, 0.5f});
    clipLabel->setPosition({panelW * 0.5f + 1.f, panelH - 22.f});
    panel->addChild(clipLabel, 3);

    auto cnt = CCLabelBMFont::create("0 clips", "chatFont.fnt");
    cnt->setScale(0.35f);
    cnt->setColor({82, 82, 98});
    cnt->setAnchorPoint({0.f, 0.5f});
    cnt->setPosition({16.f, panelH - 22.f});
    panel->addChild(cnt, 3);
    m_countLabel = cnt;

    auto headerMenu = CCMenu::create();
    headerMenu->setPosition({0.f, 0.f});
    panel->addChild(headerMenu, 10);

    auto xSpr = CCLabelBMFont::create("X", "bigFont.fnt");
    xSpr->setScale(0.37f);
    xSpr->setColor({92, 92, 108});
    auto closeBtn = CCMenuItemSpriteExtra::create(xSpr, this, menu_selector(EchoClipGallery::onClose));
    closeBtn->setPosition({panelW - 14.f, panelH - 14.f});
    headerMenu->addChild(closeBtn);

    float scrollW = panelW - 14.f;
    float scrollH = panelH - 92.f;

    m_container = CCLayer::create();
    m_scrollView = CCScrollView::create(CCSizeMake(scrollW, scrollH), m_container);
    m_scrollView->setDirection(kCCScrollViewDirectionVertical);
    m_scrollView->setAnchorPoint({0.f, 0.f});
    m_scrollView->setPosition({7.f, 44.f});
    panel->addChild(m_scrollView, 2);

    loadClips();
    m_filtered = m_clips;
    updateCount();

    if (m_clips.empty()) {
        auto nl = CCLabelBMFont::create("no clips yet", "bigFont.fnt");
        nl->setScale(0.4f);
        nl->setColor({72, 72, 88});
        nl->setAlignment(kCCTextAlignmentCenter);
        nl->setAnchorPoint({0.5f, 0.5f});
        nl->setPosition({panelW * 0.5f, panelH * 0.5f + 12.f});
        panel->addChild(nl, 3);

        auto hint = CCLabelBMFont::create("use your clip keybind in a level to record", "chatFont.fnt");
        hint->setScale(0.32f);
        hint->setColor({58, 58, 72});
        hint->setAlignment(kCCTextAlignmentCenter);
        hint->setAnchorPoint({0.5f, 0.5f});
        hint->setPosition({panelW * 0.5f, panelH * 0.5f - 12.f});
        panel->addChild(hint, 3);
    } else {
        buildGrid(panelW);
    }

    auto btmBg = CCLayerColor::create({24, 24, 32, 255}, panelW, 44.f);
    btmBg->setAnchorPoint({0.f, 0.f});
    btmBg->setPosition({0.f, 0.f});
    panel->addChild(btmBg, 1);

    auto btmBorder = CCLayerColor::create({40, 40, 52, 255}, panelW, 1.f);
    btmBorder->setAnchorPoint({0.f, 0.f});
    btmBorder->setPosition({0.f, 44.f});
    panel->addChild(btmBorder, 2);

    auto btmMenu = CCMenu::create();
    btmMenu->setPosition({panelW * 0.5f, 22.f});
    panel->addChild(btmMenu, 3);

    auto folderLbl = CCLabelBMFont::create("open folder", "chatFont.fnt");
    folderLbl->setScale(0.4f);
    folderLbl->setColor({122, 176, 122});
    auto folderBtn = CCMenuItemSpriteExtra::create(folderLbl, this, menu_selector(EchoClipGallery::onOpenFolder));
    folderBtn->setPositionX(-145.f);
    btmMenu->addChild(folderBtn);

    auto refSpr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png");
    if (!refSpr) refSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    if (refSpr) {
        refSpr->setScale(0.36f);
        refSpr->setColor({130, 130, 150});
        auto refBtn = CCMenuItemSpriteExtra::create(refSpr, this, menu_selector(EchoClipGallery::onRefresh));
        refBtn->setPositionX(-50.f);
        btmMenu->addChild(refBtn);
    }

    auto closeLbl = CCLabelBMFont::create("close", "chatFont.fnt");
    closeLbl->setScale(0.4f);
    closeLbl->setColor({180, 100, 100});
    auto closeBtnBtm = CCMenuItemSpriteExtra::create(closeLbl, this, menu_selector(EchoClipGallery::onClose));
    closeBtnBtm->setPositionX(50.f);
    btmMenu->addChild(closeBtnBtm);

    auto supLbl = CCLabelBMFont::create("support", "chatFont.fnt");
    supLbl->setScale(0.4f);
    supLbl->setColor({92, 142, 196});
    auto supBtn = CCMenuItemSpriteExtra::create(supLbl, this, menu_selector(EchoClipGallery::onOpenSupport));
    supBtn->setPositionX(145.f);
    btmMenu->addChild(supBtn);

    return true;
}

void EchoClipGallery::onClose(CCObject*) {
    this->removeFromParent();
}

void EchoClipGallery::loadClips() {
    m_clips.clear();
    auto dir = Mod::get()->getSaveDir() / "clips";
    std::error_code ec;

    if (!std::filesystem::exists(dir, ec)) return;

    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;

        auto p = e.path();
        if (p.extension() != ".mkv" && p.extension() != ".mp4") continue;

        ClipInfo inf;
        inf.path = p;
        auto u8stem = p.stem().generic_u8string();
        std::string stem(u8stem.begin(), u8stem.end());
        inf.levelName = stemToName(stem);
        inf.attempt = stemToAtt(stem);

        std::error_code fec;
        inf.fileSize = std::filesystem::file_size(p, fec);
        inf.displaySize = fmtBytes(inf.fileSize);

        std::error_code tec;
        auto ft = std::filesystem::last_write_time(p, tec);
        inf.displayTime = tec ? "?" : ftimeStr(ft);

        m_clips.push_back(std::move(inf));
    }

    std::sort(m_clips.begin(), m_clips.end(), [](const ClipInfo& a, const ClipInfo& b) {
        std::error_code ea, eb;
        auto ta = std::filesystem::last_write_time(a.path, ea);
        auto tb = std::filesystem::last_write_time(b.path, eb);
        return ta > tb;
    });
}

void EchoClipGallery::buildGrid(float panelW) {
    if (!m_scrollView || !m_container) return;

    float scrollW = panelW - 14.f;
    int cols = 4;
    float padX = 8.f;
    float padY = 8.f;
    float cardW = (scrollW - padX * (cols + 1)) / cols;
    float cardH = cardW * 0.75f;

    m_container->removeAllChildren();

    int count = (int)m_filtered.size();
    int rowCount = (count + cols - 1) / cols;
    if (rowCount < 1) rowCount = 1;

    float totalHeight = rowCount * (cardH + padY) + padY;
    float viewH = m_scrollView->getViewSize().height;
    if (totalHeight < viewH) totalHeight = viewH;

    m_container->setContentSize({scrollW, totalHeight});

    for (int i = 0; i < count; i++) {
        int col = i % cols;
        int row = i / cols;
        float x = padX + col * (cardW + padX);
        float y = totalHeight - padY - (row + 1) * cardH - row * padY;

        auto card = ClipCardNode::create(m_filtered[i], cardW, cardH);
        if (!card) continue;

        card->setPosition({x, y});
        m_container->addChild(card);
    }

    m_scrollView->setContentSize({scrollW, totalHeight});
    float offset = viewH - totalHeight;
    if (offset > 0.f) offset = 0.f;
    m_scrollView->setContentOffset({0.f, offset}, false);
}

void EchoClipGallery::buildGrid() {
    auto winSize = CCDirector::get()->getWinSize();
    float panelW = std::min(winSize.width - 40.f, 860.f);
    buildGrid(panelW);
}

void EchoClipGallery::updateCount() {
    if (!m_countLabel) return;
    std::string cnt = std::to_string(m_filtered.size()) + " clips";
    m_countLabel->setString(cnt.c_str());
}

void EchoClipGallery::onOpenFolder(CCObject*) {
#ifdef GEODE_IS_WINDOWS
    auto dir = Mod::get()->getSaveDir() / "clips";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void EchoClipGallery::onOpenSupport(CCObject*) {
#ifdef GEODE_IS_WINDOWS
    ShellExecuteW(nullptr, L"open", L"https://paypal.me/AxiomSultra", nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void EchoClipGallery::onRefresh(CCObject*) {
    m_clips.clear();
    m_filtered.clear();

    if (m_container) m_container->removeAllChildren();

    loadClips();
    m_filtered = m_clips;

    if (!m_filtered.empty()) buildGrid();

    updateCount();
    Notification::create("refreshed", NotificationIcon::Success)->show();
}