#include "ui.hpp"
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

class $modify(EchoClipMenuBtn, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto bottomMenu = this->getChildByID("bottom-menu");
        if (!bottomMenu) return true;

        auto resDir = Mod::get()->getResourcesDir();
        auto spr = CCSprite::create((resDir / "logo.png").string().c_str());
        if (!spr) {
            spr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
            if (spr) spr->setColor({160, 160, 175});
        }

        if (!spr) return true;

        spr->setScale(0.45f);

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            this,
            menu_selector(EchoClipMenuBtn::onOpenGallery)
        );
        btn->setID("echoclip-gallery-btn");
        bottomMenu->addChild(btn);
        bottomMenu->updateLayout();

        return true;
    }

    void onOpenGallery(CCObject*) {
        EchoClipGallery::open();
    }
};
