#include "ui.hpp"
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

class $modify(MenuHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        CCMenu* menu = (CCMenu*)getChildByID("bottom-menu");
        if (!menu) return true;

        CCSprite* s = CCSprite::create("gallery.png"_spr); // idk why i use the samsung gallery icon but id go with it
        if (!s) {
            s = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
            if (s) s->setColor({160, 160, 175});
        }
        if (!s) return true;

        s->setScale(0.5f);

        auto btn = CCMenuItemSpriteExtra::create(s, this, menu_selector(MenuHook::onGallery));
        btn->setID("axiom.echoclip/gallery-btn");
        menu->addChild(btn);
        menu->updateLayout();

        return true;
    }

    void onGallery(CCObject*) {
        Gallery::open();
    }
};