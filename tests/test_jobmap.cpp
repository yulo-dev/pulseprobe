#include "pulseprobe/jobmap.hpp"

#include "testing.hpp"

using namespace pulseprobe;

namespace {

void test_parse_job_file() {
  const auto pids = jobmap::parse_job_file("40213\n40219\n\n40224\n");
  CHECK(pids.size() == 3);
  if (pids.size() == 3) {
    CHECK(pids[0] == 40213);
    CHECK(pids[1] == 40219);
    CHECK(pids[2] == 40224);
  }
}

// A job file being written while the agent reads it will show partial or
// malformed lines. Skipping those beats dropping the whole job's attribution.
void test_parse_job_file_skips_garbage() {
  const auto pids = jobmap::parse_job_file("40213\nnot-a-pid\n0\n  40219  \n");
  CHECK(pids.size() == 2);
  if (pids.size() == 2) {
    CHECK(pids[0] == 40213);
    CHECK(pids[1] == 40219);
  }
}

// Job attribution is optional: a node without a scheduler still serves GPU
// metrics, just without job labels.
void test_missing_directory_is_not_an_error() {
  CHECK(jobmap::load_directory("/nonexistent/pulseprobe/jobs").empty());
  CHECK(jobmap::load_directory("").empty());
}

void test_load_directory() {
  const auto map = jobmap::load_directory(std::string(PULSEPROBE_FIXTURE_DIR) + "/jobs");
  CHECK(map.size() == 3);
  const auto first = map.find(40213);
  CHECK(first != map.end());
  if (first != map.end()) CHECK(first->second == "88412");
  const auto second = map.find(51002);
  CHECK(second != map.end());
  if (second != map.end()) CHECK(second->second == "88413");
  CHECK(map.find(99999) == map.end());
}

}  // namespace

int main() {
  test_parse_job_file();
  test_parse_job_file_skips_garbage();
  test_missing_directory_is_not_an_error();
  test_load_directory();
  return testing::summarize("jobmap");
}
