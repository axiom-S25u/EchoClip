#pragma once
#include <string>
#include <cstdint>
#include <filesystem>
#include <vector>

double get_time_val();
bool check_cpu_bad();
bool check_vram_low();
void cleanup_temp_folder();
void get_target_rec_size(int& outW, int& outH);
void cleanup_old_clips(const std::filesystem::path& p_root_clips);

// plat. specific shit
std::string get_codec();
int64_t get_total_ram_mb();
void save_clip(std::filesystem::path srcPath, std::string sLvlName, int nAttempts);

#ifdef GEODE_IS_WINDOWS
bool is_running_under_wine();
#endif
