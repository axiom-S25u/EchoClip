#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/PauseLayer.hpp>
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

#ifdef GEODE_IS_ANDROID
#include <fstream>
#include <sys/sysinfo.h>
#include <EGL/egl.h>
typedef void (*PFNGLBLITFRAMEBUFFERPROC)(int,int,int,int,int,int,int,int,unsigned int,unsigned int);
static PFNGLBLITFRAMEBUFFERPROC s_glBlitFramebuffer = nullptr;
static void ensureBlitFramebuffer() {
    if (!s_glBlitFramebuffer)
        s_glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)eglGetProcAddress("glBlitFramebuffer");
}
#define glBlitFramebuffer(...) (ensureBlitFramebuffer(), s_glBlitFramebuffer(__VA_ARGS__))
#endif
// FUCK ANDROID FRAMEBUFFER BS

using namespace geode::prelude;
namespace fs = std::filesystem;

// axiom was here
// i hate this project so much why did i start this, at least it has features now
double get_time_val() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool check_vram_low() { // i know someonme will complain about low fps so i added this
#ifdef GEODE_IS_WINDOWS
    static int vram_mb = -1;
    if (vram_mb == -1) {
        IDXGIFactory* f; if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&f))) return false;
        IDXGIAdapter* a; if (FAILED(f->EnumAdapters(0, &a))) { f->Release(); return false; }
        DXGI_ADAPTER_DESC d; a->GetDesc(&d);
        vram_mb = (int)(d.DedicatedVideoMemory / (1024 * 1024));
        a->Release(); f->Release();
    }
    return vram_mb < 2048;
#endif
    return false;
}

bool check_cpu_bad() {
    static int cores = std::thread::hardware_concurrency();
    return cores < 4;
}

bool is_running_under_wine() {
#ifdef GEODE_IS_WINDOWS
    static int cached_result = -1;
    if (cached_result != -1) return (bool)cached_result;

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
        cached_result = 1;
        return true;
    }

    cached_result = 0;
    return false;
#else
    return false;
#endif
}

int64_t get_total_ram_mb() {
#ifdef GEODE_IS_WINDOWS
    MEMORYSTATUSEX s; s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s)) return (int64_t)(s.ullTotalPhys / (1024 * 1024));
#elif defined(GEODE_IS_ANDROID)
    struct sysinfo si;
    if (sysinfo(&si) == 0) return (int64_t)(si.totalram * si.mem_unit / (1024 * 1024));
#endif
    return 4096;
}

std::string get_codec() {
    static std::string cached_codec = "";
    if (!cached_codec.empty()) return cached_codec;

#ifdef GEODE_IS_ANDROID
    // h264_mediacodec uses the hardware encoder on android, much better than libx264
    cached_codec = "h264_mediacodec";
    return cached_codec;
#endif

    char* sz_vendor_ptr = (char*)glGetString(GL_VENDOR);
    if (!sz_vendor_ptr) return "libx264";
    std::string vStr = sz_vendor_ptr ? geode::utils::string::toLower(sz_vendor_ptr) : "";

    if (is_running_under_wine()) {
        geode::log::info("wine detected!!!!!!");
        cached_codec = "libx264";
        return cached_codec;
    }

    if (vStr.find("nvidia") != std::string::npos) cached_codec = "h264_nvenc";
    else if (vStr.find("amd") != std::string::npos || vStr.find("ati") != std::string::npos || vStr.find("advanced micro") != std::string::npos) cached_codec = "h264_amf";
    else if (vStr.find("intel") != std::string::npos) cached_codec = "h264_qsv";
    else cached_codec = "libx264";
    return cached_codec;
}

void cleanup_temp_folder() {
    std::error_code ec;
    fs::path temp_dir = Mod::get()->getSaveDir() / "temp";
    if (fs::exists(temp_dir, ec)) {
        for (auto const& entry : fs::directory_iterator(temp_dir, ec)) {
            fs::remove(entry.path(), ec);
        }
    }
}

static void get_target_rec_size(int& outW, int& outH) {
    std::string outputRes = Mod::get()->getSettingValue<std::string>("output-res");
    int targetH = 720;
    if (outputRes == "480p") targetH = 480;
    else if (outputRes == "720p") targetH = 720;
    else if (outputRes == "1080p") targetH = 1080;
    else if (outputRes == "1440p") targetH = 1440;

    float userScale = (float)Mod::get()->getSettingValue<int64_t>("recording-scale") / 100.0f;
    targetH = (int)(targetH * userScale);

    int targetW = (int)(targetH * 16.0f / 9.0f);

    if (Mod::get()->getSettingValue<bool>("auto-performance")) {
        if (check_cpu_bad() || check_vram_low()) {
            targetW = (int)(targetW * 0.8f);
            targetH = (int)(targetH * 0.8f);
        }
    }

    targetW &= ~1;
    targetH &= ~1;

    outW = targetW;
    outH = targetH;
}

void save_clip(fs::path srcPath, std::string sLvlName, int nAttempts) {
    std::error_code ec;
    if (srcPath.empty() || !fs::exists(srcPath, ec)) return;
    geode::async::spawn([srcPath, sLvlName, nAttempts]() -> arc::Future<> {
        std::error_code ec;
        fs::path p_root_clips = Mod::get()->getSaveDir() / "clips";

        std::string clean_name = sLvlName;
        if (clean_name.empty()) clean_name = "Unknown";
        for (char& c : clean_name)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        if (clean_name.size() > 80) clean_name = clean_name.substr(0, 80);

        fs::path p_lvl_dir = p_root_clips / clean_name;
        fs::create_directories(p_lvl_dir, ec);

        fs::path out_file_path = p_lvl_dir / fmt::format("{}_att{}_{}.mp4", clean_name, nAttempts, (long long)::time(0));

        bool success = false;

#ifdef GEODE_IS_WINDOWS
        fs::path tmp_out = Mod::get()->getSaveDir() / "temp" / fmt::format("_tmp_{}_{}.mp4", (long long)::time(0), rand() % 1000);

        std::string codec = get_codec();
        std::string ff_bin;
        auto ffmpegMod = Loader::get()->getLoadedMod("eclipse.ffmpeg-api");
        if (ffmpegMod) {
            auto path = ffmpegMod->getResourcesDir() / "ffmpeg.exe";
            if (fs::exists(path, ec)) ff_bin = "\"" + geode::utils::string::pathToString(path) + "\"";
        }

        if (ff_bin.empty()) {
            fs::rename(srcPath, out_file_path, ec);
            if (!ec) success = true;
        } else {
            std::string ff_cmd = fmt::format("{} -y -i \"{}\" -metadata title=\"EchoClip\" -c:v {} -preset medium -crf 23 -pix_fmt yuv420p -movflags +faststart \"{}\"",
                ff_bin, geode::utils::string::pathToString(srcPath), codec, geode::utils::string::pathToString(tmp_out));

            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            std::vector<char> buf(ff_cmd.begin(), ff_cmd.end()); buf.push_back(0);
            if (CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            }
            if (fs::exists(tmp_out, ec)) {
                fs::remove(srcPath, ec); fs::rename(tmp_out, out_file_path, ec);
                if (!ec) success = true;
            } else {
                fs::rename(srcPath, out_file_path, ec);
                if (!ec) success = true;
            }
        }
#else
        // on android just move the file directly, no re-encoding needed
        fs::rename(srcPath, out_file_path, ec);
        if (!ec) success = true;
#endif

        int64_t max_days = Mod::get()->getSettingValue<int64_t>("cleanup-days");
        uintmax_t max_bytes = (uintmax_t)Mod::get()->getSettingValue<int64_t>("storage-limit") * 1024 * 1024 * 1024;

        std::vector<fs::path> vFiles;
        auto now_sys = std::chrono::system_clock::now();

        for (auto const& dir_entry : fs::recursive_directory_iterator(p_root_clips, ec)) {
            if (ec) break;
            if (dir_entry.is_regular_file() && (dir_entry.path().extension() == ".mp4" || dir_entry.path().extension() == ".mkv")) {
                std::string rel = geode::utils::string::pathToString(fs::relative(dir_entry.path(), p_root_clips, ec));
                if (rel.find("favorites") != std::string::npos) continue;

                if (max_days > 0) {
                    auto ftime = fs::last_write_time(dir_entry.path(), ec);
                    auto sclock = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                    auto diff_hours = std::chrono::duration_cast<std::chrono::hours>(now_sys - sclock).count();
                    if (diff_hours > max_days * 24) {
                        fs::remove(dir_entry.path(), ec);
                        continue;
                    }
                }
                vFiles.push_back(dir_entry.path());
            }
        }

        std::sort(vFiles.begin(), vFiles.end(), [](fs::path a, fs::path b) {
            std::error_code e1, e2;
            return fs::last_write_time(a, e1) < fs::last_write_time(b, e2);
        });

        uintmax_t nTotal = 0;
        for (auto const& f : vFiles) { std::error_code e; nTotal += fs::file_size(f, e); }
        for (size_t i = 0; i < vFiles.size() && nTotal > max_bytes; i++) {
            std::error_code e;
            nTotal -= fs::file_size(vFiles[i], e); fs::remove(vFiles[i], e);
        }

        Loader::get()->queueInMainThread([success] {
            if (!CCDirector::get()->getRunningScene()) return;
            if (success) {
                Notification::create("Clip Saved!", CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png"))->show();
                Gallery::refresh();
            } else {
                Notification::create("Save Failed!", CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png"))->show();
            }
        });
        co_return;
    });
}

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
    save_clip(s->temp_file_p, lvl, att);
}

class $modify(MyBaseGameLayer, GJBaseGameLayer) {
    struct Fields {
        std::shared_ptr<RecSession> session;
        bool active = false;
        int nW = 0, nH = 0;
        int prev_downscale_w = 0, prev_downscale_h = 0;
        std::string s_lvl_str;
        int n_att_count = 0, best_percent = 0;
        float f_timer_val = 0;

#ifndef GEODE_IS_ANDROID
        GLuint pbo_bufer[3] = {0, 0, 0};
        GLsync fences_sync_ptr[3] = {0, 0, 0};
        int write_idx = 0;
#endif
        int n_pushed_frames = 0;
        bool b_setup_done = false;
        bool b_capture_this_frame = false;

        GLuint downscale_fbo = 0;
        GLuint downscale_tex = 0;

        float gap_cache = 0.01666f;
        bool clip_new_best = false;
        int current_rec_att = 0;

        ~Fields() {
            if (session) { session->dead = true; session->m_cv.notify_all(); }
#ifndef GEODE_IS_ANDROID
            for (int i = 0; i < 3; i++) if (fences_sync_ptr[i]) { glDeleteSync(fences_sync_ptr[i]); fences_sync_ptr[i] = 0; }
#endif
            if (downscale_fbo) glDeleteFramebuffers(1, &downscale_fbo);
            if (downscale_tex) glDeleteTextures(1, &downscale_tex);
        }
    };

    bool init() {
        if (!GJBaseGameLayer::init()) return false;
        m_fields->current_rec_att = 1;
        m_fields->best_percent = 0;
        m_fields->n_att_count = 1;
        return true;
    }

    void trigger_clip() {
        Fields* f = m_fields.self();
        if (f->active) {
            auto s = kill_rec();
            if (s) {
                finalize_and_save(s, f->s_lvl_str, f->current_rec_att);
                int w = 0, h = 0;
                get_target_rec_size(w, h);
                start_rec(w, h);
            }
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

        ffmpeg::RenderSettings config;
        config.m_height = recH; config.m_width = recW;
        config.m_fps = (uint16_t)Mod::get()->getSettingValue<int64_t>("target-fps");
        config.m_bitrate = 15000000;
        config.m_outputFile = temp_p;
        config.m_codec = get_codec();
#ifdef GEODE_IS_ANDROID
        config.m_pixelFormat = ffmpeg::PixelFormat::RGBA;
#else
        config.m_pixelFormat = ffmpeg::PixelFormat::BGRA;
#endif
        config.m_doVerticalFlip = true;

        ffmpeg::events::Recorder* p_rec = new ffmpeg::events::Recorder();
        auto res = p_rec->init(config);
        if (res.isOk()) {
            auto s = std::make_shared<RecSession>();
            s->rec = p_rec; s->max_frames = max_f; s->temp_file_p = temp_p;
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
                    (void)s->rec->writeFrame(c_pixel);
                    std::lock_guard<std::mutex> l(s->m_p_mtx);
                    std::lock_guard<std::mutex> lq(s->m_q_mtx);
                    if ((int)(s->pool_frames.size() + s->c_pixel_q.size()) < s->max_frames) {
                        s->pool_frames.push_back(std::move(c_pixel));
                    }
                }
            });
            if (m_fields->b_setup_done) cleanup_gl();
        } else {
            delete p_rec;
        }
    }

    std::shared_ptr<RecSession> kill_rec() {
        if (!m_fields->active || !m_fields->session) return nullptr;
        m_fields->active = false;
        auto s = m_fields->session;
        m_fields->session = nullptr;
        s->dead = true; s->m_cv.notify_all();
        return s;
    }

    void cleanup_gl() {
        if (!m_fields->b_setup_done) return;
#ifndef GEODE_IS_ANDROID
        glDeleteBuffers(3, m_fields->pbo_bufer);
        for (int i = 0; i < 3; i++) if (m_fields->fences_sync_ptr[i]) { glDeleteSync(m_fields->fences_sync_ptr[i]); m_fields->fences_sync_ptr[i] = 0; }
#endif
        m_fields->b_setup_done = false;
    }

    void update(float dt) {
        GJBaseGameLayer::update(dt);
        Fields* f = m_fields.self();
        if (!f->active || !f->session) return;
        auto s = f->session;

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
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    listenForKeybindSettingPresses("clip-keybind", [](geode::Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            if (Mod::get()->getSettingValue<bool>("enabled")) {
                if (auto layer = GJBaseGameLayer::get()) {
                    static_cast<MyBaseGameLayer*>(layer)->trigger_clip();
                }
            }
        }
    });
#endif
}

class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        auto layer = GJBaseGameLayer::get();
        if (layer) {
            auto f = static_cast<MyBaseGameLayer*>(layer)->m_fields.self();
            if (f->active && f->session && f->b_capture_this_frame) {
                auto s = f->session;
                f->b_capture_this_frame = false;
                int recW = f->nW; int recH = f->nH;
                int sz_bytes = recW * recH * 4;

                auto fs = CCDirector::get()->getOpenGLView()->getFrameSize();
                int winW = (int)fs.width; int winH = (int)fs.height;

#ifdef GEODE_IS_ANDROID
                // OpenGL ES 2.0, fuck PBO's, fuck sync objects, used glReadPixels
                #define GL_READ_FRAMEBUFFER 0x8CA8
                #define GL_DRAW_FRAMEBUFFER 0x8CA9

                if (winW != recW || winH != recH) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->downscale_fbo);
                    glBlitFramebuffer(0, 0, winW, winH, 0, 0, recW, recH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->downscale_fbo);
                } else {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                }

                std::vector<uint8_t> c_pixel;
                {
                    std::lock_guard<std::mutex> l(s->m_p_mtx);
                    if (!s->pool_frames.empty()) { c_pixel = std::move(s->pool_frames.back()); s->pool_frames.pop_back(); }
                }
                if (c_pixel.size() != (size_t)sz_bytes) c_pixel.resize(sz_bytes);
                // GL_BGRA may not be available on ES, use GL_RGBA
                glReadPixels(0, 0, recW, recH, GL_RGBA, GL_UNSIGNED_BYTE, c_pixel.data());
                glBindFramebuffer(GL_FRAMEBUFFER, 0);

                {
                    std::lock_guard<std::mutex> lp(s->m_p_mtx);
                    std::lock_guard<std::mutex> lq(s->m_q_mtx);
                    if ((int)s->c_pixel_q.size() < s->max_frames) {
                        s->c_pixel_q.push(std::move(c_pixel));
                        s->m_cv.notify_one();
                    } else if ((int)s->pool_frames.size() < s->max_frames) {
                        s->pool_frames.push_back(std::move(c_pixel));
                    }
                }
#else
                // Desktop OpenGL - full async triple-PBO path
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
                        void* p_pix = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sz_bytes, GL_MAP_READ_BIT);
                        if (p_pix) {
                            std::vector<uint8_t> c_pixel;
                            {
                                std::lock_guard<std::mutex> l(s->m_p_mtx);
                                if (!s->pool_frames.empty()) { c_pixel = std::move(s->pool_frames.back()); s->pool_frames.pop_back(); }
                            }
                            if (c_pixel.size() != (size_t)sz_bytes) c_pixel.resize(sz_bytes);
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
#endif
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

        auto bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
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
        auto bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
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
        auto bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        auto f = bgl->m_fields.self();
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
        auto bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        auto f = bgl->m_fields.self();
        if (!f || !m_level) return;
        auto s = bgl->kill_rec();
        if (s) {
            finalize_and_save(s, f->s_lvl_str, f->current_rec_att);
            int w = 0, h = 0;
            get_target_rec_size(w, h);
            bgl->start_rec(w, h);
        }
    }

    void destroyPlayer(PlayerObject* boi, GameObject* obj) {
        PlayLayer::destroyPlayer(boi, obj);
        auto bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        auto f = bgl->m_fields.self();
        if (!f || !f->clip_new_best || !m_player1 || !m_level || m_levelLength <= 0.f) return;
        int cur = (int)(m_player1->getPositionX() / m_levelLength * 100.f);
        if (cur <= f->best_percent) return;
        f->best_percent = cur;
        auto s = bgl->kill_rec();
        if (s) {
            finalize_and_save(s, f->s_lvl_str, f->current_rec_att);
            int w = 0, h = 0;
            get_target_rec_size(w, h);
            bgl->start_rec(w, h);
        }
    }

    void onExit() {
        PlayLayer::onExit();
        auto bgl = static_cast<MyBaseGameLayer*>(static_cast<GJBaseGameLayer*>(this));
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

        auto spr = CCSprite::create("gallery.png"_spr);
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
        spr->setScale(0.5f);

        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MyPauseLayer::onGallery));
        btn->setID("gallery-btn"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onGallery(CCObject*) {
        Gallery::open();
    }
};
