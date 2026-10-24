#include <string>

#include "gmf/gmf.hpp"
#include "gmf/json.hpp"
#include "harness.hpp"

namespace {

gmf::json::Value solve(const std::string& request) {
  return gmf::json::parse(gmf::solve_json(request));
}

double number(const gmf::json::Value& value, const std::string& key) {
  const gmf::json::Value* found = value.find(key);
  CHECK(found != nullptr);
  return found->as_number();
}

}  // namespace

TEST(an_adjacency_object_is_solved) {
  const gmf::json::Value answer = solve(R"({
    "source": "s",
    "sink": "t",
    "adjacency": {
      "s": [{"to": "a", "capacity": 10, "multiplier": 1.0}],
      "a": [{"to": "t", "capacity": 5, "multiplier": 2.0}]
    }
  })");
  CHECK(answer.find("status")->as_string() == "optimal");
  CHECK_NEAR(number(answer, "max_flow"), 10.0, 1e-9);
  CHECK_NEAR(number(answer, "source_outflow"), 5.0, 1e-9);
  CHECK(answer.find("paths")->as_array().size() == 1);
  CHECK(answer.find("source")->as_string() == "s");
  CHECK(answer.find("sink")->as_string() == "t");
}

TEST(an_edge_list_is_solved) {
  const gmf::json::Value answer = solve(R"({
    "source": 0, "sink": 2,
    "edges": [
      {"from": 0, "to": 1, "capacity": 4, "multiplier": 0.5},
      {"from": 1, "to": 2, "capacity": 4, "multiplier": 0.5}
    ]
  })");
  CHECK(answer.find("status")->as_string() == "optimal");
  CHECK_NEAR(number(answer, "max_flow"), 1.0, 1e-9);
  // Numeric labels come back as numbers, not strings.
  CHECK(answer.find("source")->is_number());
  CHECK(answer.find("edges")->as_array().size() == 2);
  const gmf::json::Value& first = answer.find("edges")->as_array()[0];
  CHECK_NEAR(first.find("flow")->as_number(), 4.0, 1e-9);
  CHECK_NEAR(first.find("delivered")->as_number(), 2.0, 1e-9);
}

TEST(positional_arrays_are_accepted) {
  // Adjacency rows indexed by node, each arc written as [to, capacity, multiplier].
  const gmf::json::Value answer = solve(R"({
    "source": 0, "sink": 2,
    "adjacency": [[[1, 6, 1.0]], [[2, 6, 0.5]], []]
  })");
  CHECK_MESSAGE(answer.find("status")->as_string() == "optimal",
                gmf::json::dump(answer));
  CHECK_NEAR(number(answer, "max_flow"), 3.0, 1e-9);
}

TEST(a_missing_capacity_means_uncapacitated) {
  const gmf::json::Value answer = solve(R"({
    "source": "s", "sink": "t",
    "edges": [{"from": "s", "to": "t", "multiplier": 0.5}]
  })");
  CHECK(answer.find("status")->as_string() == "unbounded");
}

TEST(the_steps_name_the_edge_and_its_direction) {
  const gmf::json::Value answer = solve(R"({
    "source": "s", "sink": "t",
    "adjacency": {"s": [{"to": "t", "cap": 8, "gain": 0.25}]},
    "options": {"pretty": false}
  })");
  const gmf::json::Value& path = answer.find("paths")->as_array()[0];
  const gmf::json::Value& step = path.find("steps")->as_array()[0];
  CHECK(step.find("direction")->as_string() == "forward");
  CHECK(step.find("from")->as_string() == "s");
  CHECK(step.find("to")->as_string() == "t");
  CHECK_NEAR(step.find("flow_in")->as_number(), 8.0, 1e-9);
  CHECK_NEAR(step.find("flow_out")->as_number(), 2.0, 1e-9);
  CHECK(path.find("bottleneck") != nullptr);
  CHECK(path.find("bottleneck")->find("direction")->as_string() == "forward");
}

TEST(a_certificate_accompanies_every_answer) {
  const gmf::json::Value answer = solve(R"({
    "source": "s", "sink": "t",
    "edges": [{"from": "s", "to": "t", "capacity": 3, "multiplier": 1}]
  })");
  const gmf::json::Value* certificate = answer.find("certificate");
  CHECK(certificate != nullptr);
  CHECK(certificate->find("no_augmenting_path")->as_bool());
  CHECK(certificate->find("no_generating_cycle_reaching_sink")->as_bool());
  CHECK_NEAR(certificate->find("max_conservation_error")->as_number(), 0.0, 1e-12);
}

TEST(bad_requests_come_back_as_invalid_input) {
  const char* requests[] = {
      "not json",
      "{}",
      R"({"source": "s", "sink": "t"})",
      R"({"sink": "t", "edges": []})",
      R"({"source": "s", "sink": "s", "edges": [{"from": "s", "to": "s", "capacity": 1}]})",
      R"({"source": "s", "sink": "t", "edges": [{"from": "s", "to": "t", "capacity": -1}]})",
      R"({"source": "s", "sink": "t", "edges": [{"from": "s", "to": "t", "multiplier": -2}]})",
      R"({"source": "s", "sink": "t", "edges": [{"to": "t", "capacity": 1}]})",
  };
  for (const char* request : requests) {
    const gmf::json::Value answer = solve(request);
    CHECK_MESSAGE(answer.find("status")->as_string() == "invalid_input",
                  std::string("accepted: ") + request);
    CHECK(answer.find("error") != nullptr);
  }
}

TEST(options_are_read_from_the_request) {
  const gmf::json::Value answer = solve(R"({
    "source": "s", "sink": "t",
    "edges": [{"from": "s", "to": "t", "capacity": 2, "multiplier": 1}],
    "options": {"threads": 2, "collect_paths": false, "pretty": true}
  })");
  CHECK(answer.find("paths")->as_array().empty());
  CHECK_NEAR(number(answer, "max_flow"), 2.0, 1e-9);
  CHECK(gmf::solve_json(R"({"source":"s","sink":"t","edges":[{"from":"s","to":"t","capacity":1}],
                            "options":{"pretty":true}})")
            .find('\n') != std::string::npos);
}

TEST(the_version_is_reported) {
  const gmf::json::Value answer = solve(R"({"source": 0, "sink": 1, "edges": []})");
  CHECK(answer.find("version") != nullptr);
  CHECK(answer.find("version")->as_string() == std::string(gmf::version()));
}
