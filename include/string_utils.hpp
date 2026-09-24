#pragma once
#include <string>

namespace scand::str_utils {
std::string to_lower(std::string s);
std::string trim(const std::string& s);
bool        is_truthy(const std::string& v);
bool        ends_with_ci(const std::string& s, const std::string& suffix);
} // namespace scand::str_utils
