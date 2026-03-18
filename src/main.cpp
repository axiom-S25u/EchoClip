#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <eclipse.ffmpeg-api/include/events.hpp>
#include <eclipse.ffmpeg-api/include/audio_mixer.hpp>
#include "ui.hpp"
#include <deque>
#include <mutex>
#include <cstring>
#include <ctime>

using namespace geode::prelude;

static ffmpeg::events::Recorder g_rec;
static std::mutex g_rec_m;
static std::thread g_worker;
static std::deque<std::vector<uint8_t>> g_q;
static std::mutex g_q_m;
static std::condition_variable g_cv;
static bool g_exit = false;
static bool g_recording = false;

static int g_rw = 0, g_rh = 0;
static int64_t g_f_count = 0;
static std::filesystem::path g_tmp;
static int g_best = 0;

struct History {
    std::string name;
    int att;
    int64_t f;
};
static std::deque<History> g_hist;

#define FIX(x) ((x) & ~1)

static void work() {
    geode::utils::thread::setName("EchoClip");
#ifdef GEODE_IS_WINDOWS
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    while (true) {
        std::vector<uint8_t> f;
        {
            std::unique_lock l(g_q_m);
            g_cv.wait(l, [] { return !g_q.empty() || g_exit; });
            if (g_exit && g_q.empty()) break;
            if (g_q.empty()) continue;
            f = std::move(g_q.front());
            g_q.pop_front();
        }
        std::lock_guard l(g_rec_m);
        if (g_rec.isValid()) (void)g_rec.writeFrame(f);
    }
}

static void stop_work() {
    {
        std::lock_guard l(g_q_m);
        g_exit = true;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    g_exit = false;
    g_q.clear();
}

static void go(int w, int h) {
    bool hr = Mod::get()->getSettingValue<bool>("high-res");
    int th = hr ? 1080 : 720;
    float r = (float)w / (float)h;
    int nh = th;
    int nw = FIX((int)(th * r));

    if (nw <= 0 || nh <= 0) return;
    if (g_recording && g_rw == nw && g_rh == nh) return;

    stop_work();
    std::lock_guard l(g_rec_m);
    if (g_recording && g_rec.isValid()) g_rec.stop();
    g_recording = false;

    auto dir = Mod::get()->getSaveDir() / "temp";
    std::filesystem::create_directories(dir);
    g_tmp = dir / fmt::format("{}.mp4", std::chrono::high_resolution_clock::now().time_since_epoch().count());

    ffmpeg::RenderSettings s;
    auto c = Mod::get()->getSettingValue<std::string>("codec");
    s.m_pixelFormat = ffmpeg::PixelFormat::RGB0;
    s.m_width = nw; s.m_height = nh;
    s.m_fps = (int)Mod::get()->getSettingValue<int64_t>("target-fps");
    s.m_bitrate = Mod::get()->getSettingValue<int64_t>("bitrate") * 1000000;
    s.m_outputFile = g_tmp.string();
    s.m_doVerticalFlip = true;

    std::vector<std::string> codes;
    if (c == "Auto") codes = {"h264_nvenc", "h264_amf", "h264_qsv", "libx264"};
    else codes = {c, "libx264"};

    bool ok = false;
    for (auto const& name : codes) {
        s.m_codec = name;
        if (g_rec.init(s).isOk()) {
            ok = true; break;
        }
    }

    if (ok) {
        g_rw = nw; g_rh = nh;
        g_f_count = 0;
        g_recording = true;
        g_worker = std::thread(work);
    }
}

static void save() {
    if (!g_recording || g_f_count == 0) return;
    Notification::create("Saving...", NotificationIcon::Info)->show();
    auto p = g_tmp;
    auto fc = g_f_count;
    auto ow = g_rw; auto oh = g_rh;
    History m = {"?", 0, 0};
    int64_t sf = 0;
    if (!g_hist.empty()) {
        m = g_hist.front();
        int count = (int)Mod::get()->getSettingValue<int64_t>("att-clip-count");
        sf = g_hist[std::min((size_t)count - 1, g_hist.size() - 1)].f;
    }
    stop_work();
    {
        std::lock_guard l(g_rec_m);
        if (g_rec.isValid()) g_rec.stop();
    }
    g_recording = false;
    std::thread([p, fc, sf, m]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto dir = Mod::get()->getSaveDir() / "clips";
        std::filesystem::create_directories(dir);
        auto out = dir / fmt::format("{}_att{}_{}.mp4", m.name, m.att, std::time(0));
        int fps = (int)Mod::get()->getSettingValue<int64_t>("target-fps");
        double t1 = (double)sf / fps;
        double t2 = (double)(fc - sf) / fps;

        bool hr = Mod::get()->getSettingValue<bool>("high-res");
        std::string filter = fmt::format("format=yuv420p,scale=-2:'min({},ih)'", hr ? 1080 : 720);

        auto cmd = fmt::format("ffmpeg -y -ss {:.3f} -i \"{}\" -t {:.3f} -vf \"{}\" -c:v libx264 -crf 23 -preset veryfast \"{}\"", 
            t1, p.string(), t2, filter, out.string());
        
        STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        if (CreateProcessA(0, (char*)cmd.c_str(), 0, 0, 0, CREATE_NO_WINDOW, 0, 0, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            std::filesystem::remove(p);
        }

        int64_t lim = Mod::get()->getSettingValue<int64_t>("storage-limit") * 1024LL * 1024LL * 1024LL;
        int64_t cur = 0; std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> l;
        for (auto const& e : std::filesystem::directory_iterator(dir)) {
            cur += (int64_t)e.file_size();
            l.push_back({e.last_write_time(), e.path()});
        }
        if (cur > lim) {
            std::sort(l.begin(), l.end());
            for (auto const& c : l) { if (cur <= lim) break; cur -= (int64_t)std::filesystem::file_size(c.second); std::filesystem::remove(c.second); }
        }
        Loader::get()->queueInMainThread([] { Notification::create("Saved!", NotificationIcon::Success)->show(); EchoClipGallery::refreshIfOpen(); });
    }).detach();
    go(ow, oh);
}

class $modify(M1, GJBaseGameLayer) {
    struct Fields {
        float t = 0; int lp = 0; GLuint b[2] = {0, 0}; int i = 0; bool ok = false; int lw = 0, lh = 0;
    };
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;
        int p = (int)m_gameState.m_currentProgress;
        if (p > m_fields->lp) { if (p > g_best) { g_best = p; if (Mod::get()->getSettingValue<bool>("clip-on-new-best")) save(); } m_fields->lp = p; }
        if (auto i = this->getChildByID("rec-indicator")) i->setVisible(g_recording);
        if (!g_recording) return;

        float iv = 1.0f / (float)Mod::get()->getSettingValue<int64_t>("target-fps");
        m_fields->t += dt;
        if (m_fields->t >= iv) {
            GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
            int nw = FIX(vp[2]), nh = FIX(vp[3]);
            if (!m_fields->ok || m_fields->lw != nw || m_fields->lh != nh) {
                if (m_fields->ok) glDeleteBuffers(2, m_fields->b);
                glGenBuffers(2, m_fields->b);
                for (int j = 0; j < 2; j++) { glBindBuffer(GL_PIXEL_PACK_BUFFER, m_fields->b[j]); glBufferData(GL_PIXEL_PACK_BUFFER, (size_t)nw * nh * 4, 0, GL_STREAM_READ); }
                m_fields->ok = true; m_fields->lw = nw; m_fields->lh = nh;
            }
            int next = (m_fields->i + 1) % 2;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_fields->b[m_fields->i]);
            glReadPixels(0, 0, nw, nh, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_fields->b[next]);
            if (auto ptr = (uint8_t*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)) {
                size_t sz = (size_t)nw * nh * 4;
                std::vector<uint8_t> data;
                bool copied = false;
                std::lock_guard lock(g_q_m);
                while (m_fields->t >= iv) {
                    m_fields->t -= iv;
                    if (g_q.size() < 5) {
                        if (!copied) { data.assign(ptr, ptr + sz); copied = true; }
                        g_q.push_back(data); g_f_count++;
                    }
                }
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); m_fields->i = next; g_cv.notify_one();
        }
    }
};

class $modify(M2, PlayLayer) {
    bool init(GJGameLevel* level, bool p1, bool p2) {
        if (!PlayLayer::init(level, p1, p2)) return false;
        g_best = 0;
        auto sz = CCDirector::get()->getWinSize();
        auto m = CCMenu::create(); m->setID("rec-indicator"); m->setPosition({sz.width - 40, sz.height - 15}); m->setVisible(false);
        auto lbl = CCLabelBMFont::create("REC", "chatFont.fnt"); lbl->setColor({255, 50, 50}); lbl->setScale(0.5f); lbl->setPositionX(12);
        auto d = CCLabelTTF::create("●", "Arial", 14); d->setColor({255, 50, 50}); d->runAction(CCRepeatForever::create(CCSequence::create(CCFadeTo::create(0.6f, 50), CCFadeTo::create(0.6f, 255), 0)));
        m->addChild(lbl); m->addChild(d); this->addChild(m, 999);
        return true;
    }
    void startGame() { PlayLayer::startGame(); if (Mod::get()->getSettingValue<bool>("enabled")) { GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); go(vp[2], vp[3]); } }
    void resetLevel() { if (auto pl = PlayLayer::get()) { if (pl->m_level) { std::lock_guard l(g_q_m); g_hist.push_front({pl->m_level->m_levelName, pl->m_attempts, g_f_count}); if (g_hist.size() > 50) g_hist.pop_back(); } } PlayLayer::resetLevel(); }
};

$on_mod(Loaded) {
    auto tmp = Mod::get()->getSaveDir() / "temp"; if (std::filesystem::exists(tmp)) std::filesystem::remove_all(tmp);
    listenForKeybindSettingPresses("clip-keybind", [](auto&, bool down, bool rep, double) { if (down && !rep) save(); });
}