#include "common.hpp"
#include <Geode/Geode.hpp>
#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

double get_time_val() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool check_cpu_bad() {
    static int cores = std::thread::hardware_concurrency();
    return cores < 4;
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

void get_target_rec_size(int& outW, int& outH) {
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

void cleanup_old_clips(const fs::path& p_root_clips) {
    std::error_code ec;
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
}

#ifndef GEODE_IS_WINDOWS
bool check_vram_low() { return false; }
#endif
