#pragma once
#include "ib.hpp"

namespace ib {
void set_timestamp(const fs::path &path, uint64_t time, bool link = false);
void set_mode(const fs::path &path, const std::string &input);
fs::path create_staging(const fs::path &parent);
void create_link(const fs::path &path, const std::string &target);
void remove_tree(const fs::path &path, std::error_code &error);
} // namespace ib
