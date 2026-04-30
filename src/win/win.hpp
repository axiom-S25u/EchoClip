#pragma once
#ifdef GEODE_IS_WINDOWS

#include <string>
#include <filesystem>
#include <cstdint>

bool check_vram_low();
bool is_running_under_wine();
std::string get_codec();
int64_t get_total_ram_mb();
void save_clip(std::filesystem::path srcPath, std::string sLvlName, int nAttempts);

#endif
