// Prometheus text exposition format (version 0.0.4) rendering.
#pragma once

#include <string>

#include "pulseprobe/snapshot.hpp"

namespace pulseprobe {

struct ExpositionOptions {
  // Added as a label to every series when non-empty.
  std::string node_name;
  std::string version = "0.1.0";
};

std::string render_prometheus(const Snapshot& snapshot, const ExpositionOptions& options);

// Exposed for tests: escapes a label value per the exposition format spec
// (backslash, double quote, newline).
std::string escape_label_value(const std::string& value);

}  // namespace pulseprobe
