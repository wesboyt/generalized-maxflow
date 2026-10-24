#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "gmf/gmf.hpp"
#include "harness.hpp"
#include "reference.hpp"

namespace {

gmf::Problem make_problem(int num_nodes, int source, int sink,
                          const std::vector<gmf::EdgeInput>& edges) {
  gmf::Problem problem;
  problem.num_nodes = num_nodes;
  problem.source = source;
  problem.sink = sink;
  problem.edges = edges;
  return problem;
}

gmf::EdgeInput edge(int from, int to, double capacity, double multiplier) {
  gmf::EdgeInput value;
  value.from = from;
  value.to = to;
  value.capacity = capacity;
  value.multiplier = multiplier;
  return value;
}

}  // namespace

TEST(single_edge_applies_its_multiplier) {
  const gmf::Problem problem = make_problem(2, 0, 1, {edge(0, 1, 10.0, 0.5)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 5.0, 1e-9);
  CHECK_NEAR(result.source_outflow, 10.0, 1e-9);
  CHECK(result.augmentations.size() == 1);
  CHECK_NEAR(result.augmentations[0].gain, 0.5, 1e-9);
}

TEST(gain_downstream_of_the_constriction_is_counted) {
  // s -> a carries 10 but a -> t only admits 5 units, which it doubles.
  const gmf::Problem problem =
      make_problem(3, 0, 2, {edge(0, 1, 10.0, 1.0), edge(1, 2, 5.0, 2.0)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 10.0, 1e-9);
  CHECK_NEAR(result.source_outflow, 5.0, 1e-9);
  CHECK(result.augmentations.size() == 1);
  // The constriction is the second edge, not the first.
  CHECK(result.augmentations[0].bottleneck_edge == 1);
}

TEST(the_highest_gain_path_is_filled_first) {
  const gmf::Problem problem =
      make_problem(2, 0, 1, {edge(0, 1, 10.0, 0.1), edge(0, 1, 10.0, 0.9)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 10.0, 1e-9);
  CHECK(result.augmentations.size() == 2);
  CHECK_NEAR(result.augmentations[0].gain, 0.9, 1e-9);
  CHECK_NEAR(result.augmentations[1].gain, 0.1, 1e-9);
}

TEST(a_crossing_edge_forces_flow_to_be_pushed_back) {
  // The best first path runs s -> a -> b -> t and claims the only capacity into
  // the sink. Reaching the optimum afterwards means sending flow back over
  // a -> b, which is what the reverse residual arcs are for.
  const gmf::Problem problem = make_problem(4, 0, 3,
                                            {edge(0, 1, 1.0, 1.0),
                                             edge(0, 2, 1.0, 1.0),
                                             edge(1, 3, 1.0, 1.0),
                                             edge(2, 3, 1.0, 2.0),
                                             edge(1, 2, 1.0, 1.5)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 3.0, 1e-9);
  CHECK(result.augmentations.size() >= 2);
  // The first augmentation takes the gain-3 route through the crossing edge.
  CHECK_NEAR(result.augmentations[0].gain, 3.0, 1e-9);

  bool used_a_reverse_step = false;
  for (const gmf::Augmentation& augmentation : result.augmentations) {
    for (const gmf::Step& step : augmentation.steps) {
      if (!step.forward) used_a_reverse_step = true;
    }
  }
  CHECK(used_a_reverse_step);

  const reference::FeasibilityReport report = reference::check_feasible(problem, result);
  CHECK(report.capacities_respected);
  CHECK(report.conserved);
}

TEST(a_flow_generating_cycle_is_cancelled_and_its_excess_routed) {
  // a -> b -> a multiplies flow by three, so one unit around the cycle leaves
  // two units of excess at the cycle, which the route carries to the sink.
  // The source is cut off, so every unit delivered was manufactured.
  const gmf::Problem problem = make_problem(4, 0, 3,
                                            {edge(0, 1, 0.0, 1.0),
                                             edge(1, 2, 1.0, 3.0),
                                             edge(2, 1, 10.0, 1.0),
                                             edge(1, 3, 100.0, 1.0)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 2.0, 1e-9);
  CHECK_NEAR(result.source_outflow, 0.0, 1e-9);
  CHECK(!result.augmentations.empty());
  CHECK(result.augmentations[0].kind == gmf::Augmentation::Kind::GeneratingCycle);
  CHECK_NEAR(result.augmentations[0].excess, 2.0, 1e-9);

  const reference::FeasibilityReport report = reference::check_feasible(problem, result);
  CHECK(report.capacities_respected);
  CHECK(report.conserved);
  CHECK_NEAR(report.sink_inflow, 2.0, 1e-9);
}

TEST(a_generating_cycle_that_cannot_reach_the_sink_is_left_alone) {
  // The amplifying cycle is a dead end: it cannot deliver anything, and it must
  // not stop the ordinary path from being found.
  const gmf::Problem problem = make_problem(5, 0, 1,
                                            {edge(0, 1, 4.0, 0.5),
                                             edge(2, 3, 1.0, 4.0),
                                             edge(3, 2, 1.0, 1.0),
                                             edge(0, 2, 5.0, 1.0)});
  const gmf::Result result = gmf::solve(problem);
  CHECK_MESSAGE(result.optimal, result.status + ": " + result.message);
  CHECK_NEAR(result.max_flow, 2.0, 1e-9);
}

TEST(an_unreachable_sink_yields_no_flow) {
  const gmf::Problem problem = make_problem(3, 0, 2, {edge(0, 1, 5.0, 1.0)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 0.0, 1e-12);
  CHECK(result.augmentations.empty());
}

TEST(zero_capacity_edges_carry_nothing) {
  const gmf::Problem problem = make_problem(2, 0, 1, {edge(0, 1, 0.0, 2.0)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 0.0, 1e-12);
}

TEST(an_uncapacitated_path_is_reported_as_unbounded) {
  const double infinity = std::numeric_limits<double>::infinity();
  const gmf::Problem problem =
      make_problem(3, 0, 2, {edge(0, 1, infinity, 1.0), edge(1, 2, infinity, 0.5)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.status == "unbounded");
  CHECK(!result.optimal);
}

TEST(a_finite_constriction_bounds_an_otherwise_infinite_path) {
  const double infinity = std::numeric_limits<double>::infinity();
  const gmf::Problem problem =
      make_problem(3, 0, 2, {edge(0, 1, infinity, 1.0), edge(1, 2, 7.0, 0.5)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 3.5, 1e-9);
}

TEST(malformed_problems_are_rejected) {
  bool threw = false;
  try {
    gmf::solve(make_problem(2, 0, 1, {edge(0, 1, 1.0, 0.0)}));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  threw = false;
  try {
    gmf::solve(make_problem(2, 0, 0, {edge(0, 1, 1.0, 1.0)}));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  threw = false;
  try {
    gmf::solve(make_problem(2, 0, 1, {edge(0, 5, 1.0, 1.0)}));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(unit_multipliers_reproduce_ordinary_maximum_flow) {
  std::mt19937 engine(20251024);
  for (int trial = 0; trial < 300; ++trial) {
    reference::RandomSpec spec;
    spec.num_nodes = 4 + static_cast<int>(engine() % 9);
    spec.num_edges = spec.num_nodes + static_cast<int>(engine() % 20);
    spec.min_multiplier = 1.0;
    spec.max_multiplier = 1.0;
    const gmf::Problem problem = reference::random_problem(engine, spec);

    reference::Dinic dinic(problem.num_nodes);
    for (const gmf::EdgeInput& e : problem.edges) dinic.add_edge(e.from, e.to, e.capacity);
    const double expected = dinic.max_flow(problem.source, problem.sink);

    const gmf::Result result = gmf::solve(problem);
    CHECK_MESSAGE(result.optimal, "trial " + std::to_string(trial) + ": " + result.status);
    CHECK_MESSAGE(test::near(result.max_flow, expected, 1e-7),
                  "trial " + std::to_string(trial) + ": got " + std::to_string(result.max_flow) +
                      ", Dinic says " + std::to_string(expected));
  }
}

TEST(random_lossy_networks_end_with_a_certificate) {
  std::mt19937 engine(777);
  for (int trial = 0; trial < 300; ++trial) {
    reference::RandomSpec spec;
    spec.num_nodes = 4 + static_cast<int>(engine() % 11);
    spec.num_edges = spec.num_nodes + static_cast<int>(engine() % 25);
    spec.min_multiplier = 0.1;
    spec.max_multiplier = 1.0;
    const gmf::Problem problem = reference::random_problem(engine, spec);
    const gmf::Result result = gmf::solve(problem);

    CHECK_MESSAGE(result.optimal, "trial " + std::to_string(trial) + ": " + result.status + " " +
                                      result.message);
    const reference::FeasibilityReport report = reference::check_feasible(problem, result);
    CHECK_MESSAGE(report.capacities_respected, "trial " + std::to_string(trial));
    CHECK_MESSAGE(report.conserved, "trial " + std::to_string(trial) + ": conservation error " +
                                        std::to_string(report.max_conservation_error));
    CHECK_MESSAGE(test::near(report.sink_inflow, result.max_flow, 1e-7),
                  "trial " + std::to_string(trial));
  }
}

TEST(random_amplifying_networks_end_with_a_certificate) {
  std::mt19937 engine(31337);
  for (int trial = 0; trial < 200; ++trial) {
    reference::RandomSpec spec;
    spec.num_nodes = 4 + static_cast<int>(engine() % 7);
    spec.num_edges = spec.num_nodes + static_cast<int>(engine() % 14);
    spec.min_multiplier = 0.5;
    spec.max_multiplier = 2.0;
    const gmf::Problem problem = reference::random_problem(engine, spec);
    const gmf::Result result = gmf::solve(problem);

    if (result.status == "unbounded") continue;
    CHECK_MESSAGE(result.optimal, "trial " + std::to_string(trial) + ": " + result.status + " " +
                                      result.message);
    const reference::FeasibilityReport report = reference::check_feasible(problem, result);
    CHECK_MESSAGE(report.capacities_respected, "trial " + std::to_string(trial));
    CHECK_MESSAGE(report.conserved, "trial " + std::to_string(trial) + ": conservation error " +
                                        std::to_string(report.max_conservation_error));
  }
}

TEST(the_parallel_engine_finds_the_same_maximum) {
  std::mt19937 engine(4242);
  for (int trial = 0; trial < 120; ++trial) {
    reference::RandomSpec spec;
    spec.num_nodes = 6 + static_cast<int>(engine() % 25);
    spec.num_edges = 2 * spec.num_nodes + static_cast<int>(engine() % 40);
    spec.min_multiplier = 0.2;
    spec.max_multiplier = 1.0;
    const gmf::Problem problem = reference::random_problem(engine, spec);

    const gmf::Result sequential = gmf::solve(problem);
    gmf::Options parallel_options;
    parallel_options.threads = 4;
    const gmf::Result parallel = gmf::solve(problem, parallel_options);

    CHECK_MESSAGE(parallel.optimal, "trial " + std::to_string(trial) + ": " + parallel.status);
    CHECK_MESSAGE(test::near(sequential.max_flow, parallel.max_flow, 1e-7),
                  "trial " + std::to_string(trial) + ": sequential " +
                      std::to_string(sequential.max_flow) + " vs parallel " +
                      std::to_string(parallel.max_flow));
  }
}

TEST(paths_can_be_left_uncollected) {
  const gmf::Problem problem =
      make_problem(3, 0, 2, {edge(0, 1, 4.0, 0.5), edge(1, 2, 4.0, 0.5)});
  gmf::Options options;
  options.collect_paths = false;
  const gmf::Result result = gmf::solve(problem, options);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 1.0, 1e-9);
  CHECK(result.augmentations.empty());
}

TEST(the_reported_steps_follow_the_chain_rule) {
  const gmf::Problem problem = make_problem(4, 0, 3,
                                            {edge(0, 1, 8.0, 0.5),
                                             edge(1, 2, 8.0, 0.5),
                                             edge(2, 3, 8.0, 0.5)});
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK_NEAR(result.max_flow, 1.0, 1e-9);
  CHECK(result.augmentations.size() == 1);

  const gmf::Augmentation& augmentation = result.augmentations[0];
  CHECK(augmentation.steps.size() == 3);
  CHECK_NEAR(augmentation.flow_in, 8.0, 1e-9);
  double carried = augmentation.flow_in;
  for (const gmf::Step& step : augmentation.steps) {
    CHECK_NEAR(step.flow_in, carried, 1e-9);
    CHECK_NEAR(step.flow_out, carried * step.multiplier, 1e-9);
    carried = step.flow_out;
  }
  CHECK_NEAR(carried, augmentation.flow_out, 1e-9);
  CHECK_NEAR(augmentation.gain, 0.125, 1e-9);
  CHECK(augmentation.nodes.size() == 4);
}
