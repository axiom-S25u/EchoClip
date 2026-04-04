#define _CRT_SECURE_NO_WARNINGS
#include "watermark.hpp"
#include <Geode/ui/OverlayManager.hpp>
#include <ctime>
using namespace geode::prelude;

bool Watermark::init() {
    if (!CCNode::init()) return false;

    auto winSize = CCDirector::get()->getWinSize();

    auto mainLabel = CCLabelBMFont::create("Activate EchoClip", "chatFont.fnt");// no one will know till next year.(if i even maintain this mod)
    mainLabel->setScale(0.75f);
    mainLabel->setOpacity(180); 
    mainLabel->setAnchorPoint({1, 0});
    mainLabel->setPosition(winSize.width - 10, 28);
    addChild(mainLabel);

    auto subLabel = CCLabelBMFont::create("Go to Settings to activate EchoClip.", "chatFont.fnt");
    subLabel->setScale(0.5f);
    subLabel->setOpacity(180);
    subLabel->setAnchorPoint({1, 0});
    subLabel->setPosition(winSize.width - 10, 12);
    addChild(subLabel);

    return true;
}

Watermark* Watermark::create() {
    auto ret = new Watermark();
    if (ret && ret->init()) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

void Watermark::checkAndShow(CCNode* layer) {
    if (!Mod::get()->getSettingValue<bool>("april-fools")) return;

    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);
    if (!now) return;

    if (now->tm_mon == 3 && now->tm_mday == 1) {
        if (!geode::OverlayManager::get()->getChildByID("axiom.echoclip/watermark")) {
            auto wm = Watermark::create();
            wm->setID("axiom.echoclip/watermark");
            geode::OverlayManager::get()->addChild(wm);
        }
    } else {
        if (auto wm = geode::OverlayManager::get()->getChildByID("axiom.echoclip/watermark")) {
            wm->removeFromParent();
        }
    }
}

#include <Geode/modify/MenuLayer.hpp>
class $modify(MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        Watermark::checkAndShow(this);
        return true;
    }
};

#include <Geode/modify/CreatorLayer.hpp>
class $modify(CreatorLayer) {
    bool init() {
        if (!CreatorLayer::init()) return false;
        Watermark::checkAndShow(this);
        return true;
    }
};

#include <Geode/modify/LevelBrowserLayer.hpp>
class $modify(LevelBrowserLayer) {
    bool init(GJSearchObject* search) {
        if (!LevelBrowserLayer::init(search)) return false;
        Watermark::checkAndShow(this);
        return true;
    }
};

#include <Geode/modify/EditLevelLayer.hpp>
class $modify(EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        Watermark::checkAndShow(this);
        return true;
    }
};

#include <Geode/modify/LevelInfoLayer.hpp>
class $modify(LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        Watermark::checkAndShow(this);
        return true;
    }
};