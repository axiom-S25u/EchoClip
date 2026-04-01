#pragma once
#include <Geode/Geode.hpp>

using namespace geode::prelude;

class Watermark : public cocos2d::CCNode {
protected:
    bool init() override;
public:
    static Watermark* create();
    static void checkAndShow(cocos2d::CCNode* layer);
};
