#include <Geode/Geode.hpp>
#ifdef GEODE_IS_MACOS
#include <sys/types.h>
#include <sys/sysctl.h>
#include "mac.hpp"
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

std::string get_codec() {
    static std::string cached_codec = "h264_videotoolbox";
    return cached_codec;
}

int64_t get_total_ram_mb() {
    int64_t mem = 0;
    size_t len = sizeof(mem);
    ::sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    return mem / (1024 * 1024);
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
                auto path = ffmpegMod->getResourcesDir() / "ffmpeg";
                if (fs::exists(path, ec)) ff_bin = "\"" + geode::utils::string::pathToString(path) + "\"";
            }

            if (ff_bin.empty()) {
                fs::rename(srcPath, out_file_path, ec);
                if (!ec) success = true;
            } else {
                std::string ff_cmd = fmt::format("{} -y -i \"{}\" -metadata title=\"EchoClip\" -c:v {} -crf 23 -pix_fmt yuv420p -movflags +faststart \"{}\" &",
                    ff_bin, geode::utils::string::pathToString(srcPath), codec, geode::utils::string::pathToString(tmp_out));
                system(ff_cmd.c_str());

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

float get_macos_backing_scale() {
    return 1.0f;
}

#endif // i hope its clean enough now ERY