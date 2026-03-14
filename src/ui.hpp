#pragma once
#include <Geode/Geode.hpp>
#include <Geode/cocos/extensions/GUI/CCScrollView/CCScrollView.h>
#include <Geode/ui/Popup.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <ctime>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

using namespace geode::prelude;

static std::string fmtBytes(uintmax_t b) {
	if (b >= 1024ULL*1024*1024) {
		return std::to_string(b/(1024ULL*1024*1024)) + " GB";
	}
	if (b >= 1024*1024) {
		return std::to_string(b/(1024*1024)) + " MB";
	}
	if (b >= 1024) {
		return std::to_string(b/1024) + " KB";
	}
	return std::to_string(b) + " B";
}

static std::string ftimeStr(const std::filesystem::file_time_type& ft) {
	try {
		auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
			ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
		std::time_t tt = std::chrono::system_clock::to_time_t(sctp);

		struct tm timeinfo;
		localtime_s(&timeinfo, &tt);

		char buffer[20];
		std::strftime(buffer, sizeof(buffer), "%m/%d", &timeinfo);
		return std::string(buffer);
	} catch (...) {
		return "?";
	}
}

static std::string stemToName(const std::string& stem) {
	std::string o;
	for (char c : stem) {
		o += (c == '_') ? ' ' : c;
	}
	auto p = o.rfind(" att");
	if (p != std::string::npos) {
		o = o.substr(0, p);
	}
	return o.empty() ? "Unknown" : o;
}

static int stemToAtt(const std::string& stem) {
	auto p = stem.rfind("_att");
	if (p == std::string::npos) return 0;
	try {
		return std::stoi(stem.substr(p + 4));
	} catch (...) {
		return 0;
	}
}

struct ClipInfo {
	std::filesystem::path path;
	std::string levelName;
	int attempt = 0;
	uintmax_t fileSize = 0;
	std::string displaySize;
	std::string displayTime;
};

class ClipCardNode : public CCNode {
public:
	static ClipCardNode* create(const ClipInfo& info, float cardW, float cardH);
	bool init(const ClipInfo& info, float cardW, float cardH);
private:
	ClipInfo m_info;
	void onPlay(CCObject*);
	void onDelete(CCObject*);
};

class EchoClipGallery : public geode::Popup {
protected:
	bool init() {
		if (!Popup::init(600.f, 420.f)) return false;

		float pw = 600.f;
		float ph = 420.f;

		auto bgLayer = CCLayerColor::create({32, 32, 36, 250}, pw, ph);
		bgLayer->setAnchorPoint({0.f, 0.f});
		bgLayer->setPosition({0.f, 0.f});
		m_mainLayer->addChild(bgLayer, 0);

		auto borderTop = CCLayerColor::create({58, 58, 65, 220}, pw, 2.f);
		borderTop->setAnchorPoint({0.f, 1.f});
		borderTop->setPosition({0.f, ph});
		m_mainLayer->addChild(borderTop, 1);

		auto borderBottom = CCLayerColor::create({58, 58, 65, 220}, pw, 2.f);
		borderBottom->setAnchorPoint({0.f, 0.f});
		borderBottom->setPosition({0.f, 0.f});
		m_mainLayer->addChild(borderBottom, 1);

		auto borderLeft = CCLayerColor::create({58, 58, 65, 220}, 2.f, ph);
		borderLeft->setAnchorPoint({0.f, 0.f});
		borderLeft->setPosition({0.f, 0.f});
		m_mainLayer->addChild(borderLeft, 1);

		auto borderRight = CCLayerColor::create({58, 58, 65, 220}, 2.f, ph);
		borderRight->setAnchorPoint({1.f, 0.f});
		borderRight->setPosition({pw, 0.f});
		m_mainLayer->addChild(borderRight, 1);

		auto cornerTL = CCLayerColor::create({70, 70, 78, 220}, 3.f, 3.f);
		cornerTL->setAnchorPoint({0.f, 1.f});
		cornerTL->setPosition({0.f, ph});
		m_mainLayer->addChild(cornerTL, 2);

		auto cornerTR = CCLayerColor::create({70, 70, 78, 220}, 3.f, 3.f);
		cornerTR->setAnchorPoint({1.f, 1.f});
		cornerTR->setPosition({pw, ph});
		m_mainLayer->addChild(cornerTR, 2);

		auto cornerBL = CCLayerColor::create({70, 70, 78, 220}, 3.f, 3.f);
		cornerBL->setAnchorPoint({0.f, 0.f});
		cornerBL->setPosition({0.f, 0.f});
		m_mainLayer->addChild(cornerBL, 2);

		auto cornerBR = CCLayerColor::create({70, 70, 78, 220}, 3.f, 3.f);
		cornerBR->setAnchorPoint({1.f, 0.f});
		cornerBR->setPosition({pw, 0.f});
		m_mainLayer->addChild(cornerBR, 2);

		auto headerBg = CCLayerColor::create({45, 45, 50, 255}, pw, 50.f);
		headerBg->setAnchorPoint({0.f, 1.f});
		headerBg->setPosition({0.f, ph});
		m_mainLayer->addChild(headerBg, 1);

		auto headerBorder = CCLayerColor::create({65, 65, 72, 220}, pw, 1.f);
		headerBorder->setAnchorPoint({0.f, 1.f});
		headerBorder->setPosition({0.f, ph - 50.f});
		m_mainLayer->addChild(headerBorder, 2);

		auto title = CCLabelBMFont::create("echoclip", "bigFont.fnt");
		title->setScale(0.65f);
		title->setColor({200, 200, 210});
		title->setAnchorPoint({0.5f, 0.5f});
		title->setPosition({pw * 0.5f, ph - 25.f});
		m_mainLayer->addChild(title, 3);

		auto cnt = CCLabelBMFont::create("0", "chatFont.fnt");
		cnt->setScale(0.45f);
		cnt->setColor({140, 140, 150});
		cnt->setAnchorPoint({1.f, 0.5f});
		cnt->setPosition({pw - 40.f, ph - 25.f});
		m_mainLayer->addChild(cnt, 3);
		m_countLabel = cnt;

		auto closeMenu = CCMenu::create();
		closeMenu->setPosition({pw - 18.f, ph - 18.f});
		m_mainLayer->addChild(closeMenu, 10);

		auto xLabel = CCLabelBMFont::create("X", "bigFont.fnt");
		xLabel->setScale(0.45f);
		xLabel->setColor({128, 128, 128});
		auto closeBtn = CCMenuItemSpriteExtra::create(xLabel, this, menu_selector(EchoClipGallery::onClose));
		closeBtn->setPosition({0.f, 0.f});
		closeMenu->addChild(closeBtn);

		float scrollW = pw - 20.f;
		float scrollH = ph - 100.f;

		m_container = CCLayer::create();
		m_scrollView = CCScrollView::create(CCSizeMake(scrollW, scrollH), m_container);
		m_scrollView->setDirection(kCCScrollViewDirectionVertical);
		m_scrollView->setAnchorPoint({0.f, 0.f});
		m_scrollView->setPosition({10.f, 50.f});
		m_mainLayer->addChild(m_scrollView, 2);

		loadClips();
		m_filtered = m_clips;
		updateCount();

		if (m_clips.empty()) {
			auto nl = CCLabelBMFont::create("no clips\nF6 record", "bigFont.fnt");
			nl->setScale(0.4f);
			nl->setColor({100, 100, 110});
			nl->setAlignment(kCCTextAlignmentCenter);
			nl->setPosition({pw * 0.5f, ph * 0.5f});
			m_mainLayer->addChild(nl, 3);
		} else {
			buildGrid();
		}

		auto btmBg = CCLayerColor::create({38, 38, 43, 230}, pw, 40.f);
		btmBg->setAnchorPoint({0.f, 0.f});
		btmBg->setPosition({0.f, 0.f});
		m_mainLayer->addChild(btmBg, 1);

		auto btmBorder = CCLayerColor::create({65, 65, 72, 220}, pw, 1.f);
		btmBorder->setAnchorPoint({0.f, 0.f});
		btmBorder->setPosition({0.f, 40.f});
		m_mainLayer->addChild(btmBorder, 2);

		auto btmMenu = CCMenu::create();
		btmMenu->setPosition({pw * 0.5f, 20.f});
		m_mainLayer->addChild(btmMenu, 3);

		auto supSpr = CCLabelBMFont::create("support", "chatFont.fnt");
		supSpr->setScale(0.5f);
		supSpr->setColor({120, 160, 200});
		auto supBtn = CCMenuItemSpriteExtra::create(supSpr, this, menu_selector(EchoClipGallery::onOpenSupport));
		supBtn->setPositionX(-60.f);
		btmMenu->addChild(supBtn);

		auto refSpr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png");
		if (!refSpr) refSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
		if (refSpr) {
			refSpr->setScale(0.45f);
			refSpr->setColor({160, 160, 175});
			auto refBtn = CCMenuItemSpriteExtra::create(refSpr, this, menu_selector(EchoClipGallery::onRefresh));
			refBtn->setPositionX(0.f);
			btmMenu->addChild(refBtn);
		}

		return true;
	}

	void onClose(CCObject*) override {
		this->removeFromParent();
	}

public:
	static EchoClipGallery* create() {
		auto ret = new EchoClipGallery();
		if (ret && ret->init()) {
			ret->autorelease();
			return ret;
		}
		CC_SAFE_DELETE(ret);
		return nullptr;
	}

	static void open() {
		if (auto popup = create()) {
			popup->show();
		}
	}

private:
	CCScrollView* m_scrollView = nullptr;
	CCLayer* m_container = nullptr;
	CCLabelBMFont* m_countLabel = nullptr;
	std::vector<ClipInfo> m_clips;
	std::vector<ClipInfo> m_filtered;

	void loadClips();
	void buildGrid();
	void updateCount();
	void onOpenSupport(CCObject*);
	void onRefresh(CCObject*);
};