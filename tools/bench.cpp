// Benchmark harness: generates networks of a given shape and size, solves them,
// and reports where the time went.
//
//   gmf_bench                       # the default sweep
//   gmf_bench layered 5000 4 0.9    # shape, nodes, out-degree, max multiplier
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "gmf/gmf.hpp"

namespace {

// A layered network: the shape most flow problems actually have, with long
// paths and many parallel routes, so the path search has real work to do.
gmf::Problem layered(int nodes, int degree, double max_multiplier, unsigned seed) {
  gmf::Problem problem;
  const int width = std::max(2, static_cast<int>(std::sqrt(static_cast<double>(nodes))));
  const int layers = std::max(2, nodes / width);
  problem.num_nodes = layers * width + 2;
  problem.source = problem.num_nodes - 2;
  problem.sink = problem.num_nodes - 1;

  std::mt19937 engine(seed);
  std::uniform_real_distribution<double> capacity(1.0, 100.0);
  std::uniform_real_distribution<double> multiplier(std::min(0.2, max_multiplier),
                                                    max_multiplier);
  std::uniform_int_distribution<int> column(0, width - 1);

  const auto node_at = [width](int layer, int index) { return layer * width + index; };

  for (int i = 0; i < width; ++i) {
    problem.edges.push_back({problem.source, node_at(0, i), capacity(engine), 1.0});
    problem.edges.push_back(
        {node_at(layers - 1, i), problem.sink, capacity(engine), multiplier(engine)});
  }
  for (int layer = 0; layer + 1 < layers; ++layer) {
    for (int i = 0; i < width; ++i) {
      for (int d = 0; d < degree; ++d) {
        problem.edges.push_back({node_at(layer, i), node_at(layer + 1, column(engine)),
                                 capacity(engine), multiplier(engine)});
      }
    }
  }
  return problem;
}

// A uniformly random network, which produces short paths and many cycles.
gmf::Problem uniform(int nodes, int degree, double max_multiplier, unsigned seed) {
  gmf::Problem problem;
  problem.num_nodes = nodes;
  problem.source = 0;
  problem.sink = nodes - 1;

  std::mt19937 engine(seed);
  std::uniform_int_distribution<int> any(0, nodes - 1);
  std::uniform_real_distribution<double> capacity(1.0, 100.0);
  std::uniform_real_distribution<double> multiplier(std::min(0.2, max_multiplier),
                                                    max_multiplier);

  for (int i = 0; i < nodes * degree; ++i) {
    int from = any(engine);
    int to = any(engine);
    if (from == to) to = (to + 1) % nodes;
    problem.edges.push_back({from, to, capacity(engine), multiplier(engine)});
  }
  // Guarantee the sink is reachable so the run is not trivially empty.
  for (int i = 0; i + 1 < nodes; ++i) {
    problem.edges.push_back({i, i + 1, capacity(engine), multiplier(engine)});
  }
  return problem;
}

std::size_t estimate_bytes(const gmf::Problem& problem, const gmf::Result& result) {
  std::size_t bytes = problem.edges.size() * sizeof(gmf::EdgeInput);
  bytes += result.edge_flow.capacity() * sizeof(double);
  bytes += result.steps.capacity() * sizeof(gmf::Step);
  bytes += result.augmentations.capacity() * sizeof(gmf::Augmentation);
  bytes += result.stats.working_bytes;
  return bytes;
}

void run(const std::string& shape, int nodes, int degree, double max_multiplier,
         bool collect_paths) {
  const gmf::Problem problem = shape == "uniform"
                                   ? uniform(nodes, degree, max_multiplier, 12345)
                                   : layered(nodes, degree, max_multiplier, 12345);
  gmf::Options options;
  options.collect_paths = collect_paths;

  const auto start = std::chrono::steady_clock::now();
  const gmf::Result result = gmf::solve(problem, options);
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  const std::size_t steps = result.steps.size();

  std::printf(
      "%-8s n=%-7d m=%-8zu %-16s flow=%-14.6g wall=%7.3fs  path=%6.3fs cycle=%6.3fs "
      "push=%6.3fs reach=%6.3fs sweep=%6.3fs verify=%6.3fs | iters=%-7lld paths=%-7lld cycles=%-6lld relax=%-12lld dij=%-7lld "
      "lc=%-6lld scans=%-12lld steps=%-9zu work=%-5zuMB steps=%-4zuMB total=%zuMB\n",
      shape.c_str(), problem.num_nodes, problem.edges.size(), result.status.c_str(),
      result.max_flow, wall, result.stats.path_search_seconds, result.stats.cycle_search_seconds,
      result.stats.push_seconds, result.stats.reach_seconds, result.stats.sweep_seconds,
      result.stats.verify_seconds, result.stats.iterations, result.stats.path_searches,
      result.stats.cycle_searches, result.stats.relaxations, result.stats.dijkstra_searches,
      result.stats.label_correcting_searches, result.stats.arc_scans, steps,
      result.stats.working_bytes / (1024 * 1024),
      (result.steps.capacity() * sizeof(gmf::Step) +
       result.augmentations.capacity() * sizeof(gmf::Augmentation)) /
          (1024 * 1024),
      estimate_bytes(problem, result) / (1024 * 1024));
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3) {
    const std::string shape = argv[1];
    const int nodes = std::atoi(argv[2]);
    const int degree = argc > 3 ? std::atoi(argv[3]) : 4;
    const double max_multiplier = argc > 4 ? std::atof(argv[4]) : 0.95;
    run(shape, nodes, degree, max_multiplier, true);
    return 0;
  }

  std::printf("== lossy networks (every multiplier below 1, no generating cycles) ==\n");
  for (const int nodes : {1000, 4000, 16000}) {
    run("layered", nodes, 4, 0.95, true);
  }
  for (const int nodes : {1000, 4000, 16000}) {
    run("uniform", nodes, 4, 0.95, true);
  }

  std::printf("\n== gain-amplifying networks (multipliers straddle 1) ==\n");
  for (const int nodes : {200, 800, 3200}) {
    run("layered", nodes, 4, 1.4, true);
  }
  for (const int nodes : {200, 800, 3200}) {
    run("uniform", nodes, 4, 1.4, true);
  }
  return 0;
}
