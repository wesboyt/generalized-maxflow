#include "gmf/gmf.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

// Set to 0 to build without the parallel sweep, as the WebAssembly build does.
#ifndef GMF_ENABLE_THREADS
#define GMF_ENABLE_THREADS 1
#endif
#if GMF_ENABLE_THREADS
#include <thread>
#endif

namespace gmf {
namespace {

const double kNegInf = -std::numeric_limits<double>::infinity();
const double kPosInf = std::numeric_limits<double>::infinity();

// A residual arc. Arcs come in twins: `a` and `a ^ 1` are the two directions of
// one input edge, so `a >> 1` is the input edge index and `a & 1` says which
// direction it is. Pushing x units into arc `a` always means
//   residual[a]     -= x
//   residual[a ^ 1] += x * gain[a]
// which is the chain rule for the multiplier, in both directions.
struct Arc {
  int to = 0;
  double residual = 0.0;
  double gain = 1.0;
  double log_gain = 0.0;
  bool blocked = false;
};

class Solver {
 public:
  Solver(const Problem& problem, const Options& options)
      : problem_(problem), options_(options), num_nodes_(problem.num_nodes) {
    validate();
    build();
  }

  Result run() {
    Result result;
    result.stats.threads_used = thread_count_;

    const long long max_iterations =
        options_.max_iterations > 0
            ? options_.max_iterations
            : 1000LL + 20LL * (static_cast<long long>(num_nodes_) +
                               static_cast<long long>(problem_.edges.size()));

    bool unbounded = false;
    bool hit_limit = false;
    // Onaga's theorem: augmenting along a maximum-gain path cannot create a
    // flow-generating cycle. So once a scan reports none, no further scan is
    // needed until a cancellation perturbs the residual network.
    bool generating_cycles_possible = options_.cancel_generating_cycles;
    int amplifying_retries = 0;

    while (true) {
      if (result.stats.iterations >= max_iterations) {
        hit_limit = true;
        break;
      }

      // Every search runs inside the set of nodes that can still reach the
      // sink. Nothing outside it can carry flow to the sink or form a
      // generalized augmenting path, and excluding it keeps a gain-amplifying
      // cycle in a dead end from making the labels diverge.
      reaches_sink_ = nodes_reaching_sink();
      mask_ = &reaches_sink_;

      if (generating_cycles_possible) {
        ++result.stats.cycle_searches;
        const std::vector<int> cycle = find_generating_cycle();
        if (!cycle.empty()) {
          ++result.stats.iterations;
          if (cancel_and_route(cycle, result) == PushOutcome::Unbounded) {
            unbounded = true;
            break;
          }
          continue;  // A cancellation may expose further generating cycles.
        }
        generating_cycles_possible = false;
        if (amplifying_retries > 0) break;  // The two scans disagree; stop here.
      }

      if (!reaches_sink_[static_cast<std::size_t>(problem_.source)]) break;

      ++result.stats.path_searches;
      std::vector<int> path;
      if (!find_max_gain_path(problem_.source, problem_.sink, path, result.stats)) {
        // A gain-amplifying cycle sits inside the sink-reaching subgraph, so it
        // is a generalized augmenting path. Let the cancellation phase take it.
        if (options_.cancel_generating_cycles && ++amplifying_retries <= 2) {
          generating_cycles_possible = true;
          continue;
        }
        break;
      }
      amplifying_retries = 0;
      if (path.empty()) break;  // No residual source -> sink path: done.

      ++result.stats.iterations;
      if (push_walk(path, 0, result) == PushOutcome::Unbounded) {
        unbounded = true;
        break;
      }
    }

    finalize(result, unbounded, hit_limit);
    return result;
  }

 private:
  enum class PushOutcome { Pushed, Blocked, Unbounded };

  void validate() {
    if (num_nodes_ <= 0) throw std::invalid_argument("num_nodes must be positive");
    if (problem_.source < 0 || problem_.source >= num_nodes_) {
      throw std::invalid_argument("source node is out of range");
    }
    if (problem_.sink < 0 || problem_.sink >= num_nodes_) {
      throw std::invalid_argument("sink node is out of range");
    }
    if (problem_.source == problem_.sink) {
      throw std::invalid_argument("source and sink must differ");
    }
    for (std::size_t i = 0; i < problem_.edges.size(); ++i) {
      const EdgeInput& edge = problem_.edges[i];
      const std::string where = " (edge " + std::to_string(i) + ")";
      if (edge.from < 0 || edge.from >= num_nodes_ || edge.to < 0 || edge.to >= num_nodes_) {
        throw std::invalid_argument("edge endpoint is out of range" + where);
      }
      if (std::isnan(edge.capacity) || edge.capacity < 0.0) {
        throw std::invalid_argument("capacity must be non-negative" + where);
      }
      if (std::isnan(edge.multiplier) || std::isinf(edge.multiplier) || edge.multiplier <= 0.0) {
        throw std::invalid_argument("multiplier must be finite and positive" + where);
      }
    }
  }

  void build() {
    const std::size_t edge_count = problem_.edges.size();
    arcs_.reserve(edge_count * 2);
    out_.assign(static_cast<std::size_t>(num_nodes_), {});
    in_.assign(static_cast<std::size_t>(num_nodes_), {});
    edge_flow_.assign(edge_count, 0.0);

    capacity_scale_ = 1.0;
    for (const EdgeInput& edge : problem_.edges) {
      if (std::isfinite(edge.capacity)) capacity_scale_ = std::max(capacity_scale_, edge.capacity);
    }
    const double epsilon = std::max(options_.epsilon, 0.0);
    capacity_epsilon_ = epsilon * capacity_scale_;
    log_epsilon_ = std::max(epsilon, 1e-15);

    for (std::size_t i = 0; i < edge_count; ++i) {
      const EdgeInput& edge = problem_.edges[i];
      Arc forward;
      forward.to = edge.to;
      forward.residual = edge.capacity;
      forward.gain = edge.multiplier;
      forward.log_gain = std::log(edge.multiplier);

      Arc backward;
      backward.to = edge.from;
      backward.residual = 0.0;
      backward.gain = 1.0 / edge.multiplier;
      backward.log_gain = -forward.log_gain;

      const int index = static_cast<int>(arcs_.size());
      arcs_.push_back(forward);
      arcs_.push_back(backward);
      out_[static_cast<std::size_t>(edge.from)].push_back(index);
      out_[static_cast<std::size_t>(edge.to)].push_back(index + 1);
      in_[static_cast<std::size_t>(edge.to)].push_back(index);
      in_[static_cast<std::size_t>(edge.from)].push_back(index + 1);
    }

    // The highest-gain arc out of a node comes first, so the label-correcting
    // sweep settles promising labels before dominated ones and re-relaxes less.
    for (std::size_t node = 0; node < out_.size(); ++node) {
      std::sort(out_[node].begin(), out_[node].end(), [this](int lhs, int rhs) {
        if (arcs_[static_cast<std::size_t>(lhs)].gain != arcs_[static_cast<std::size_t>(rhs)].gain) {
          return arcs_[static_cast<std::size_t>(lhs)].gain >
                 arcs_[static_cast<std::size_t>(rhs)].gain;
        }
        return lhs < rhs;
      });
    }

    label_.assign(static_cast<std::size_t>(num_nodes_), kNegInf);
    parent_.assign(static_cast<std::size_t>(num_nodes_), -1);
    in_queue_.assign(static_cast<std::size_t>(num_nodes_), 0);
    visit_count_.assign(static_cast<std::size_t>(num_nodes_), 0);
    stamp_at_.assign(static_cast<std::size_t>(num_nodes_), 0);
    arc_coefficient_.assign(arcs_.size(), 0.0);
    arc_delta_.assign(arcs_.size(), 0.0);

#if GMF_ENABLE_THREADS
    thread_count_ = options_.threads;
    if (thread_count_ == 0) {
      thread_count_ = static_cast<int>(std::thread::hardware_concurrency());
      if (thread_count_ <= 0) thread_count_ = 1;
    }
    thread_count_ = std::max(1, thread_count_);
    if (thread_count_ > 1) {
      const std::size_t count = static_cast<std::size_t>(num_nodes_);
      atomic_label_ = std::make_unique<std::atomic<double>[]>(count);
      atomic_parent_ = std::make_unique<std::atomic<int>[]>(count);
      queued_ = std::make_unique<std::atomic<bool>[]>(count);
    }
#else
    thread_count_ = 1;
#endif
  }

  const Arc& arc_at(int arc) const { return arcs_[static_cast<std::size_t>(arc)]; }
  Arc& arc_at(int arc) { return arcs_[static_cast<std::size_t>(arc)]; }
  int tail(int arc) const { return arc_at(arc ^ 1).to; }

  bool usable(int arc) const {
    const Arc& a = arc_at(arc);
    return !a.blocked && a.residual > capacity_epsilon_;
  }

  bool allowed(int node) const {
    return mask_ == nullptr || (*mask_)[static_cast<std::size_t>(node)] != 0;
  }

  std::vector<char> nodes_reaching_sink() const {
    std::vector<char> reaches(static_cast<std::size_t>(num_nodes_), 0);
    std::vector<int> stack;
    reaches[static_cast<std::size_t>(problem_.sink)] = 1;
    stack.push_back(problem_.sink);
    while (!stack.empty()) {
      const int node = stack.back();
      stack.pop_back();
      for (const int arc : in_[static_cast<std::size_t>(node)]) {
        if (!usable(arc)) continue;
        const int from = tail(arc);
        if (!reaches[static_cast<std::size_t>(from)]) {
          reaches[static_cast<std::size_t>(from)] = 1;
          stack.push_back(from);
        }
      }
    }
    return reaches;
  }

  // Label-correcting search in log-gain space. Maximising a sum of log gains
  // maximises the product of multipliers, and log space keeps long lossy paths
  // from underflowing to zero.
  //
  // Returns false when a gain-amplifying cycle is reachable from `source`,
  // which makes the maximum-gain label unbounded.
  bool relax_sequential(int source, Stats& stats) {
    std::fill(label_.begin(), label_.end(), kNegInf);
    std::fill(parent_.begin(), parent_.end(), -1);
    std::fill(in_queue_.begin(), in_queue_.end(), char(0));
    std::fill(visit_count_.begin(), visit_count_.end(), 0);

    label_[static_cast<std::size_t>(source)] = 0.0;
    std::deque<int> queue;
    queue.push_back(source);
    in_queue_[static_cast<std::size_t>(source)] = 1;

    while (!queue.empty()) {
      const int node = queue.front();
      queue.pop_front();
      in_queue_[static_cast<std::size_t>(node)] = 0;
      const double base = label_[static_cast<std::size_t>(node)];
      for (const int arc : out_[static_cast<std::size_t>(node)]) {
        if (!usable(arc)) continue;
        const int next = arc_at(arc).to;
        if (!allowed(next)) continue;
        const double candidate = base + arc_at(arc).log_gain;
        if (candidate > label_[static_cast<std::size_t>(next)] + log_epsilon_) {
          label_[static_cast<std::size_t>(next)] = candidate;
          parent_[static_cast<std::size_t>(next)] = arc;
          ++stats.relaxations;
          if (!in_queue_[static_cast<std::size_t>(next)]) {
            if (++visit_count_[static_cast<std::size_t>(next)] > num_nodes_) return false;
            in_queue_[static_cast<std::size_t>(next)] = 1;
            queue.push_back(next);
          }
        }
      }
    }
    return true;
  }

#if GMF_ENABLE_THREADS
  // The same relaxation, fanned out across threads: each round expands the
  // entire current frontier at once. That frontier is exactly the set of nodes
  // whose best incoming gain is still in question after a constriction was
  // removed, so the work grows and shrinks with the affected region.
  bool relax_parallel(int source, Stats& stats) {
    const std::size_t count = static_cast<std::size_t>(num_nodes_);
    for (std::size_t i = 0; i < count; ++i) {
      atomic_label_[i].store(kNegInf, std::memory_order_relaxed);
      atomic_parent_[i].store(-1, std::memory_order_relaxed);
      queued_[i].store(false, std::memory_order_relaxed);
    }
    atomic_label_[static_cast<std::size_t>(source)].store(0.0, std::memory_order_relaxed);

    std::vector<int> frontier{source};
    std::vector<std::vector<int>> next(static_cast<std::size_t>(thread_count_));
    std::atomic<long long> relaxations{0};
    int rounds = 0;

    while (!frontier.empty()) {
      if (++rounds > num_nodes_ + 1) return false;  // gain-amplifying cycle
      for (std::vector<int>& bucket : next) bucket.clear();

      const auto worker = [&](int slot) {
        std::vector<int>& local = next[static_cast<std::size_t>(slot)];
        long long local_relaxations = 0;
        for (std::size_t i = static_cast<std::size_t>(slot); i < frontier.size();
             i += static_cast<std::size_t>(thread_count_)) {
          const int node = frontier[i];
          const double base =
              atomic_label_[static_cast<std::size_t>(node)].load(std::memory_order_relaxed);
          if (base == kNegInf) continue;
          for (const int arc : out_[static_cast<std::size_t>(node)]) {
            if (!usable(arc)) continue;
            const int next_node = arc_at(arc).to;
            if (!allowed(next_node)) continue;
            const double candidate = base + arc_at(arc).log_gain;
            std::atomic<double>& slot_label = atomic_label_[static_cast<std::size_t>(next_node)];
            double current = slot_label.load(std::memory_order_relaxed);
            while (candidate > current + log_epsilon_) {
              if (slot_label.compare_exchange_weak(current, candidate,
                                                   std::memory_order_relaxed)) {
                // The parent is stored outside the label exchange, so a race can
                // leave it pointing at a slightly worse predecessor. The path is
                // re-checked against its label below and a mismatch falls back
                // to the sequential sweep.
                atomic_parent_[static_cast<std::size_t>(next_node)].store(
                    arc, std::memory_order_relaxed);
                ++local_relaxations;
                if (!queued_[static_cast<std::size_t>(next_node)].exchange(
                        true, std::memory_order_relaxed)) {
                  local.push_back(next_node);
                }
                break;
              }
            }
          }
        }
        relaxations.fetch_add(local_relaxations, std::memory_order_relaxed);
      };

      std::vector<std::thread> pool;
      pool.reserve(static_cast<std::size_t>(thread_count_ - 1));
      for (int slot = 1; slot < thread_count_; ++slot) pool.emplace_back(worker, slot);
      worker(0);
      for (std::thread& thread : pool) thread.join();

      frontier.clear();
      for (const std::vector<int>& bucket : next) {
        for (const int node : bucket) {
          queued_[static_cast<std::size_t>(node)].store(false, std::memory_order_relaxed);
          frontier.push_back(node);
        }
      }
    }

    stats.relaxations += relaxations.load();
    for (std::size_t i = 0; i < count; ++i) {
      label_[i] = atomic_label_[i].load(std::memory_order_relaxed);
      parent_[i] = atomic_parent_[i].load(std::memory_order_relaxed);
    }
    return true;
  }
#endif  // GMF_ENABLE_THREADS

  bool extract_parent_path(int source, int target, std::vector<int>& path) const {
    path.clear();
    int node = target;
    int guard = 0;
    while (node != source) {
      if (++guard > num_nodes_ + 1) return false;
      const int arc = parent_[static_cast<std::size_t>(node)];
      if (arc < 0) return false;
      path.push_back(arc);
      node = tail(arc);
    }
    std::reverse(path.begin(), path.end());
    return true;
  }

  bool path_matches_label(const std::vector<int>& path, int target) const {
    double total = 0.0;
    for (const int arc : path) total += arc_at(arc).log_gain;
    const double expected = label_[static_cast<std::size_t>(target)];
    return std::fabs(total - expected) <= 1e-9 * (1.0 + std::fabs(expected));
  }

  // Fills `path` with a maximum-gain residual path, or leaves it empty when the
  // target is unreachable. Returns false when a gain-amplifying cycle made the
  // labels diverge.
  bool find_max_gain_path(int source, int target, std::vector<int>& path, Stats& stats) {
#if GMF_ENABLE_THREADS
    bool parallel = thread_count_ > 1;
    bool ok = parallel ? relax_parallel(source, stats) : relax_sequential(source, stats);
#else
    bool parallel = false;
    bool ok = relax_sequential(source, stats);
#endif
    if (parallel && !ok) {
      // Confirm sequentially: the parallel round bound is the coarser test.
      ++stats.parallel_fallbacks;
      parallel = false;
      ok = relax_sequential(source, stats);
    }
    if (!ok) return false;

    path.clear();
    if (label_[static_cast<std::size_t>(target)] == kNegInf) return true;
    if (extract_parent_path(source, target, path) && path_matches_label(path, target)) return true;
    if (!parallel) {
      path.clear();
      return true;
    }
    ++stats.parallel_fallbacks;
    if (!relax_sequential(source, stats)) return false;
    path.clear();
    if (label_[static_cast<std::size_t>(target)] == kNegInf) return true;
    if (!extract_parent_path(source, target, path)) path.clear();
    return true;
  }

  // Any residual path, used to carry a cycle's excess to the sink when the
  // maximum-gain search cannot produce one.
  bool find_any_path(int source, int target, std::vector<int>& path) {
    std::fill(parent_.begin(), parent_.end(), -1);
    std::vector<char> seen(static_cast<std::size_t>(num_nodes_), 0);
    std::deque<int> queue;
    seen[static_cast<std::size_t>(source)] = 1;
    queue.push_back(source);
    while (!queue.empty()) {
      const int node = queue.front();
      queue.pop_front();
      if (node == target) return extract_parent_path(source, target, path);
      for (const int arc : out_[static_cast<std::size_t>(node)]) {
        if (!usable(arc)) continue;
        const int next = arc_at(arc).to;
        if (!allowed(next) || seen[static_cast<std::size_t>(next)]) continue;
        seen[static_cast<std::size_t>(next)] = 1;
        parent_[static_cast<std::size_t>(next)] = arc;
        queue.push_back(next);
      }
    }
    path.clear();
    return false;
  }

  // Finds a residual cycle whose multipliers multiply to more than 1. Such a
  // cycle, together with a route to the sink, is a generalized augmenting path:
  // it manufactures flow out of nothing, so no flow that admits one is optimal.
  //
  // Every allowed node starts at label 0, so this looks for a cycle anywhere in
  // the subgraph rather than one reachable from a particular node. Full
  // Bellman-Ford rounds are used rather than a queue: with the queue variant the
  // parent pointers at the moment of detection can lead out of the cycle, and a
  // cycle that is missed is reported as optimality.
  std::vector<int> find_generating_cycle() {
    std::fill(label_.begin(), label_.end(), kNegInf);
    std::fill(parent_.begin(), parent_.end(), -1);
    for (int node = 0; node < num_nodes_; ++node) {
      if (allowed(node)) label_[static_cast<std::size_t>(node)] = 0.0;
    }

    bool changed = true;
    for (int round = 0; round < num_nodes_ && changed; ++round) {
      changed = false;
      for (int node = 0; node < num_nodes_; ++node) {
        if (!allowed(node)) continue;
        const double base = label_[static_cast<std::size_t>(node)];
        for (const int arc : out_[static_cast<std::size_t>(node)]) {
          if (!usable(arc)) continue;
          const int next = arc_at(arc).to;
          if (!allowed(next)) continue;
          if (base + arc_at(arc).log_gain >
              label_[static_cast<std::size_t>(next)] + log_epsilon_) {
            label_[static_cast<std::size_t>(next)] = base + arc_at(arc).log_gain;
            parent_[static_cast<std::size_t>(next)] = arc;
            changed = true;
          }
        }
      }
    }
    if (!changed) return {};  // The labels settled: no amplifying cycle.

    // Something is still improving after n rounds, so the parent graph holds a
    // cycle. Any arc that can still be relaxed leads to one.
    for (int node = 0; node < num_nodes_; ++node) {
      if (!allowed(node)) continue;
      const double base = label_[static_cast<std::size_t>(node)];
      for (const int arc : out_[static_cast<std::size_t>(node)]) {
        if (!usable(arc)) continue;
        const int next = arc_at(arc).to;
        if (!allowed(next)) continue;
        if (base + arc_at(arc).log_gain <=
            label_[static_cast<std::size_t>(next)] + log_epsilon_) {
          continue;
        }
        const std::vector<int> cycle = extract_cycle(next);
        if (!cycle.empty()) return cycle;
      }
    }
    return {};
  }

  // Follows parent pointers from `start` until a node repeats, which closes a
  // cycle, and returns it in forward order.
  std::vector<int> extract_cycle(int start) {
    ++stamp_;
    int node = start;
    for (int step = 0; step <= num_nodes_ + 1; ++step) {
      if (stamp_at_[static_cast<std::size_t>(node)] == stamp_) break;
      stamp_at_[static_cast<std::size_t>(node)] = stamp_;
      const int arc = parent_[static_cast<std::size_t>(node)];
      if (arc < 0) return {};  // This chain leaves the parent graph; try another.
      node = tail(arc);
    }
    if (stamp_at_[static_cast<std::size_t>(node)] != stamp_) return {};

    std::vector<int> cycle;
    const int anchor = node;
    int walker = anchor;
    do {
      const int arc = parent_[static_cast<std::size_t>(walker)];
      if (arc < 0) return {};
      cycle.push_back(arc);
      walker = tail(arc);
      if (cycle.size() > static_cast<std::size_t>(num_nodes_) + 1) return {};
    } while (walker != anchor);
    std::reverse(cycle.begin(), cycle.end());

    double total = 0.0;
    for (const int arc : cycle) total += arc_at(arc).log_gain;
    if (!(total > log_epsilon_)) return {};  // Not actually gain-amplifying.
    return cycle;
  }

  struct Bottleneck {
    double amount = 0.0;
    int arc = -1;  // the limiting residual arc
    bool unbounded = false;
  };

  // The distinct input edges a walk touches.
  void collect_edges(const std::vector<int>& walk) {
    scratch_edges_.clear();
    scratch_edges_.reserve(walk.size());
    for (const int arc : walk) scratch_edges_.push_back(arc >> 1);
    std::sort(scratch_edges_.begin(), scratch_edges_.end());
    scratch_edges_.erase(std::unique(scratch_edges_.begin(), scratch_edges_.end()),
                         scratch_edges_.end());
  }

  // The constriction: what runs out first when x units enter the head of the
  // walk. `coefficient[i]` is how many units enter walk[i] per unit of x, which
  // is the running product of the multipliers ahead of it.
  //
  // The limit is computed per input edge on the NET change to its flow, not per
  // arc. A walk may use an edge in both directions -- a cycle running one way
  // and the route carrying its excess running the other -- and those two uses
  // cancel rather than compete, because pushing into the backward arc hands
  // capacity back to the forward one. Charging each direction separately is
  // feasible but far too pessimistic: it shrinks every augmentation to a
  // fraction of what the edge allows, and the cancellation loop then creeps
  // toward the optimum geometrically instead of saturating something and
  // finishing.
  Bottleneck bottleneck_of(const std::vector<int>& walk,
                           const std::vector<double>& coefficient) {
    collect_edges(walk);
    for (std::size_t i = 0; i < walk.size(); ++i) {
      arc_coefficient_[static_cast<std::size_t>(walk[i])] += coefficient[i];
    }

    Bottleneck result;
    result.amount = kPosInf;
    result.unbounded = true;
    for (const int edge : scratch_edges_) {
      const int forward = edge * 2;
      const double gain = arc_at(forward).gain;
      const double entering = arc_coefficient_[static_cast<std::size_t>(forward)];
      const double retracted = arc_coefficient_[static_cast<std::size_t>(forward + 1)];
      // Units of this edge's flow added per unit of x. Pushing `retracted * x`
      // into the backward arc removes `retracted * x / gain` units of flow.
      const double net = entering - retracted / gain;

      double limit;
      int limiting;
      if (net > 0.0) {
        const double residual = arc_at(forward).residual;
        if (!std::isfinite(residual)) continue;  // Uncapacitated: no limit here.
        limit = residual / net;
        limiting = forward;
      } else if (net < 0.0) {
        // The edge's flow goes down, and it cannot go below zero. The backward
        // residual is gain * flow, so the flow itself is residual / gain.
        limit = arc_at(forward + 1).residual / (-net * gain);
        limiting = forward + 1;
      } else {
        continue;  // The two directions cancel exactly.
      }
      if (limit < result.amount) {
        result.amount = limit;
        result.arc = limiting;
        result.unbounded = false;
      }
    }

    for (const int edge : scratch_edges_) {
      arc_coefficient_[static_cast<std::size_t>(edge * 2)] = 0.0;
      arc_coefficient_[static_cast<std::size_t>(edge * 2 + 1)] = 0.0;
    }
    return result;
  }

  // Records one arc traversal and books the amount it carries, without touching
  // the residuals: a walk that uses an edge both ways would pass through an
  // infeasible intermediate state if each traversal were applied as it came, so
  // the amounts are accumulated and the net is applied once per edge afterwards.
  void record_push(int arc, double amount, Augmentation* record) {
    const Arc& forward = arc_at(arc);
    if (record != nullptr) {
      Step step;
      step.edge = arc >> 1;
      step.from = tail(arc);
      step.to = forward.to;
      step.forward = (arc & 1) == 0;
      step.flow_in = amount;
      step.flow_out = amount * forward.gain;
      step.multiplier = forward.gain;
      record->steps.push_back(step);
    }
    arc_delta_[static_cast<std::size_t>(arc)] += amount;
  }

  // Flow is tracked per input edge rather than derived from a residual, so that
  // infinite capacities stay well defined; both residuals follow from it.
  void set_edge_flow(int edge, double flow) {
    const double capacity = problem_.edges[static_cast<std::size_t>(edge)].capacity;
    if (flow < 0.0 && flow > -capacity_epsilon_) flow = 0.0;
    if (flow > capacity && flow < capacity + capacity_epsilon_) flow = capacity;
    edge_flow_[static_cast<std::size_t>(edge)] = flow;
    const int forward = edge * 2;
    arc_at(forward).residual = std::isfinite(capacity) ? capacity - flow : capacity;
    arc_at(forward + 1).residual = flow * arc_at(forward).gain;
  }

  // Pushes along a walk. `cycle_length > 0` means the first `cycle_length` arcs
  // form a flow-generating cycle and the rest is the route that carries the
  // excess the cycle produces. Both halves share one bottleneck computation, so
  // the excess is always fully routed and conservation is never broken.
  PushOutcome push_walk(const std::vector<int>& walk, std::size_t cycle_length, Result& result) {
    std::vector<double> coefficient(walk.size(), 1.0);
    double cycle_gain = 1.0;
    for (std::size_t i = 0; i < cycle_length; ++i) {
      coefficient[i] = cycle_gain;
      cycle_gain *= arc_at(walk[i]).gain;
    }
    if (cycle_length > 0 && !(cycle_gain > 1.0)) return PushOutcome::Blocked;
    double running = cycle_length > 0 ? cycle_gain - 1.0 : 1.0;
    for (std::size_t i = cycle_length; i < walk.size(); ++i) {
      coefficient[i] = running;
      running *= arc_at(walk[i]).gain;
    }

    const Bottleneck limit = bottleneck_of(walk, coefficient);
    if (limit.unbounded) return PushOutcome::Unbounded;
    if (!(limit.amount > 0.0)) {
      // Nothing can move through the constriction; retire it so the search stops
      // rediscovering the same walk.
      arc_at(limit.arc).blocked = true;
      ++blocked_arcs_;
      return PushOutcome::Blocked;
    }

    Augmentation augmentation;
    augmentation.kind =
        cycle_length > 0 ? Augmentation::Kind::GeneratingCycle : Augmentation::Kind::Path;
    augmentation.flow_in = limit.amount;
    augmentation.cycle_length = cycle_length;
    Augmentation* record = options_.collect_paths ? &augmentation : nullptr;

    double carried = limit.amount;
    for (std::size_t i = 0; i < cycle_length; ++i) {
      record_push(walk[i], carried, record);
      carried *= arc_at(walk[i]).gain;
    }
    if (cycle_length > 0) {
      augmentation.excess = carried - limit.amount;
      carried = augmentation.excess;
    }
    for (std::size_t i = cycle_length; i < walk.size(); ++i) {
      record_push(walk[i], carried, record);
      carried *= arc_at(walk[i]).gain;
    }

    // Apply the net change to each edge the walk touched.
    collect_edges(walk);
    for (const int edge : scratch_edges_) {
      const std::size_t forward = static_cast<std::size_t>(edge) * 2;
      const double delta =
          arc_delta_[forward] - arc_delta_[forward + 1] * arc_at(static_cast<int>(forward + 1)).gain;
      arc_delta_[forward] = 0.0;
      arc_delta_[forward + 1] = 0.0;
      set_edge_flow(edge, edge_flow_[static_cast<std::size_t>(edge)] + delta);
    }

    augmentation.flow_out = carried;
    augmentation.gain = carried / limit.amount;
    augmentation.bottleneck_edge = limit.arc >> 1;
    augmentation.bottleneck_forward = (limit.arc & 1) == 0;
    augmentation.bottleneck_step = 0;
    for (std::size_t i = 0; i < walk.size(); ++i) {
      if (walk[i] == limit.arc) {
        augmentation.bottleneck_step = i;
        break;
      }
    }

    if (options_.collect_paths) {
      augmentation.nodes = walk_nodes(walk);
      result.augmentations.push_back(std::move(augmentation));
    }
    return PushOutcome::Pushed;
  }

  PushOutcome cancel_and_route(const std::vector<int>& cycle, Result& result) {
    const int origin = tail(cycle.front());
    std::vector<int> route;
    if (!find_max_gain_path(origin, problem_.sink, route, result.stats) || route.empty()) {
      if (!find_any_path(origin, problem_.sink, route)) return PushOutcome::Blocked;
    }
    std::vector<int> walk = cycle;
    walk.insert(walk.end(), route.begin(), route.end());
    return push_walk(walk, cycle.size(), result);
  }

  std::vector<int> walk_nodes(const std::vector<int>& walk) const {
    std::vector<int> nodes;
    nodes.reserve(walk.size() + 1);
    if (walk.empty()) return nodes;
    nodes.push_back(tail(walk.front()));
    for (const int arc : walk) nodes.push_back(arc_at(arc).to);
    return nodes;
  }

  void finalize(Result& result, bool unbounded, bool hit_limit) {
    const std::size_t edge_count = problem_.edges.size();
    result.edge_flow.assign(edge_count, 0.0);
    for (std::size_t i = 0; i < edge_count; ++i) {
      double flow = edge_flow_[i];
      const double capacity = problem_.edges[i].capacity;
      if (flow < 0.0) {
        result.certificate.max_capacity_violation =
            std::max(result.certificate.max_capacity_violation, -flow);
        flow = 0.0;
      } else if (flow > capacity) {
        result.certificate.max_capacity_violation =
            std::max(result.certificate.max_capacity_violation, flow - capacity);
        flow = capacity;
      }
      result.edge_flow[i] = flow;
    }

    std::vector<double> net(static_cast<std::size_t>(num_nodes_), 0.0);
    for (std::size_t i = 0; i < edge_count; ++i) {
      const EdgeInput& edge = problem_.edges[i];
      const double flow = result.edge_flow[i];
      net[static_cast<std::size_t>(edge.from)] -= flow;
      net[static_cast<std::size_t>(edge.to)] += flow * edge.multiplier;
    }
    for (int node = 0; node < num_nodes_; ++node) {
      if (node == problem_.source || node == problem_.sink) continue;
      result.certificate.max_conservation_error = std::max(
          result.certificate.max_conservation_error, std::fabs(net[static_cast<std::size_t>(node)]));
    }

    result.max_flow = net[static_cast<std::size_t>(problem_.sink)];
    result.source_outflow = -net[static_cast<std::size_t>(problem_.source)];

    if (unbounded) {
      result.status = "unbounded";
      result.optimal = false;
      result.max_flow = kPosInf;
      result.message =
          "the network admits unbounded flow: a path or flow-generating cycle reaching the sink "
          "has no finite capacity";
      return;
    }

    // Optimality certificate. A generalized flow is maximum exactly when its
    // residual network holds no augmenting path and no flow-generating cycle
    // that can reach the sink.
    reaches_sink_ = nodes_reaching_sink();
    mask_ = &reaches_sink_;
    Stats ignored;
    const bool search_ok = relax_sequential(problem_.source, ignored);
    result.certificate.no_augmenting_path =
        search_ok && label_[static_cast<std::size_t>(problem_.sink)] == kNegInf;
    result.certificate.no_generating_cycle_reaching_sink = find_generating_cycle().empty();

    const bool certified = result.certificate.no_augmenting_path &&
                           result.certificate.no_generating_cycle_reaching_sink;
    if (certified) {
      result.status = "optimal";
      result.optimal = true;
      if (blocked_arcs_ > 0) {
        result.message = std::to_string(blocked_arcs_) +
                         " arc(s) were retired by the epsilon guard, so the certificate holds up "
                         "to the configured tolerance";
      }
    } else if (hit_limit) {
      result.status = "iteration_limit";
      result.message = "stopped at the iteration limit before the optimality certificate held";
    } else if (!result.certificate.no_generating_cycle_reaching_sink &&
               !options_.cancel_generating_cycles) {
      result.status = "generating_cycle_rejected";
      result.message =
          "the network has a flow-generating cycle reaching the sink and cycle cancellation is "
          "disabled";
    } else {
      result.status = "suboptimal";
      result.message =
          "the solver stopped without a full optimality certificate; try a smaller epsilon";
    }
  }

  const Problem& problem_;
  const Options& options_;
  int num_nodes_ = 0;
  int thread_count_ = 1;
  long long blocked_arcs_ = 0;

  std::vector<Arc> arcs_;
  std::vector<std::vector<int>> out_;
  std::vector<std::vector<int>> in_;
  std::vector<double> edge_flow_;
  std::vector<double> arc_coefficient_;
  std::vector<double> arc_delta_;
  std::vector<int> scratch_edges_;

  std::vector<double> label_;
  std::vector<int> parent_;
  std::vector<char> in_queue_;
  std::vector<int> visit_count_;
  std::vector<long long> stamp_at_;
  long long stamp_ = 0;

  std::vector<char> reaches_sink_;
  const std::vector<char>* mask_ = nullptr;

  std::unique_ptr<std::atomic<double>[]> atomic_label_;
  std::unique_ptr<std::atomic<int>[]> atomic_parent_;
  std::unique_ptr<std::atomic<bool>[]> queued_;

  double capacity_scale_ = 1.0;
  double capacity_epsilon_ = 0.0;
  double log_epsilon_ = 1e-15;
};

}  // namespace

Result solve(const Problem& problem, const Options& options) {
  Solver solver(problem, options);
  return solver.run();
}

const char* version() { return "0.1.0"; }

}  // namespace gmf
