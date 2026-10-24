// Independent references the solver is checked against.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <random>
#include <vector>

#include "gmf/gmf.hpp"

namespace reference {

// Dinic's algorithm: the ordinary maximum flow, used to check the generalized
// solver on networks where every multiplier is 1.
class Dinic {
 public:
  explicit Dinic(int num_nodes) : graph_(static_cast<std::size_t>(num_nodes)) {}

  void add_edge(int from, int to, double capacity) {
    graph_[static_cast<std::size_t>(from)].push_back(
        {to, capacity, static_cast<int>(graph_[static_cast<std::size_t>(to)].size())});
    graph_[static_cast<std::size_t>(to)].push_back(
        {from, 0.0, static_cast<int>(graph_[static_cast<std::size_t>(from)].size()) - 1});
  }

  double max_flow(int source, int sink) {
    double total = 0.0;
    while (build_levels(source, sink)) {
      iterator_.assign(graph_.size(), 0);
      while (true) {
        const double pushed = augment(source, sink, std::numeric_limits<double>::infinity());
        if (pushed <= 1e-12) break;
        total += pushed;
      }
    }
    return total;
  }

 private:
  struct Edge {
    int to;
    double capacity;
    int twin;
  };

  bool build_levels(int source, int sink) {
    level_.assign(graph_.size(), -1);
    std::queue<int> queue;
    level_[static_cast<std::size_t>(source)] = 0;
    queue.push(source);
    while (!queue.empty()) {
      const int node = queue.front();
      queue.pop();
      for (const Edge& edge : graph_[static_cast<std::size_t>(node)]) {
        if (edge.capacity > 1e-12 && level_[static_cast<std::size_t>(edge.to)] < 0) {
          level_[static_cast<std::size_t>(edge.to)] = level_[static_cast<std::size_t>(node)] + 1;
          queue.push(edge.to);
        }
      }
    }
    return level_[static_cast<std::size_t>(sink)] >= 0;
  }

  double augment(int node, int sink, double limit) {
    if (node == sink) return limit;
    std::size_t& cursor = iterator_[static_cast<std::size_t>(node)];
    for (; cursor < graph_[static_cast<std::size_t>(node)].size(); ++cursor) {
      Edge& edge = graph_[static_cast<std::size_t>(node)][cursor];
      if (edge.capacity <= 1e-12) continue;
      if (level_[static_cast<std::size_t>(edge.to)] != level_[static_cast<std::size_t>(node)] + 1) {
        continue;
      }
      const double pushed = augment(edge.to, sink, std::min(limit, edge.capacity));
      if (pushed > 1e-12) {
        edge.capacity -= pushed;
        graph_[static_cast<std::size_t>(edge.to)][static_cast<std::size_t>(edge.twin)].capacity +=
            pushed;
        return pushed;
      }
    }
    return 0.0;
  }

  std::vector<std::vector<Edge>> graph_;
  std::vector<int> level_;
  std::vector<std::size_t> iterator_;
};

struct RandomSpec {
  int num_nodes = 8;
  int num_edges = 16;
  double max_capacity = 10.0;
  double min_multiplier = 0.25;
  double max_multiplier = 1.0;
  bool integral_capacities = true;
};

inline gmf::Problem random_problem(std::mt19937& engine, const RandomSpec& spec) {
  gmf::Problem problem;
  problem.num_nodes = spec.num_nodes;
  problem.source = 0;
  problem.sink = spec.num_nodes - 1;

  std::uniform_int_distribution<int> node(0, spec.num_nodes - 1);
  std::uniform_real_distribution<double> capacity(1.0, spec.max_capacity);
  std::uniform_real_distribution<double> multiplier(spec.min_multiplier, spec.max_multiplier);

  for (int i = 0; i < spec.num_edges; ++i) {
    gmf::EdgeInput edge;
    edge.from = node(engine);
    edge.to = node(engine);
    if (edge.from == edge.to) {
      edge.to = (edge.to + 1) % spec.num_nodes;
    }
    edge.capacity = capacity(engine);
    if (spec.integral_capacities) edge.capacity = std::max(1.0, std::round(edge.capacity));
    edge.multiplier = multiplier(engine);
    problem.edges.push_back(edge);
  }
  return problem;
}

// Checks the invariants any answer must satisfy, whatever the algorithm did.
struct FeasibilityReport {
  bool capacities_respected = true;
  bool conserved = true;
  double max_conservation_error = 0.0;
  double sink_inflow = 0.0;
};

inline FeasibilityReport check_feasible(const gmf::Problem& problem, const gmf::Result& result,
                                        double tolerance = 1e-7) {
  FeasibilityReport report;
  std::vector<double> net(static_cast<std::size_t>(problem.num_nodes), 0.0);
  for (std::size_t i = 0; i < problem.edges.size(); ++i) {
    const gmf::EdgeInput& edge = problem.edges[i];
    const double flow = result.edge_flow[i];
    if (flow < -tolerance || flow > edge.capacity + tolerance) report.capacities_respected = false;
    net[static_cast<std::size_t>(edge.from)] -= flow;
    net[static_cast<std::size_t>(edge.to)] += flow * edge.multiplier;
  }
  for (int i = 0; i < problem.num_nodes; ++i) {
    if (i == problem.source || i == problem.sink) continue;
    const double error = std::fabs(net[static_cast<std::size_t>(i)]);
    report.max_conservation_error = std::max(report.max_conservation_error, error);
    if (error > tolerance) report.conserved = false;
  }
  report.sink_inflow = net[static_cast<std::size_t>(problem.sink)];
  return report;
}

}  // namespace reference
