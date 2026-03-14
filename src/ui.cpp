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

	auto bgBase = CCLayerColor::create({48, 48, 54, 210}, cw, ch);
	bgBase->setAnchorPoint({0.f, 0.f});
	bgBase->setPosition({0.f, 0.f});
	addChild(bgBase, 0);

	auto borderCard = CCLayerColor::create({72, 72, 80, 190}, cw, 1.f);
	borderCard->setAnchorPoint({0.f, 0.f});
	borderCard->setPosition({0.f, ch - 1.f});
	addChild(borderCard, 1);

	auto cornerCardTL = CCLayerColor::create({72, 72, 80, 190}, 2.f, 2.f);
	cornerCardTL->setAnchorPoint({0.f, 0.f});
	cornerCardTL->setPosition({0.f, ch - 2.f});
	addChild(cornerCardTL, 2);

	auto cornerCardTR = CCLayerColor::create({72, 72, 80, 190}, 2.f, 2.f);
	cornerCardTR->setAnchorPoint({0.f, 0.f});
	cornerCardTR->setPosition({cw - 2.f, ch - 2.f});
	addChild(cornerCardTR, 2);

	auto cornerCardBL = CCLayerColor::create({72, 72, 80, 190}, 2.f, 2.f);
	cornerCardBL->setAnchorPoint({0.f, 0.f});
	cornerCardBL->setPosition({0.f, 0.f});
	addChild(cornerCardBL, 2);

	auto cornerCardBR = CCLayerColor::create({72, 72, 80, 190}, 2.f, 2.f);
	cornerCardBR->setAnchorPoint({0.f, 0.f});
	cornerCardBR->setPosition({cw - 2.f, 0.f});
	addChild(cornerCardBR, 2);

	auto name = info.levelName;
	if (name.size() > 15) name = name.substr(0, 15);

	auto nl = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
	nl->setScale(0.35f);
	nl->setColor({210, 210, 220});
	nl->setAnchorPoint({0.5f, 0.5f});
	nl->setPosition({cw * 0.5f, ch - 25.f});
	addChild(nl, 2);

	auto al = CCLabelBMFont::create(("Att " + std::to_string(info.attempt)).c_str(), "chatFont.fnt");
	al->setScale(0.4f);
	al->setColor({200, 180, 130});
	al->setAnchorPoint({0.5f, 0.5f});
	al->setPosition({cw * 0.5f, ch - 40.f});
	addChild(al, 2);

	auto sl = CCLabelBMFont::create(info.displaySize.c_str(), "chatFont.fnt");
	sl->setScale(0.38f);
	sl->setColor({140, 140, 150});
	sl->setAnchorPoint({0.5f, 0.5f});
	sl->setPosition({cw * 0.5f, 10.f});
	addChild(sl, 2);

	auto tl = CCLabelBMFont::create(info.displayTime.c_str(), "chatFont.fnt");
	tl->setScale(0.32f);
	tl->setColor({120, 120, 130});
	tl->setAnchorPoint({0.5f, 0.5f});
	tl->setPosition({cw * 0.5f, 2.f});
	addChild(tl, 2);

	auto menu = CCMenu::create();
	menu->setPosition({0, 0});
	addChild(menu, 3);

	auto pspr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
	if (!pspr) pspr = CCSprite::createWithSpriteFrameName("GJ_playBtn_001.png");
	if (pspr) {
		pspr->setScale(0.6f);
		pspr->setColor({180, 180, 195});
		auto pb = CCMenuItemSpriteExtra::create(pspr, this, menu_selector(ClipCardNode::onPlay));
		pb->setPosition({cw * 0.5f, ch * 0.65f});
		menu->addChild(pb);
	}

	auto dspr = CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png");
	if (!dspr) dspr = CCSprite::createWithSpriteFrameName("GJ_trashBtn_001.png");
	if (dspr) {
		dspr->setScale(0.5f);
		dspr->setColor({200, 140, 140});
		auto db = CCMenuItemSpriteExtra::create(dspr, this, menu_selector(ClipCardNode::onDelete));
		db->setPosition({cw - 10.f, ch - 10.f});
		menu->addChild(db);
	}

	return true;
}

void ClipCardNode::onPlay(CCObject*) {
#ifdef GEODE_IS_WINDOWS
	ShellExecuteA(nullptr, "open", m_info.path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
		std::string stem = p.stem().string();
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

void EchoClipGallery::buildGrid() {
	if (!m_scrollView || !m_container) return;

	float scrollW = 580.f;
	float cardW = 160.f;
	float cardH = 140.f;
	float padX = 12.f;
	float padY = 12.f;
	int cols = 3;

	m_container->removeAllChildren();

	int rows = ((int)m_filtered.size() + cols - 1) / cols;
	if (rows == 0) rows = 1;

	float totalHeight = rows * (cardH + padY) + padY;
	if (totalHeight < 270.f) totalHeight = 270.f;

	float gridWidth = cols * cardW + (cols - 1) * padX;
	float startX = (scrollW - gridWidth) * 0.5f;

	m_container->setContentSize({scrollW, totalHeight});

	for (size_t i = 0; i < m_filtered.size(); ++i) {
		int col = (int)i % cols;
		int row = (int)i / cols;
		float x = startX + col * (cardW + padX);
		float y = totalHeight - padY - (row + 1) * cardH - row * padY;

		auto card = ClipCardNode::create(m_filtered[i], cardW, cardH);
		if (!card) continue;

		card->setPosition({x, y});
		m_container->addChild(card);
	}

	m_scrollView->setContentSize({scrollW, totalHeight});
	m_scrollView->setContentOffset({0.f, 270.f - totalHeight}, false);
}

void EchoClipGallery::updateCount() {
	if (!m_countLabel) return;
	std::string cnt = std::to_string(m_filtered.size()) + "/" + std::to_string(m_clips.size());
	m_countLabel->setString(cnt.c_str());
}

void EchoClipGallery::onOpenSupport(CCObject*) {
#ifdef GEODE_IS_WINDOWS
	ShellExecuteA(nullptr, "open", "https://paypal.me/AxiomSultra", nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void EchoClipGallery::onRefresh(CCObject*) {
	m_clips.clear();
	m_filtered.clear();

	if (m_container) {
		m_container->removeAllChildren();
	}

	loadClips();
	m_filtered = m_clips;

	if (!m_filtered.empty()) {
		buildGrid();
	}

	updateCount();
	Notification::create("refreshed", NotificationIcon::Success)->show();
}