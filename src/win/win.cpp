#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include "win.hpp"
#include "common/common.hpp"
#include <Geode/utils/async.hpp>
#include <Geode/utils/string.hpp>
#include <eclipse.ffmpeg-api/include/events.hpp>
#include "ui.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <algorithm>

using namespace geode::prelude;
namespace fs = std::filesystem;

bool check_vram_low() {
    static int vram_mb = -1;
    if (vram_mb == -1) {
        IDXGIFactory* f; if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&f))) return false;
        IDXGIAdapter* a; if (FAILED(f->EnumAdapters(0, &a))) { f->Release(); return false; }
        DXGI_ADAPTER_DESC d; a->GetDesc(&d);
        vram_mb = (int)(d.DedicatedVideoMemory / (1024 * 1024));
        a->Release(); f->Release();
    }
    return vram_mb < 2048;
}

bool is_running_under_wine() {
    static int cached_result = -1;
    if (cached_result != -1) return (bool)cached_result;

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
        cached_result = 1;
        return true;
    }

    cached_result = 0;
    return false;
}

std::string get_codec() {
    static std::string cached_codec = "";
    if (!cached_codec.empty()) return cached_codec;

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

int64_t get_total_ram_mb() {
    MEMORYSTATUSEX s; s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s)) return (int64_t)(s.ullTotalPhys / (1024 * 1024));
    return 4096;
}

void save_clip(fs::path srcPath, std::string sLvlName, int nAttempts) {
    std::error_code ec;
    if (srcPath.empty() || !fs::exists(srcPath, ec)) return;
    geode::async::spawn([srcPath, sLvlName, nAttempts]() mutable -> arc::Future<> {
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

        {
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
        }

        cleanup_old_clips(p_root_clips);

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

#endif
