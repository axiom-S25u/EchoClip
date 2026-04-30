#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <eclipse.ffmpeg-api/include/events.hpp>
#include <Geode/utils/string.hpp>
#include "common/common.hpp"
#include "win/win.hpp"
#include "mac/mac.hpp"
#include "ui.hpp"
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <ctime>

#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif

using namespace geode::prelude;
namespace fs = std::filesystem;

// axiom was here
// i hate this project so much why did i start this, at least it has features now
struct RecSession {
    ffmpeg::events::Recorder* rec = nullptr;
    std::thread* p_worker_thread = nullptr;
    std::queue<std::vector<uint8_t>> c_pixel_q;
    std::vector<std::vector<uint8_t>> pool_frames;
    std::mutex m_q_mtx;
    std::mutex m_p_mtx;
    std::condition_variable m_cv;
    bool dead = false;
    int max_frames = 30;
    fs::path temp_file_p;
    bool b_saved = false;
    int fps = 30;
    std::atomic<int> frames_written{0};

    ~RecSession() {
        dead = true; m_cv.notify_all();
        if (p_worker_thread) {
            if (p_worker_thread->joinable()) {
                if (std::this_thread::get_id() == p_worker_thread->get_id()) p_worker_thread->detach();
                else p_worker_thread->join();
            }
            delete p_worker_thread;
        }
        if (rec) { rec->stop(); delete rec; }

        if (!b_saved && !temp_file_p.empty()) {
            std::error_code ec;
            fs::remove(temp_file_p, ec);
        }
    }
};

void finalize_and_save(std::shared_ptr<RecSession> s, std::string lvl, int att) {
    if (!s || s->temp_file_p.empty()) return;

    s->dead = true;
    s->m_cv.notify_all();
    if (s->p_worker_thread && s->p_worker_thread->joinable())
        s->p_worker_thread->join();

    if (s->rec) {
        s->rec->stop();
        delete s->rec;
        s->rec = nullptr;
    }

    std::error_code ec;
    if (!fs::exists(s->temp_file_p, ec)) {
        Notification::create("Temp file missing!", CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png"))->show();
        return;
    }

    s->b_saved = true;
    Notification::create("Clipping...", CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png"))->show();

    int fwritten = s->frames_written.load();
    int fps = s->fps > 0 ? s->fps : 30;
    double dur = (double)fwritten / (double)fps;
    save_clip(s->temp_file_p, lvl, att);
}

class $modify(MyBaseGameLayer, GJBaseGameLayer) {
    struct Fields {
        std::shared_ptr<RecSession> session;
        bool active = false;
        int nW = 0, nH = 0;
        int prev_downscale_w = 0, prev_downscale_h = 0;
        std::string s_lvl_str;
        int n_att_count = 1, best_percent = 0;
        float f_timer_val = 0;

        GLuint pbo_bufer[3] = {0, 0, 0};
        GLsync fences_sync_ptr[3] = {0, 0, 0};
        int write_idx = 0;

        int n_pushed_frames = 0;
        bool b_setup_done = false;
        bool b_capture_this_frame = false;

        GLuint downscale_fbo = 0;
        GLuint downscale_tex = 0;

        float gap_cache = 0.01666f;
        bool clip_new_best = false;
        int current_rec_att = 1;

        ~Fields() {
            if (session) { session->dead = true; session->m_cv.notify_all(); }
            for (int i = 0; i < 3; i++) if (fences_sync_ptr[i]) { glDeleteSync(fences_sync_ptr[i]); fences_sync_ptr[i] = 0; }
            if (downscale_fbo) glDeleteFramebuffers(1, &downscale_fbo);
            if (downscale_tex) glDeleteTextures(1, &downscale_tex);
        }
    };

    void trigger_clip() {
        Fields* f = m_fields.self();
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;
        if (!f->active || !f->session) {
            geode::log::warn("trigger_clip called but not active");
            return;
        }
        std::shared_ptr<RecSession> s = kill_rec();
        if (s) {
            finalize_and_save(s, f->s_lvl_str, f->current_rec_att);
            int w = 0, h = 0;
            get_target_rec_size(w, h);
            start_rec(w, h);
        }
    }

    void start_rec(int recW, int recH) {
        kill_rec();

        if (m_isPracticeMode && !Mod::get()->getSettingValue<bool>("record-practice")) return;
        if (m_isTestMode && !Mod::get()->getSettingValue<bool>("record-startpos")) return;

        int sz_bytes = recW * recH * 4;

        m_fields->gap_cache = 1.f / (float)Mod::get()->getSettingValue<int64_t>("target-fps");
        m_fields->clip_new_best = Mod::get()->getSettingValue<bool>("clip-on-new-best");

        int64_t ram_mb = Mod::get()->getSettingValue<int64_t>("max-ram-usage");
        int64_t sys_ram = get_total_ram_mb();
        if (ram_mb > (sys_ram * 8 / 10)) ram_mb = sys_ram * 8 / 10;
        if (ram_mb > 4096) ram_mb = 4096;

        int max_f = sz_bytes > 0 ? std::max(10, (int)((ram_mb * 1024 * 1024) / sz_bytes)) : 30;

        std::error_code ec;
        fs::path d = Mod::get()->getSaveDir() / "temp";
        fs::create_directories(d, ec);
        fs::path temp_p = d / fmt::format("r_{}_{}.mp4", (int)get_time_val(), rand() % 100000);

        std::vector<std::string> codecs_to_try;
        std::string preferred = get_codec();
        codecs_to_try.push_back(preferred);
        if (preferred != "libx264") codecs_to_try.push_back("libx264");

        ffmpeg::events::Recorder* p_rec = nullptr;
        std::string working_codec = "";

        for (std::string const& codec : codecs_to_try) {
            ffmpeg::RenderSettings config;
            config.m_height = recH; config.m_width = recW;
            config.m_fps = (uint16_t)Mod::get()->getSettingValue<int64_t>("target-fps");
            config.m_bitrate = 15000000;
            config.m_outputFile = temp_p;
            config.m_codec = codec;
            config.m_pixelFormat = ffmpeg::PixelFormat::BGRA;
            config.m_doVerticalFlip = true;

            ffmpeg::events::Recorder* candidate = new ffmpeg::events::Recorder();
            auto res = candidate->init(config);
            if (res.isOk()) {
                p_rec = candidate;
                working_codec = codec;
                break;
            }
            geode::log::warn("codec {} failed to init, trying next", codec);
            delete candidate;
        }

        if (!p_rec) {
            geode::log::error("all codecs failed, recording disabled for this attempt");
            return;
        }

        if (working_codec != preferred) {
            Loader::get()->queueInMainThread([working_codec] {
                Notification::create(fmt::format("Fallback codec: {}", working_codec), CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png"), 3.f)->show();
            });
        }

        std::shared_ptr<RecSession> s = std::make_shared<RecSession>();
        s->rec = p_rec; s->max_frames = max_f; s->temp_file_p = temp_p;
    s->fps = (int)Mod::get()->getSettingValue<int64_t>("target-fps");
        m_fields->session = s;
        m_fields->nW = recW; m_fields->nH = recH;
        m_fields->active = true; m_fields->n_pushed_frames = 0;
        Fields* f = m_fields.self();

        if (f->downscale_fbo && f->prev_downscale_w == recW && f->prev_downscale_h == recH) {
        } else {
            if (f->downscale_fbo) { glDeleteFramebuffers(1, &f->downscale_fbo); f->downscale_fbo = 0; }
            if (f->downscale_tex) { glDeleteTextures(1, &f->downscale_tex); f->downscale_tex = 0; }
            glGenFramebuffers(1, &f->downscale_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, f->downscale_fbo);
            glGenTextures(1, &f->downscale_tex);
            glBindTexture(GL_TEXTURE_2D, f->downscale_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, recW, recH, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->downscale_tex, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            f->prev_downscale_w = recW;
            f->prev_downscale_h = recH;
        }

        {
            std::lock_guard<std::mutex> l(s->m_p_mtx);
            s->pool_frames.clear();
            int pre = std::min(s->max_frames, 60);
            if (s->max_frames > 200) pre = std::max(pre, s->max_frames / 4);
            for (int i = 0; i < pre; i++) s->pool_frames.push_back(std::vector<uint8_t>(sz_bytes));
        }

        s->p_worker_thread = new std::thread([s]() {
            while (true) {
                std::vector<uint8_t> c_pixel;
                {
                    std::unique_lock<std::mutex> lk(s->m_q_mtx);
                    s->m_cv.wait(lk, [s] { return s->dead || !s->c_pixel_q.empty(); });
                    if (s->dead && s->c_pixel_q.empty()) break;
                    c_pixel = std::move(s->c_pixel_q.front()); s->c_pixel_q.pop();
                }
                if (s->rec->writeFrame(c_pixel).isOk()) s->frames_written.fetch_add(1);
                std::lock_guard<std::mutex> l(s->m_p_mtx);
                std::lock_guard<std::mutex> lq(s->m_q_mtx);
                if ((int)(s->pool_frames.size() + s->c_pixel_q.size()) < s->max_frames) {
                    s->pool_frames.push_back(std::move(c_pixel));
                }
            }
        });
        if (m_fields->b_setup_done) cleanup_gl();
    }

    std::shared_ptr<RecSession> kill_rec() {
        if (!m_fields->active || !m_fields->session) return nullptr;
        m_fields->active = false;
        std::shared_ptr<RecSession> s = m_fields->session;
        m_fields->session = nullptr;
        s->dead = true; s->m_cv.notify_all();
        return s;
    }

    void cleanup_gl() {
        if (!m_fields->b_setup_done) return;
        glDeleteBuffers(3, m_fields->pbo_bufer);
        for (int i = 0; i < 3; i++) if (m_fields->fences_sync_ptr[i]) { glDeleteSync(m_fields->fences_sync_ptr[i]); m_fields->fences_sync_ptr[i] = 0; }
        m_fields->b_setup_done = false;
    }

    void update(float dt) {
        GJBaseGameLayer::update(dt);
        Fields* f = m_fields.self();
        if (!f->active || !f->session) return;
        std::shared_ptr<RecSession> s = f->session;

        f->f_timer_val += dt;
        while (f->f_timer_val >= f->gap_cache) {
            f->f_timer_val -= f->gap_cache;
            f->b_capture_this_frame = true;
            std::lock_guard<std::mutex> l(s->m_q_mtx);
            if ((int)s->c_pixel_q.size() > (int)(s->max_frames * 0.8)) f->b_capture_this_frame = false;
        }
    }
};

$execute {
    cleanup_temp_folder();
    listenForKeybindSettingPresses("clip-keybind", [](geode::Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            if (Mod::get()->getSettingValue<bool>("enabled")) {
                if (auto bgl = GJBaseGameLayer::get()) {
                    static_cast<MyBaseGameLayer*>(bgl)->trigger_clip();
                }
            }
        }
    });
}

class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        auto layer = GJBaseGameLayer::get();
        if (layer) {
            MyBaseGameLayer::Fields* f = static_cast<MyBaseGameLayer*>(layer)->m_fields.self();
            if (f->active && f->session && f->b_capture_this_frame) {
                std::shared_ptr<RecSession> s = f->session;
                f->b_capture_this_frame = false;
                int recW = f->nW; int recH = f->nH;
                int sz_bytes = recW * recH * 4;

                // mac retina lies in getFrameSize, ask GL directly so blit src is real pixels
                GLint vp[4] = {0, 0, 0, 0};
                glGetIntegerv(GL_VIEWPORT, vp);
                int winW = vp[2];
                int winH = vp[3];
                if (winW <= 0 || winH <= 0) {
                    CCSize fs2 = CCDirector::get()->getOpenGLView()->getFrameSize();
                    winW = (int)fs2.width; winH = (int)fs2.height;
                }
#ifdef GEODE_IS_MACOS
                // bro retina displays are FUCKED the viewport is in POINTS not PIXELS
                // only reason i added macos support is more damn downloads, regret already
                extern float get_macos_backing_scale();
                float scale = get_macos_backing_scale();
                winW = (int)(winW * scale);
                winH = (int)(winH * scale);
#endif

                if (!f->b_setup_done) {
                    glGenBuffers(3, f->pbo_bufer);
                    for (int i = 0; i < 3; i++) {
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, f->pbo_bufer[i]);
                        glBufferData(GL_PIXEL_PACK_BUFFER, sz_bytes, nullptr, GL_DYNAMIC_READ);
                        f->fences_sync_ptr[i] = 0;
                    }
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    f->b_setup_done = true;
                }

                if (winW != recW || winH != recH) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->downscale_fbo);
                    glBlitFramebuffer(0, 0, winW, winH, 0, 0, recW, recH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->downscale_fbo);
                } else {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                }

                int writeIdx = f->write_idx;
                int readIdx = (writeIdx + 2) % 3;
                f->write_idx = (writeIdx + 1) % 3;

                glBindBuffer(GL_PIXEL_PACK_BUFFER, f->pbo_bufer[writeIdx]);
                glReadPixels(0, 0, recW, recH, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                if (f->fences_sync_ptr[writeIdx]) { glDeleteSync(f->fences_sync_ptr[writeIdx]); f->fences_sync_ptr[writeIdx] = 0; }
                f->fences_sync_ptr[writeIdx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                glFlush();

                if (++f->n_pushed_frames > 3 && f->fences_sync_ptr[readIdx]) {
                    GLint signaled = 0;
                    glGetSynciv(f->fences_sync_ptr[readIdx], GL_SYNC_STATUS, 1, nullptr, &signaled);
                    if (signaled == GL_SIGNALED) {
                        glDeleteSync(f->fences_sync_ptr[readIdx]); f->fences_sync_ptr[readIdx] = 0;
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, f->pbo_bufer[readIdx]);
#ifdef GEODE_IS_MACOS
                        void* p_pix = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
#else
                        void* p_pix = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sz_bytes, GL_MAP_READ_BIT);
#endif
                        if (p_pix) {
                            std::vector<uint8_t> c_pixel;
                            {
                                std::lock_guard<std::mutex> l(s->m_p_mtx);
                                if (!s->pool_frames.empty()) { c_pixel = std::move(s->pool_frames.back()); s->pool_frames.pop_back(); }
                            }
                            if ((int)c_pixel.size() != sz_bytes) c_pixel.resize(sz_bytes);
                            memcpy(c_pixel.data(), p_pix, sz_bytes);
                            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

                            std::lock_guard<std::mutex> lp(s->m_p_mtx);
                            std::lock_guard<std::mutex> lq(s->m_q_mtx);
                            if ((int)s->c_pixel_q.size() < s->max_frames) {
                                s->c_pixel_q.push(std::move(c_pixel));
                                s->m_cv.notify_one();
                            } else if ((int)s->pool_frames.size() < s->max_frames) {
                                s->pool_frames.push_back(std::move(c_pixel));
                            }
                        }
                    }
                }
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }
        }
        CCEGLView::swapBuffers();
    }
};

class $modify(MyPlayLayer, PlayLayer) {
    void startGame() {
        PlayLayer::startGame();
        if (!Mod::get()->getSettingValue<bool>("enabled") || !m_level) return;

        if (m_isPracticeMode && !Mod::get()->getSettingValue<bool>("record-practice")) return;
        if (m_isTestMode && !Mod::get()->getSettingValue<bool>("record-startpos")) return;

        static bool s_warned = false;
        if (!s_warned) {
            if (check_vram_low()) {
                Notification::create("Low VRAM! Reduce Scale to avoid lag.", CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png"), 5.f)->show();
                s_warned = true;
            } else if (check_cpu_bad()) {
                Notification::create("Bad CPU! Recording will probably lag.", CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png"), 5.f)->show();
                s_warned = true;
            } else if (get_codec() == "libx264") {
                Notification::create("No GPU Encoder! CPU usage will be high.", CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png"), 5.f)->show();
                s_warned = true;
            }
        }

        if (Mod::get()->getSettingValue<bool>("show-encoder")) {
            Notification::create(fmt::format("Encoder: {}", get_codec()), CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png"))->show();
        }

        MyBaseGameLayer* bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        MyBaseGameLayer::Fields* f = bgl->m_fields.self();
        f->s_lvl_str = m_level->m_levelName;
        f->n_att_count = m_level->m_attempts;
        f->current_rec_att = f->n_att_count;
        f->best_percent = m_level->m_normalPercent;
        int w = 0, h = 0;
        get_target_rec_size(w, h);
        bgl->start_rec(w, h);
    }

    void togglePracticeMode(bool practice) {
        PlayLayer::togglePracticeMode(practice);
        MyBaseGameLayer* bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        if (practice && !Mod::get()->getSettingValue<bool>("record-practice")) {
            bgl->kill_rec();
        } else if (!practice && Mod::get()->getSettingValue<bool>("enabled")) {
            int w = 0, h = 0;
            get_target_rec_size(w, h);
            bgl->start_rec(w, h);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        MyBaseGameLayer* bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        MyBaseGameLayer::Fields* f = bgl->m_fields.self();
        if (!f) return;

        f->n_att_count = m_level ? m_level->m_attempts : 0;

        if (f->active) {
            bgl->kill_rec();
            f->current_rec_att = f->n_att_count;
            int w = 0, h = 0;
            get_target_rec_size(w, h);
            bgl->start_rec(w, h);
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        MyBaseGameLayer* bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        MyBaseGameLayer::Fields* f = bgl->m_fields.self();
        if (!f || !m_level) return;
        std::shared_ptr<RecSession> s = bgl->kill_rec();
        if (s) {
            finalize_and_save(s, f->s_lvl_str, f->current_rec_att);
            int w = 0, h = 0;
            get_target_rec_size(w, h);
            bgl->start_rec(w, h);
        }
    }

    void destroyPlayer(PlayerObject* boi, GameObject* obj) {
        PlayLayer::destroyPlayer(boi, obj);
        MyBaseGameLayer* bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        MyBaseGameLayer::Fields* f = bgl->m_fields.self();
        if (!f || !f->clip_new_best || !m_player1 || !m_level) return;
        int cur = (int)this->getCurrentPercent();
        if (cur <= f->best_percent) return;
        f->best_percent = cur;
        std::shared_ptr<RecSession> s = bgl->kill_rec();
        if (s) {
            finalize_and_save(s, f->s_lvl_str, f->current_rec_att);
            int w = 0, h = 0;
            get_target_rec_size(w, h);
            bgl->start_rec(w, h);
        }
    }

    void onExit() {
        PlayLayer::onExit();
        MyBaseGameLayer* bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        bgl->kill_rec();
        bgl->cleanup_gl();
    }
};

class $modify(MyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = getChildByID("right-button-menu");
        if (!menu) menu = getChildByID("left-button-menu");
        if (!menu) return;

        CCSprite* spr = CCSprite::create("gallery.png"_spr);
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
        spr->setScale(0.35f);

        CCMenuItemSpriteExtra* btn = CCMenuItemSpriteExtra::create(spr, nullptr, this, menu_selector(MyPauseLayer::onGallery));
        btn->setID("gallery-btn"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onGallery(CCObject*) {
        Gallery::open();
    }
};
