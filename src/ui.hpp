#pragma once
#include <Geode/Geode.hpp>
#include <Geode/cocos/extensions/GUI/CCScrollView/CCScrollView.h>
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
    if (b >= 1024ULL * 1024 * 1024) return std::to_string(b / (1024ULL * 1024 * 1024)) + " GB";
    if (b >= 1024 * 1024) return std::to_string(b / (1024 * 1024)) + " MB";
    if (b >= 1024) return std::to_string(b / 1024) + " KB";
    return std::to_string(b) + " B";
}

static std::string ftimeStr(const std::filesystem::file_time_type& ft) {
    try {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
        struct tm timeinfo = {};
#ifdef _WIN32
        localtime_s(&timeinfo, &tt);
#else
        localtime_r(&tt, &timeinfo);
#endif
        char buffer[20];
        std::strftime(buffer, sizeof(buffer), "%m/%d %H:%M", &timeinfo);
        return std::string(buffer);
    } catch (...) {
        return "?";
    }
}

static std::string stemToName(const std::string& stem) {
    auto attPos = stem.rfind("_att");
    std::string namePart = (attPos != std::string::npos) ? stem.substr(0, attPos) : stem;
    auto tsPos = namePart.rfind('_');
    if (tsPos != std::string::npos) {
        bool allDigits = true;
        for (size_t i = tsPos + 1; i < namePart.size(); i++) {
            if (!std::isdigit((unsigned char)namePart[i])) { allDigits = false; break; }
        }
        if (allDigits && (namePart.size() - tsPos - 1) >= 8) {
            namePart = namePart.substr(0, tsPos);
        }
    }
    std::string o;
    for (char c : namePart) {
        o += (c == '_') ? ' ' : c;
    }
    return o.empty() ? "Unknown" : o;
}

static int stemToAtt(const std::string& stem) {
    auto p = stem.rfind("_att");
    if (p == std::string::npos) return 0;
    try {
        std::string rest = stem.substr(p + 4);
        std::string digits;
        for (char c : rest) {
            if (std::isdigit((unsigned char)c)) digits += c;
            else break;
        }
        return digits.empty() ? 0 : std::stoi(digits);
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
    void setHighlight(bool on);
private:
    ClipInfo m_info;
    CCLayerColor* m_bgBase = nullptr;
    void onPlay(CCObject*);
    void onDelete(CCObject*);
};

class EchoClipGallery : public CCLayerColor {
protected:
    bool init();

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
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return;
        if (auto existing = scene->getChildByID("echoclip-gallery")) {
            existing->removeFromParent();
            return;
        }
        auto layer = create();
        if (!layer) return;
        layer->setID("echoclip-gallery");
        scene->addChild(layer, 9999);
    }

private:
    CCScrollView* m_scrollView = nullptr;
    CCLayer* m_container = nullptr;
    CCLabelBMFont* m_countLabel = nullptr;
    std::vector<ClipInfo> m_clips;
    std::vector<ClipInfo> m_filtered;

    void onClose(CCObject*);
    void loadClips();
    void buildGrid();
    void buildGrid(float panelW);
    void updateCount();
    void onOpenFolder(CCObject*);
    void onOpenSupport(CCObject*);
    void onRefresh(CCObject*);
};