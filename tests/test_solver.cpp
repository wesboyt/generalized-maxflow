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
  for (const gmf::Step& step : result.steps) {
    if (!step.forward) used_a_reverse_step = true;
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

TEST(the_two_search_engines_find_the_same_maximum) {
  std::mt19937 engine(4242);
  for (int trial = 0; trial < 120; ++trial) {
    reference::RandomSpec spec;
    spec.num_nodes = 6 + static_cast<int>(engine() % 25);
    spec.num_edges = 2 * spec.num_nodes + static_cast<int>(engine() % 40);
    spec.min_multiplier = 0.2;
    spec.max_multiplier = 1.0;
    const gmf::Problem problem = reference::random_problem(engine, spec);

    // Dijkstra over node potentials against the label-correcting reference.
    const gmf::Result sequential = gmf::solve(problem);
    gmf::Options reference_options;
    reference_options.use_potentials = false;
    const gmf::Result parallel = gmf::solve(problem, reference_options);
    CHECK_MESSAGE(sequential.optimal, "trial " + std::to_string(trial));

    CHECK_MESSAGE(parallel.optimal, "trial " + std::to_string(trial) + ": " + parallel.status);
    CHECK_MESSAGE(test::near(sequential.max_flow, parallel.max_flow, 1e-7),
                  "trial " + std::to_string(trial) + ": dijkstra " +
                      std::to_string(sequential.max_flow) + " vs label-correcting " +
                      std::to_string(parallel.max_flow));
  }
}

// Fat-Path with gain-scaling against plain highest-gain augmentation, and the
// rounded phases against unrounded ones, across both regimes. The three share
// the exact finish and the certificate but not the route to them.
TEST(fat_path_agrees_with_plain_highest_gain_augmentation) {
  std::mt19937 engine(19980601);
  for (int trial = 0; trial < 240; ++trial) {
    reference::RandomSpec spec;
    spec.num_nodes = 6 + static_cast<int>(engine() % 30);
    spec.num_edges = 2 * spec.num_nodes + static_cast<int>(engine() % 50);
    spec.min_multiplier = trial % 2 == 0 ? 0.2 : 0.5;
    spec.max_multiplier = trial % 2 == 0 ? 1.0 : 1.8;
    const gmf::Problem problem = reference::random_problem(engine, spec);
    const std::string where = "trial " + std::to_string(trial);

    gmf::Options onaga;
    onaga.fat_path = false;
    gmf::Options unrounded;
    unrounded.gain_scaling = false;
    const gmf::Result scaled = gmf::solve(problem);
    const gmf::Result plain = gmf::solve(problem, onaga);
    const gmf::Result exact_gains = gmf::solve(problem, unrounded);

    if (plain.status == "unbounded") {
      CHECK_MESSAGE(scaled.status == "unbounded", where + ": " + scaled.status);
      continue;
    }
    CHECK_MESSAGE(scaled.optimal, where + ": " + scaled.status + " " + scaled.message);
    CHECK_MESSAGE(exact_gains.optimal, where + ": " + exact_gains.status);
    CHECK_MESSAGE(plain.optimal, where + ": " + plain.status);
    CHECK_MESSAGE(test::near(scaled.max_flow, plain.max_flow, 1e-7),
                  where + ": fat-path " + std::to_string(scaled.max_flow) + " vs onaga " +
                      std::to_string(plain.max_flow));
    CHECK_MESSAGE(test::near(exact_gains.max_flow, plain.max_flow, 1e-7),
                  where + ": unrounded fat-path " + std::to_string(exact_gains.max_flow) +
                      " vs onaga " + std::to_string(plain.max_flow));
    const reference::FeasibilityReport report = reference::check_feasible(problem, scaled);
    CHECK_MESSAGE(report.capacities_respected, where);
    CHECK_MESSAGE(report.conserved, where + ": conservation error " +
                                        std::to_string(report.max_conservation_error));
  }
}

// The phases must actually do the work on an ordinary lossy network, not
// hand everything to the exact finish.
TEST(fat_path_phases_carry_the_flow) {
  std::vector<gmf::EdgeInput> edges;
  // Three parallel two-arc routes of different gain and capacity, so the fat
  // and the highest-gain choices differ.
  edges.push_back(edge(0, 1, 100.0, 0.9));
  edges.push_back(edge(1, 4, 100.0, 0.9));
  edges.push_back(edge(0, 2, 1.0, 0.99));
  edges.push_back(edge(2, 4, 1.0, 0.99));
  edges.push_back(edge(0, 3, 10.0, 0.95));
  edges.push_back(edge(3, 4, 10.0, 0.95));
  const gmf::Problem problem = make_problem(5, 0, 4, edges);
  const gmf::Result result = gmf::solve(problem);
  CHECK(result.optimal);
  CHECK(test::near(result.max_flow, 100 * 0.81 + 1 * 0.9801 + 10 * 0.9025, 1e-9));
  CHECK(result.stats.scaling_phases >= 1);
  CHECK(result.stats.fat_augmentations >= 3);
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
  CHECK(augmentation.step_count == 3);
  CHECK_NEAR(augmentation.flow_in, 8.0, 1e-9);
  double carried = augmentation.flow_in;
  for (std::size_t i = 0; i < augmentation.step_count; ++i) {
    const gmf::Step& step = result.steps[augmentation.step_begin + i];
    CHECK_NEAR(step.flow_in, carried, 1e-9);
    CHECK_NEAR(gmf::step_flow_out(problem, step), carried * gmf::step_multiplier(problem, step),
               1e-9);
    carried = gmf::step_flow_out(problem, step);
  }
  CHECK_NEAR(carried, augmentation.flow_out, 1e-9);
  CHECK_NEAR(augmentation.gain, 0.125, 1e-9);
  // The node sequence follows from the steps rather than being stored.
  CHECK(gmf::step_from(problem, result.steps[augmentation.step_begin]) == 0);
  CHECK(gmf::step_to(problem, result.steps[augmentation.step_begin + 2]) == 3);
}
