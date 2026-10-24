// Instances that once produced a wrong answer, with the optimum taken from an
// independent linear program (tools/lp_check.py, scipy HiGHS).
//
// Each of these caught a real defect:
//   * detector_misses_a_cycle_through_the_sink -- the cycle detector walked
//     parent pointers a fixed number of steps and gave up when a chain left the
//     parent graph, so it reported "no flow-generating cycle" and the solver
//     stopped early while claiming optimality.
//   * retired_arc_hid_an_augmenting_path / retired_arc_hid_a_backward_path -- a
//     walk that pushes an arc and then its twin hands the first arc's residual
//     back. The old progress guard read that as "no progress" and retired the
//     arc, deleting capacity the optimum needed and hiding the remaining
//     augmenting path from the certificate.
#include <string>

#include "gmf/gmf.hpp"
#include "gmf/json.hpp"
#include "harness.hpp"

namespace {

void check_optimum(const std::string& request, double expected) {
  const gmf::json::Value answer = gmf::json::parse(gmf::solve_json(request));
  const std::string status = answer.find("status")->as_string();
  CHECK_MESSAGE(status == "optimal", status + ": " + answer.find("message")->as_string());
  CHECK_NEAR(answer.find("max_flow")->as_number(), expected, 1e-7);
  const gmf::json::Value* certificate = answer.find("certificate");
  CHECK(certificate->find("no_augmenting_path")->as_bool());
  CHECK(certificate->find("no_generating_cycle_reaching_sink")->as_bool());
  CHECK(certificate->find("max_conservation_error")->as_number() < 1e-9);
  CHECK(certificate->find("max_capacity_violation")->as_number() < 1e-9);
}

}  // namespace

TEST(detector_misses_a_cycle_through_the_sink) {
  check_optimum(R"({"source": 0, "sink": 6, "edges": [
    {"from": 4, "to": 6, "capacity": 10.0, "multiplier": 0.301},
    {"from": 1, "to": 3, "capacity": 2.0, "multiplier": 1.129},
    {"from": 6, "to": 2, "capacity": 1.0, "multiplier": 1.74},
    {"from": 1, "to": 2, "capacity": 10.0, "multiplier": 0.686},
    {"from": 6, "to": 3, "capacity": 6.0, "multiplier": 0.924},
    {"from": 2, "to": 4, "capacity": 2.0, "multiplier": 0.354},
    {"from": 1, "to": 6, "capacity": 5.0, "multiplier": 0.791},
    {"from": 2, "to": 1, "capacity": 3.0, "multiplier": 1.619},
    {"from": 5, "to": 2, "capacity": 1.0, "multiplier": 2.0},
    {"from": 3, "to": 0, "capacity": 7.0, "multiplier": 1.324}]})",
                1.3890298571428574);
}

TEST(retired_arc_hid_an_augmenting_path) {
  check_optimum(R"({"source": 0, "sink": 5, "edges": [
    {"from": 4, "to": 2, "capacity": 8.0, "multiplier": 0.91},
    {"from": 2, "to": 3, "capacity": 3.0, "multiplier": 1.089},
    {"from": 4, "to": 5, "capacity": 2.0, "multiplier": 1.475},
    {"from": 0, "to": 3, "capacity": 6.0, "multiplier": 1.736},
    {"from": 3, "to": 0, "capacity": 8.0, "multiplier": 1.924},
    {"from": 5, "to": 0, "capacity": 9.0, "multiplier": 0.852},
    {"from": 0, "to": 1, "capacity": 7.0, "multiplier": 1.862},
    {"from": 1, "to": 2, "capacity": 3.0, "multiplier": 1.984},
    {"from": 3, "to": 5, "capacity": 3.0, "multiplier": 0.778},
    {"from": 3, "to": 4, "capacity": 9.0, "multiplier": 0.726},
    {"from": 0, "to": 3, "capacity": 5.0, "multiplier": 1.515},
    {"from": 2, "to": 5, "capacity": 3.0, "multiplier": 1.597},
    {"from": 1, "to": 0, "capacity": 8.0, "multiplier": 0.231}]})",
                10.075);
}

TEST(retired_arc_hid_a_backward_path) {
  check_optimum(R"({"source": 0, "sink": 5, "edges": [
    {"from": 1, "to": 3, "capacity": 6.0, "multiplier": 1.863},
    {"from": 1, "to": 2, "capacity": 7.0, "multiplier": 0.998},
    {"from": 4, "to": 5, "capacity": 5.0, "multiplier": 1.792},
    {"from": 3, "to": 2, "capacity": 8.0, "multiplier": 1.245},
    {"from": 3, "to": 0, "capacity": 2.0, "multiplier": 1.427},
    {"from": 0, "to": 4, "capacity": 2.0, "multiplier": 0.504},
    {"from": 0, "to": 2, "capacity": 6.0, "multiplier": 1.859},
    {"from": 3, "to": 0, "capacity": 2.0, "multiplier": 1.118},
    {"from": 4, "to": 2, "capacity": 2.0, "multiplier": 0.756},
    {"from": 2, "to": 4, "capacity": 1.0, "multiplier": 1.381},
    {"from": 5, "to": 3, "capacity": 9.0, "multiplier": 1.582},
    {"from": 4, "to": 0, "capacity": 4.0, "multiplier": 1.796}]})",
                4.281088);
}

TEST(amplifying_network_with_many_interacting_cycles) {
  check_optimum(R"({"source": 0, "sink": 9, "edges": [
    {"from": 3, "to": 6, "capacity": 7.0, "multiplier": 0.852},
    {"from": 4, "to": 7, "capacity": 8.0, "multiplier": 1.152},
    {"from": 8, "to": 3, "capacity": 5.0, "multiplier": 0.24},
    {"from": 4, "to": 6, "capacity": 1.0, "multiplier": 1.366},
    {"from": 0, "to": 9, "capacity": 8.0, "multiplier": 0.714},
    {"from": 3, "to": 5, "capacity": 9.0, "multiplier": 1.431},
    {"from": 5, "to": 7, "capacity": 2.0, "multiplier": 0.915},
    {"from": 8, "to": 3, "capacity": 10.0, "multiplier": 1.984},
    {"from": 3, "to": 1, "capacity": 2.0, "multiplier": 0.644},
    {"from": 5, "to": 8, "capacity": 7.0, "multiplier": 1.127},
    {"from": 4, "to": 0, "capacity": 5.0, "multiplier": 1.13},
    {"from": 1, "to": 8, "capacity": 2.0, "multiplier": 0.224},
    {"from": 2, "to": 6, "capacity": 2.0, "multiplier": 1.006},
    {"from": 6, "to": 9, "capacity": 9.0, "multiplier": 1.265},
    {"from": 9, "to": 2, "capacity": 2.0, "multiplier": 1.035},
    {"from": 3, "to": 7, "capacity": 4.0, "multiplier": 1.884},
    {"from": 8, "to": 5, "capacity": 10.0, "multiplier": 1.859},
    {"from": 9, "to": 0, "capacity": 1.0, "multiplier": 0.596},
    {"from": 3, "to": 9, "capacity": 2.0, "multiplier": 0.637},
    {"from": 4, "to": 5, "capacity": 3.0, "multiplier": 1.924},
    {"from": 8, "to": 9, "capacity": 2.0, "multiplier": 1.958},
    {"from": 2, "to": 4, "capacity": 4.0, "multiplier": 1.666},
    {"from": 3, "to": 2, "capacity": 1.0, "multiplier": 0.819},
    {"from": 2, "to": 3, "capacity": 1.0, "multiplier": 1.532},
    {"from": 7, "to": 5, "capacity": 5.0, "multiplier": 1.822},
    {"from": 2, "to": 5, "capacity": 2.0, "multiplier": 1.307}]})",
                20.89445907490838);
}

TEST(amplifying_network_feeding_the_source_backwards) {
  check_optimum(R"({"source": 0, "sink": 6, "edges": [
    {"from": 5, "to": 6, "capacity": 9.0, "multiplier": 0.569},
    {"from": 1, "to": 4, "capacity": 2.0, "multiplier": 1.198},
    {"from": 6, "to": 4, "capacity": 1.0, "multiplier": 0.303},
    {"from": 0, "to": 4, "capacity": 9.0, "multiplier": 1.905},
    {"from": 3, "to": 5, "capacity": 8.0, "multiplier": 1.273},
    {"from": 6, "to": 0, "capacity": 8.0, "multiplier": 1.947},
    {"from": 1, "to": 5, "capacity": 8.0, "multiplier": 0.624},
    {"from": 0, "to": 4, "capacity": 9.0, "multiplier": 0.417},
    {"from": 3, "to": 2, "capacity": 8.0, "multiplier": 0.659},
    {"from": 5, "to": 0, "capacity": 4.0, "multiplier": 1.708},
    {"from": 2, "to": 6, "capacity": 6.0, "multiplier": 0.755},
    {"from": 0, "to": 5, "capacity": 9.0, "multiplier": 1.544},
    {"from": 5, "to": 6, "capacity": 10.0, "multiplier": 1.202},
    {"from": 2, "to": 6, "capacity": 10.0, "multiplier": 1.768},
    {"from": 1, "to": 5, "capacity": 7.0, "multiplier": 1.682},
    {"from": 4, "to": 2, "capacity": 2.0, "multiplier": 0.483},
    {"from": 0, "to": 6, "capacity": 1.0, "multiplier": 0.56},
    {"from": 1, "to": 6, "capacity": 2.0, "multiplier": 1.817}]})",
                16.504712);
}

TEST(a_long_lossy_chain_keeps_its_precision) {
  // Fifty halvings: the delivered flow is 2^-50 of what enters, which is where a
  // solver that multiplies gains instead of adding their logarithms loses the
  // path entirely.
  std::string request = R"({"source": 0, "sink": 50, "edges": [)";
  for (int i = 0; i < 50; ++i) {
    if (i) request += ",";
    request += "{\"from\":" + std::to_string(i) + ",\"to\":" + std::to_string(i + 1) +
               ",\"capacity\":1,\"multiplier\":0.5}";
  }
  request += "]}";
  const gmf::json::Value answer = gmf::json::parse(gmf::solve_json(request));
  CHECK(answer.find("status")->as_string() == "optimal");
  CHECK_NEAR(answer.find("max_flow")->as_number(), 1.0 / 1125899906842624.0, 1e-12);
}
