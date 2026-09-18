#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulseprobe::util {

// Reads a whole file. procfs files report st_size 0, so this cannot be
// sized up front and has to be read until EOF.
std::optional<std::string> read_file(const std::string& path);

std::vector<std::string> split_whitespace(const std::string& line);
std::vector<std::string> split_lines(const std::string& content);
std::string trim(const std::string& s);

int64_t now_unix_millis();
int64_t monotonic_micros();

}  // namespace pulseprobe::util
