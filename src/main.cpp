#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <eclipse.ffmpeg-api/include/events.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/string.hpp>
#include "ui.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <ctime>

#ifdef GEODE_IS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <shellapi.h>
#endif

using namespace geode::prelude;
namespace fs = std::filesystem;

// axiom was here
// i hate this project so much why did i start this
double get_time_val() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool check_vram_low() { // i know someonme will complain about low fps so i added this 
#ifdef GEODE_IS_WINDOWS
    IDXGIFactory* f; if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&f))) return false;
    IDXGIAdapter* a; if (FAILED(f->EnumAdapters(0, &a))) { f->Release(); return false; }
    DXGI_ADAPTER_DESC d; a->GetDesc(&d);
    size_t mb = d.DedicatedVideoMemory / (1024 * 1024);
    a->Release(); f->Release();
    return mb < 2048; // lowered this because 6gb was crazy lol, wait but if someone is on an igpu, dont they have like 256mb or smh? meh not my problem
#endif
    return false;
}

bool check_cpu_bad() {
    return std::thread::hardware_concurrency() < 4; // bro if u have < 4 cores in 2026 just give up
}

std::string get_codec() { // if 1 PERSON SAYS "android when" im banning them from my server
    // this is NEVER comming to other platforms.. maybe, ok but the only one thta could maybe work is mocos
    static std::string cached_codec = "";
    if (!cached_codec.empty()) return cached_codec;
    char* sz_vendor_ptr = (char*)glGetString(GL_VENDOR);
    if (!sz_vendor_ptr) return "libx264";
    std::string vStr = geode::utils::string::toLower(sz_vendor_ptr);
    if (vStr.find("nvidia") != std::string::npos) cached_codec = "h264_nvenc";
    else if (vStr.find("amd") != std::string::npos || vStr.find("ati") != std::string::npos || vStr.find("advanced micro") != std::string::npos) cached_codec = "h264_amf";
    else if (vStr.find("intel") != std::string::npos) cached_codec = "h264_qsv";
    else cached_codec = "libx264";
    return cached_codec;
}

// u know what, thank god gd uses opengl vulkan would be hell
void save_clip(fs::path srcPath, std::string sLvlName, int nAttempts) {
    std::error_code ec;
    if (srcPath.empty() || !fs::exists(srcPath, ec)) return;
    geode::async::spawn([srcPath, sLvlName, nAttempts]() -> arc::Future<> {
        std::error_code ec;
        fs::path p_save_dir = Mod::get()->getSaveDir() / "clips";
        fs::create_directories(p_save_dir, ec);
        std::string clean_name = sLvlName;
        for (char& c : clean_name)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        if (clean_name.size() > 40) clean_name = clean_name.substr(0, 40);

        fs::path out_file_path = p_save_dir / fmt::format("{}_att{}_{}.mp4", clean_name, nAttempts, (long long)::time(0));
        fs::path tmp_out = p_save_dir / fmt::format("_tmp_{}.mp4", (long long)::time(0));
        
        std::string codec = get_codec();
        std::string ff_cmd = fmt::format("ffmpeg -y -i \"{}\" -vf null -c:v {} -preset ultrafast -crf 23 -pix_fmt yuv420p -movflags +faststart \"{}\"", 
            srcPath.string(), codec, tmp_out.string());

        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessA(NULL, (char*)ff_cmd.c_str(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        if (fs::exists(tmp_out, ec)) {
            fs::remove(srcPath, ec); fs::rename(tmp_out, out_file_path, ec);
        } else {
            fs::rename(srcPath, out_file_path, ec);
        }

        uintmax_t max_bytes = (uintmax_t)Mod::get()->getSettingValue<int64_t>("storage-limit") * 1024 * 1024 * 1024;
        std::vector<fs::path> vFiles;
        for (auto const& e : fs::directory_iterator(p_save_dir, ec))
            if (e.path().extension() == ".mp4" || e.path().extension() == ".mkv") vFiles.push_back(e.path());
        std::sort(vFiles.begin(), vFiles.end(), [](fs::path a, fs::path b) { 
            std::error_code e1, e2;
            return fs::last_write_time(a, e1) < fs::last_write_time(b, e2); 
        });
        uintmax_t nTotal = 0;
        for (auto const& f : vFiles) {
            std::error_code e;
            nTotal += fs::file_size(f, e);
        }
        for (size_t i = 0; i < vFiles.size() && nTotal > max_bytes; i++) {
            std::error_code e;
            nTotal -= fs::file_size(vFiles[i], e); fs::remove(vFiles[i], e);
        }

        Loader::get()->queueInMainThread([] {
            if (!CCDirector::get()->getRunningScene()) return;
            Notification::create("Clip Saved!", CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png"))->show();
            Gallery::refresh();
        });
        co_return;
    });
}

class $modify(MyBaseGameLayer, GJBaseGameLayer) {
    struct Fields {
        ffmpeg::events::Recorder* rec = nullptr;
        bool active = false;
        int nW = 0, nH = 0;
        fs::path temp_file_p;
        std::string s_lvl_str;
        int n_att_count = 0, best_percent = 0;
        float f_timer_val = 0;

        GLuint pbo_bufer[3] = {0, 0, 0};
        GLsync fences_sync_ptr[3] = {0, 0, 0};
        int write_idx = 0, n_pushed_frames = 0;
        bool b_setup_done = false;
        bool b_capture_this_frame = false;

        GLuint downscale_fbo = 0;
        GLuint downscale_tex = 0;

        float gap_cache = 0.01666f;
        bool clip_new_best = false;

        std::thread* p_worker_thread = nullptr;
        std::queue<std::vector<uint8_t>> c_pixel_q;
        std::vector<std::vector<uint8_t>> pool_frames;
        std::mutex m_q_mtx;
        std::mutex m_p_mtx;
        std::condition_variable m_cv;
        bool dead = false;

        ~Fields() {
            dead = true; m_cv.notify_all();
            if (p_worker_thread) { if (p_worker_thread->joinable()) p_worker_thread->join(); delete p_worker_thread; }
            if (rec) { rec->stop(); delete rec; }
            for (int i = 0; i < 3; i++) if (fences_sync_ptr[i]) { glDeleteSync(fences_sync_ptr[i]); fences_sync_ptr[i] = 0; }
            if (downscale_fbo) glDeleteFramebuffers(1, &downscale_fbo);
            if (downscale_tex) glDeleteTextures(1, &downscale_tex);
        }
    };

    void trigger_clip() {
        Fields* f = m_fields.self();
        if (f->active) {
            fs::path p = kill_rec();
            std::error_code ec;
            if (!p.empty() && fs::exists(p, ec)) {
                Notification::create("Clipping...", CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png"))->show();
                save_clip(p, f->s_lvl_str, f->n_att_count);
                auto frameSize = CCDirector::get()->getOpenGLView()->getFrameSize();
                start_rec((int)frameSize.width, (int)frameSize.height);
            }
        }
    }

    void start_rec(int srcW, int srcH) {
        kill_rec();
        int scale = (int)std::min((int64_t)100, Mod::get()->getSettingValue<int64_t>("recording-scale"));
        int w_res = (srcW * scale / 100) & ~1;
        int h_res = (srcH * scale / 100) & ~1;
        int sz_bytes = w_res * h_res * 4;
        
        m_fields->gap_cache = 1.f / (float)Mod::get()->getSettingValue<int64_t>("target-fps");
        m_fields->clip_new_best = Mod::get()->getSettingValue<bool>("clip-on-new-best");

        std::error_code ec;
        fs::path d = Mod::get()->getSaveDir() / "temp";
        fs::create_directories(d, ec);
        m_fields->temp_file_p = d / fmt::format("r_{}_{}.mp4", (int)get_time_val(), rand() % 100);

        ffmpeg::RenderSettings config;
        config.m_height = h_res; config.m_width = w_res;
        config.m_fps = (uint16_t)Mod::get()->getSettingValue<int64_t>("target-fps");
        config.m_bitrate = std::min((int64_t)8000000, Mod::get()->getSettingValue<int64_t>("bitrate") * 1000000);
        config.m_outputFile = m_fields->temp_file_p;
        config.m_codec = get_codec();
        config.m_pixelFormat = ffmpeg::PixelFormat::BGRA;
        config.m_doVerticalFlip = true;

        ffmpeg::events::Recorder* p_rec = new ffmpeg::events::Recorder();
        auto res = p_rec->init(config);
        if (res.isOk()) {
            m_fields->rec = p_rec; m_fields->nW = w_res; m_fields->nH = h_res;
            m_fields->active = true; m_fields->dead = false; m_fields->n_pushed_frames = 0;
            Fields* f = m_fields.self();

            glGenFramebuffers(1, &f->downscale_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, f->downscale_fbo);
            glGenTextures(1, &f->downscale_tex);
            glBindTexture(GL_TEXTURE_2D, f->downscale_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w_res, h_res, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->downscale_tex, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            {
                std::lock_guard<std::mutex> l(f->m_p_mtx);
                f->pool_frames.clear();
                for (int i = 0; i < 24; i++) f->pool_frames.push_back(std::vector<uint8_t>(sz_bytes));
            }

            m_fields->p_worker_thread = new std::thread([f, p_rec]() {
                while (true) {
                    std::vector<uint8_t> c_pixel;
                    {
                        std::unique_lock<std::mutex> lk(f->m_q_mtx);
                        f->m_cv.wait(lk, [f] { return f->dead || !f->c_pixel_q.empty(); });
                        if (f->dead && f->c_pixel_q.empty()) break;
                        c_pixel = std::move(f->c_pixel_q.front()); f->c_pixel_q.pop();
                    }
                    (void)p_rec->writeFrame(c_pixel);
                    std::lock_guard<std::mutex> l(f->m_p_mtx);
                    if (f->pool_frames.size() < 24) f->pool_frames.push_back(std::move(c_pixel));
                }
            });
            if (m_fields->b_setup_done) cleanup_gl();
        } else {
            delete p_rec;
        }
    }

    fs::path kill_rec() {
        if (!m_fields->active) return "";
        m_fields->active = false;
        fs::path p_temp = m_fields->temp_file_p;
        m_fields->dead = true; m_fields->m_cv.notify_all();
        if (m_fields->p_worker_thread) { if (m_fields->p_worker_thread->joinable()) m_fields->p_worker_thread->join(); delete m_fields->p_worker_thread; m_fields->p_worker_thread = nullptr; }
        if (m_fields->rec) { m_fields->rec->stop(); delete m_fields->rec; m_fields->rec = nullptr; }
        {
            std::lock_guard<std::mutex> l(m_fields->m_q_mtx);
            while (!m_fields->c_pixel_q.empty()) m_fields->c_pixel_q.pop();
        }
        {
            std::lock_guard<std::mutex> l(m_fields->m_p_mtx);
            m_fields->pool_frames.clear();
        }
        if (m_fields->downscale_fbo) { glDeleteFramebuffers(1, &m_fields->downscale_fbo); m_fields->downscale_fbo = 0; }
        if (m_fields->downscale_tex) { glDeleteTextures(1, &m_fields->downscale_tex); m_fields->downscale_tex = 0; }
        return p_temp;
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
        if (!f->active) return;
        
        f->f_timer_val += dt;
        if (f->f_timer_val >= f->gap_cache) {
            f->f_timer_val = fmodf(f->f_timer_val, f->gap_cache);
            f->b_capture_this_frame = true;
            std::lock_guard<std::mutex> l(f->m_q_mtx);
            if (f->c_pixel_q.size() > 8) f->b_capture_this_frame = false;
        }
    }

    void onExit() { kill_rec(); cleanup_gl(); GJBaseGameLayer::onExit(); }
};

$execute {
    listenForKeybindSettingPresses("clip-keybind", [](geode::Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            if (Mod::get()->getSettingValue<bool>("enabled")) {
                if (auto layer = GJBaseGameLayer::get()) {
                    static_cast<MyBaseGameLayer*>(layer)->trigger_clip();
                }
            }
        }
    });
}

class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        auto layer = GJBaseGameLayer::get();
        if (layer) {
            auto f = static_cast<MyBaseGameLayer*>(layer)->m_fields.self();
            if (f->active && f->b_capture_this_frame) {
                f->b_capture_this_frame = false;
                int recW = f->nW; int recH = f->nH;
                int sz_bytes = recW * recH * 4;
                
                auto fs = CCDirector::get()->getOpenGLView()->getFrameSize();
                int winW = (int)fs.width; int winH = (int)fs.height;

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
                int readIdx = (writeIdx + 1) % 3;
                f->write_idx = (writeIdx + 1) % 3;

                glBindBuffer(GL_PIXEL_PACK_BUFFER, f->pbo_bufer[writeIdx]);
                glReadPixels(0, 0, recW, recH, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                if (f->fences_sync_ptr[writeIdx]) { glDeleteSync(f->fences_sync_ptr[writeIdx]); f->fences_sync_ptr[writeIdx] = 0; }
                f->fences_sync_ptr[writeIdx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                glFlush();

                if (++f->n_pushed_frames >= 3 && f->fences_sync_ptr[readIdx]) {
                    GLenum status = glClientWaitSync(f->fences_sync_ptr[readIdx], 0, 0); 
                    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
                        glDeleteSync(f->fences_sync_ptr[readIdx]); f->fences_sync_ptr[readIdx] = 0;
                        glBindBuffer(GL_PIXEL_PACK_BUFFER, f->pbo_bufer[readIdx]);
                        void* p_pix = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sz_bytes, GL_MAP_READ_BIT);
                        if (p_pix) {
                            std::vector<uint8_t> c_pixel;
                            {
                                std::lock_guard<std::mutex> l(f->m_p_mtx);
                                if (!f->pool_frames.empty()) { c_pixel = std::move(f->pool_frames.back()); f->pool_frames.pop_back(); }
                            }
                            if (c_pixel.size() != (size_t)sz_bytes) c_pixel.resize(sz_bytes);
                            memcpy(c_pixel.data(), p_pix, sz_bytes);
                            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                            
                            std::lock_guard<std::mutex> l(f->m_q_mtx);
                            if (f->c_pixel_q.size() < 12) { f->c_pixel_q.push(std::move(c_pixel)); f->m_cv.notify_one(); }
                            else {
                                std::lock_guard<std::mutex> lp(f->m_p_mtx);
                                if (f->pool_frames.size() < 24) f->pool_frames.push_back(std::move(c_pixel));
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
        PlayLayer::startGame(); if (!Mod::get()->getSettingValue<bool>("enabled") || !m_level) return;
        
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

        MyBaseGameLayer::Fields* f = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->m_fields.self();
        f->s_lvl_str = m_level->m_levelName; f->n_att_count = m_level->m_attempts; f->best_percent = m_level->m_normalPercent;
        auto frameSize = CCDirector::get()->getOpenGLView()->getFrameSize();
        static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->start_rec((int)frameSize.width, (int)frameSize.height);
    }
    void resetLevel() { PlayLayer::resetLevel(); if (auto f = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->m_fields.self()) f->n_att_count = m_level ? m_level->m_attempts : 0; }
    void levelComplete() {
        PlayLayer::levelComplete();
        auto f = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->m_fields.self();
        if (!f || !f->clip_new_best || !m_level) return;
        if (m_level->m_normalPercent <= f->best_percent) return;
        f->best_percent = m_level->m_normalPercent;
        fs::path p = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->kill_rec();
        if (!p.empty()) { save_clip(p, f->s_lvl_str, f->n_att_count); auto fs = CCDirector::get()->getOpenGLView()->getFrameSize(); static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->start_rec((int)fs.width, (int)fs.height); }
    }
    void destroyPlayer(PlayerObject* boi, GameObject* obj) {
        PlayLayer::destroyPlayer(boi, obj);
        auto f = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->m_fields.self();
        if (!f || !f->clip_new_best || !m_player1 || !m_level) return;
        int cur = (int)(m_player1->getPositionX() / m_levelLength * 100.f); if (cur <= f->best_percent) return;
        f->best_percent = cur; fs::path p = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->kill_rec();
        if (!p.empty()) { save_clip(p, f->s_lvl_str, f->n_att_count); auto fs = CCDirector::get()->getOpenGLView()->getFrameSize(); static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this))->start_rec((int)fs.width, (int)fs.height); }
    }
};
