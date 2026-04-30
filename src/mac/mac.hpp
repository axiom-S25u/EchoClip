#pragma once
#ifdef GEODE_IS_MACOS

#include <string>
#include <filesystem>
#include <cstdint>

std::string get_codec();
int64_t get_total_ram_mb();
void save_clip(std::filesystem::path srcPath, std::string sLvlName, int nAttempts);

#endif
