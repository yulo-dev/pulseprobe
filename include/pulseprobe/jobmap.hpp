// Maps PIDs to scheduler job IDs so GPU metrics can be attributed to the job
// that owns them on a shared node. The layout mirrors the HPC job-mapping
// directory convention: one file per job, named for the job ID, containing the
// PIDs belonging to it.
//
//   /var/run/pulseprobe/jobs/88412  ->  "40213\n40219\n"
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace pulseprobe::jobmap {

using PidToJob = std::unordered_map<unsigned, std::string>;

// Parses one job file's contents. Blank lines and non-numeric lines are
// skipped rather than failing the whole map.
std::vector<unsigned> parse_job_file(const std::string& content);

// Scans a job-mapping directory. A missing or unreadable directory yields an
// empty map: job attribution is optional, never fatal.
PidToJob load_directory(const std::string& dir);

}  // namespace pulseprobe::jobmap
