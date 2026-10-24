#include "gmf/gmf.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gmf {
namespace {

const double kInf = std::numeric_limits<double>::infinity();
const double kNegInf = -kInf;

// Adds its lifetime to a counter, so the dominant phase is measured rather than
// guessed. Tardos and Wayne report that cycle cancellation took as much as 90%
// of the time in their implementation, which is not a thing to assume either way.
class Timer {
 public:
  explicit Timer(double& sink) : sink_(sink), start_(std::chrono::steady_clock::now()) {}
  ~Timer() {
    sink_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

 private:
  double& sink_;
  std::chrono::steady_clock::time_point start_;
};

// The residual network, laid out for the inner loop.
//
// Arcs come in twins, one per direction of an input edge, and are numbered so
// that every arc leaving a node is contiguous. The relaxation loop then walks
// `head`, `residual` and `cost` straight through memory with no indirection,
// which is the whole point of the layout: those three arrays are read once per
// arc per search and nothing else is touched.
//
// What is *not* stored per arc matters as much. The multiplier, capacity and
// flow live once per edge, not twice per arc, and the gain of a backward arc is
// the reciprocal of its twin's. Pushing x units into arc `a` means
//   flow(edge) += x        when a runs forwards
//   flow(edge) -= x / g    when a runs backwards
// and both residuals follow from the flow, which is also what keeps infinite
// capacities well defined.
struct Layout {
  // Per node, size num_nodes + 1.
  std::vector<int32_t> arc_begin;
  // Per arc, size 2 * num_edges. Hot: read by every search.
  std::vector<int32_t> head;
  std::vector<double> residual;
  std::vector<double> cost;  // -log(multiplier); minimising it maximises gain
  // Per arc, cold: read only when pushing.
  std::vector<int32_t> twin;
  std::vector<int32_t> code;  // edge index in the high bits, direction in bit 0
  // Per edge. The multiplier and capacity are not copied here: they live in the
  // caller's Problem, which outlives the solve, and are only read on the push
  // path. At 16 bytes per edge the copy was a sixth of the whole footprint.
  std::vector<int32_t> forward_arc;
  std::vector<double> flow;
  // One bit per arc; an arc is retired only when nothing can move through it.
  std::vector<uint64_t> blocked;

  std::size_t bytes() const {
    return arc_begin.capacity() * sizeof(int32_t) + head.capacity() * sizeof(int32_t) +
           residual.capacity() * sizeof(double) + cost.capacity() * sizeof(double) +
           twin.capacity() * sizeof(int32_t) + code.capacity() * sizeof(int32_t) +
           forward_arc.capacity() * sizeof(int32_t) + flow.capacity() * sizeof(double) +
           blocked.capacity() * sizeof(uint64_t);
  }
};

// Everything the path search reads per node, in one place. The relaxation loop
// touches a node's distance, potential, stamps and parent together, and keeping
// them in separate arrays cost four random memory streams per arc scanned --
// which is what the inner loop spends its time on once the graph no longer fits
// in cache.
struct NodeState {
  double distance = 0.0;
  double potential = 0.0;
  uint32_t distance_stamp = 0;
  uint32_t settled_stamp = 0;
  int32_t parent = -1;
  int32_t padding = 0;
};
static_assert(sizeof(NodeState) == 32, "NodeState should stay at two per cache line");

struct HeapEntry {
  double key;
  int32_t node;
};

// A 4-ary heap. The path search pushes once per improving relaxation, which on
// a large network is millions of sifts, and a 4-ary heap is about half as deep
// as a binary one while still comparing only a handful of siblings per level.
class MinHeap {
 public:
  void clear() { entries_.clear(); }
  bool empty() const { return entries_.empty(); }
  std::size_t capacity() const { return entries_.capacity(); }

  void push(double key, int32_t node) {
    entries_.push_back({key, node});
    std::size_t index = entries_.size() - 1;
    while (index > 0) {
      const std::size_t parent = (index - 1) >> 2;
      if (entries_[parent].key <= entries_[index].key) break;
      std::swap(entries_[parent], entries_[index]);
      index = parent;
    }
  }

  HeapEntry pop() {
    const HeapEntry top = entries_.front();
    entries_.front() = entries_.back();
    entries_.pop_back();
    const std::size_t size = entries_.size();
    std::size_t index = 0;
    while (true) {
      const std::size_t first = index * 4 + 1;
      if (first >= size) break;
      std::size_t best = first;
      const std::size_t last = std::min(first + 4, size);
      for (std::size_t child = first + 1; child < last; ++child) {
        if (entries_[child].key < entries_[best].key) best = child;
      }
      if (entries_[index].key <= entries_[best].key) break;
      std::swap(entries_[index], entries_[best]);
      index = best;
    }
    return top;
  }

 private:
  std::vector<HeapEntry> entries_;
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
    stats_ = &result.stats;
    Timer total(result.stats.seconds);

    const long long max_iterations =
        options_.max_iterations > 0
            ? options_.max_iterations
            : 1000LL + 20LL * (static_cast<long long>(num_nodes_) +
                               static_cast<long long>(problem_.edges.size()));

    bool unbounded = false;
    bool hit_limit = false;
    int stalled_scans = 0;

    // Tardos-Wayne Rounded Fat-Path. In the paper it closes the gap to the
    // optimum in O(log(n/xi)) Delta-phases of at most n + 2m augmentations
    // each; here it leaves the loop below only the rounding error to remove.
    // The label-correcting reference engine skips it, so the two engines stay
    // independent.
    if (options_.fat_path && options_.use_potentials) {
      Timer scaling(result.stats.scaling_seconds);
      run_fat_path(result, max_iterations, unbounded, hit_limit);
      // The rounding error the phases leave is a set of slightly generating
      // cycles through arcs the potentials still almost certify, which is
      // exactly what the Dijkstra-based half of CancelCycles is for.
      if (routes_certified_ && !unbounded && !hit_limit) {
        const CancelOutcome outcome = cancel_violations(result, max_iterations);
        unbounded = outcome == CancelOutcome::Unbounded;
        hit_limit = outcome == CancelOutcome::Limit;
      }
    }

    // The exact finish: Onaga's highest-gain augmentation on the true gains,
    // cancelling whatever flow-generating cycles the rounding left behind.
    while (!unbounded && !hit_limit) {
      if (result.stats.iterations >= max_iterations) {
        hit_limit = true;
        break;
      }

      // One sweep does two jobs. It hunts for a flow-generating cycle in the
      // subgraph that can still reach the sink, and if there is none it leaves
      // behind node potentials with every reduced cost non-negative. Those
      // potentials are themselves the proof that no such cycle exists, which is
      // why the two searches are the same search: a separate cycle scan per
      // iteration was costing as much as the path searches.
      if (!potentials_valid_) {
        ++result.stats.cycle_searches;
        if (!establish_potentials(result.stats)) {
          // The sweep found a gain-amplifying cycle instead.
          if (!options_.cancel_generating_cycles) break;
          ++result.stats.iterations;
          ++result.stats.cycles_cancelled;
          const PushOutcome outcome = cancel_and_route(cycle_buffer_, result);
          if (outcome == PushOutcome::Unbounded) {
            unbounded = true;
            break;
          }
          if (outcome == PushOutcome::Blocked && ++stalled_scans > 2) break;
          if (outcome == PushOutcome::Pushed) stalled_scans = 0;
          continue;
        }
        stalled_scans = 0;
      }

      if (!reaches_sink_[static_cast<std::size_t>(problem_.source)]) break;

      ++result.stats.path_searches;
      if (!find_max_gain_path(problem_.source, problem_.sink, path_buffer_, result.stats)) {
        continue;  // Potentials were stale; the next pass rebuilds them.
      }
      if (path_buffer_.empty()) break;  // No residual source -> sink path: done.

      ++result.stats.iterations;
      const PushOutcome outcome = push_walk(path_buffer_, 0, result);
      if (outcome == PushOutcome::Unbounded) {
        unbounded = true;
        break;
      }
    }

    finalize(result, unbounded, hit_limit);
    result.stats.working_bytes = working_bytes();
    return result;
  }

 private:
  enum class PushOutcome { Pushed, Blocked, Unbounded };
  enum class CancelOutcome { Lossy, Rejected, Unbounded, Limit, Stalled };

  // ---- Fat-Path with gain-scaling ------------------------------------------
  //
  // Tardos and Wayne, "Simple generalized maximum flow algorithms", IPCO 1998,
  // section 6, algorithm RFP:
  //
  //   round the gains to powers of b = (1 + xi)^(1/n)
  //   Delta <- an upper bound on OPT - |h|
  //   repeat
  //     CancelCycles             -- the residual network becomes lossy
  //     FatAugmentations(Delta / 2m)
  //     Delta <- Delta / 2
  //   until Delta is small
  //
  // FatAugmentations repeatedly augments along a highest-gain path among the
  // delta-fat ones: paths every arc of which can carry enough to raise the
  // sink's excess by delta. Measured in sink units through labels mu, an arc
  // (v, w) is fat when r(v, w) / mu(v) >= delta. With node potentials pi the
  // labels are mu(v) = exp(pi(t) - pi(v)): reduced costs non-negative make
  // exp(pi(v) - pi(t)) an upper bound on the gain of every v -> t path, and
  // the bound is exact along the tree the last search built.
  //
  // Rounding a gain down to a power of b is rounding its cost -log(gain) up to
  // a multiple of q = log b. In a lossy network relabelled by pi the costs are
  // the reduced costs, so the rounded network H is Dijkstra on integer keys
  // ceil(c_pi / q) -- and a search on those keys leaves every reduced cost
  // above -q, which is exactly the tolerance the next search needs. So the
  // phases maintain q-optimal rather than exact potentials, CancelCycles only
  // has to remove the cycles that gain more than q in total, and both
  // tolerances shrink with Delta through xi = Delta / Delta_0.
  //
  // Two departures from the paper, both forced by reporting a conserving flow
  // rather than one with excesses:
  //
  //  - The flow lives in the original network, not in H. A path chosen in H
  //    is pushed with its true gains, so it delivers at least what H promised
  //    and no "interpretation" step strands excess at interior nodes.
  //  - CancelCycles routes each cancelled cycle's excess to the sink along a
  //    residual path (a GAP augmentation) instead of leaving it on the cycle.
  //
  // The paper's exact algorithm finishes by rounding a B^(-4m)-optimal flow,
  // which is meaningless in floating point. Here the phases stop once Delta is
  // below the tolerance and the exact loop in run() takes over, at q = 0, from
  // a flow that is already within the last Delta of optimal.
  void run_fat_path(Result& result, long long max_iterations, bool& unbounded, bool& hit_limit) {
    Stats& stats = result.stats;

    // Phase zero is an exact CancelCycles: the bound below needs a lossy
    // network, and this is where an amplifying input's cycles are first met.
    switch (cancel_cycles(result, max_iterations)) {
      case CancelOutcome::Lossy:
        break;
      case CancelOutcome::Unbounded:
        unbounded = true;
        return leave_fat_path();
      case CancelOutcome::Limit:
        hit_limit = true;
        return leave_fat_path();
      default:
        return leave_fat_path();  // Let the exact loop report it.
    }
    if (!reaches_sink_[static_cast<std::size_t>(problem_.source)]) return leave_fat_path();
    canonical_labels();
    routes_certified_ = true;

    const double initial_gap = discrepancy_bound();
    if (!(initial_gap > 0.0) || !std::isfinite(initial_gap)) return leave_fat_path();

    const double edges = static_cast<double>(std::max<std::size_t>(1, problem_.edges.size()));
    // Below this the remaining gap is within the rounding of the answer itself,
    // and the exact finish is cheaper than further halvings.
    const double stop = std::max(capacity_epsilon_, initial_gap * 1e-12) / (2.0 * edges);
    double gap = initial_gap;

    while (true) {
      const double fatness = gap / (2.0 * edges);
      if (fatness <= stop) break;
      ++stats.scaling_phases;

      if (options_.gain_scaling) {
        const double xi = std::min(0.5, gap / initial_gap);
        slack_ = std::log1p(xi) / static_cast<double>(std::max(1, num_nodes_));
      }
      // Delta halved, so arcs that were too thin are fat now and the
      // potentials were only maintained on the old fat subnetwork. Re-tighten
      // them over every residual arc, starting from where they are.
      potentials_valid_ = false;
      repair_all_next_ = true;

      fat_ = fatness;
      skipped_fatness_ = 0.0;
      bool finished = false;  // no residual path of any fatness is left
      int drifts = 0;
      while (true) {
        if (!potentials_valid_) {
          CancelOutcome outcome = cancel_violations(result, max_iterations);
          if (outcome == CancelOutcome::Lossy) outcome = cancel_cycles(result, max_iterations);
          if (outcome == CancelOutcome::Unbounded) {
            unbounded = true;
            return leave_fat_path();
          }
          if (outcome == CancelOutcome::Limit) {
            hit_limit = true;
            return leave_fat_path();
          }
          if (outcome != CancelOutcome::Lossy) return leave_fat_path();
          if (!reaches_sink_[static_cast<std::size_t>(problem_.source)]) {
            finished = true;
            break;
          }
          canonical_labels();
        }
        if (stats.iterations >= max_iterations) {
          hit_limit = true;
          return leave_fat_path();
        }
        ++stats.path_searches;
        skipped_fatness_ = 0.0;
        if (!find_max_gain_path(problem_.source, problem_.sink, path_buffer_, stats)) {
          // The potentials drifted; the top of the loop repairs them. A repair
          // that keeps failing means the rounded tolerances are fighting the
          // floating point, and the exact finish is the safer place to be.
          if (++drifts > 3) return leave_fat_path();
          continue;
        }
        drifts = 0;
        if (path_buffer_.empty()) {
          // No fat path. If the search skipped nothing, there is no path at all.
          finished = skipped_fatness_ <= 0.0;
          break;
        }
        ++stats.iterations;
        ++stats.fat_augmentations;
        if (push_walk(path_buffer_, 0, result) == PushOutcome::Unbounded) {
          unbounded = true;
          return leave_fat_path();
        }
      }
      fat_ = 0.0;
      if (finished) break;

      // Every s -> t path leaves the fat subnetwork through an arc the search
      // skipped, so no path can deliver more than the fattest of those. Phases
      // that could only find nothing are skipped outright.
      gap = std::min(gap / 2.0, skipped_fatness_ * 2.0 * edges);
    }

    leave_fat_path();
  }

  // Canonical labels: mu(v) is the inverse of the highest gain of any residual
  // v -> t path, so the fatness test r / mu(v) >= delta is exact rather than an
  // upper bound. One Dijkstra backwards from the sink over the reduced costs
  // gives D(v), the reduced cost of the best v -> t path, and pi(v) - D(v) is a
  // potential with pi(v) - pi(t) = log(best gain to t) and every reduced cost
  // still non-negative: c_pi(v, w) - D(v) + D(w) >= 0 is the triangle
  // inequality D(v) <= c_pi(v, w) + D(w).
  //
  // Without this the first phase reads its fatness off whatever potentials
  // the cycle sweep left, which on a lossy network claim a gain of one
  // everywhere, and fills paths that deliver almost nothing -- then spends
  // tens of thousands of cycle cancellations rerouting them.
  void canonical_labels() {
    ++search_stamp_;
    heap_.clear();
    settled_.clear();
    const int sink = problem_.sink;
    set_distance(sink, 0.0);
    heap_.push(0.0, static_cast<int32_t>(sink));
    const bool check_blocked = blocked_arcs_ > 0;
    while (!heap_.empty()) {
      const HeapEntry top = heap_.pop();
      NodeState& here = node_[static_cast<std::size_t>(top.node)];
      if (here.settled_stamp == search_stamp_) continue;
      here.settled_stamp = search_stamp_;
      settled_.push_back(top.node);
      const double potential = here.potential;
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(top.node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(top.node)]; a < end; ++a) {
        // The arcs entering this node are the twins of those leaving it.
        const int incoming = twin_of(a);
        if (net_.residual[static_cast<std::size_t>(incoming)] <= capacity_epsilon_) continue;
        if (check_blocked && blocked(incoming)) continue;
        const int32_t from = net_.head[static_cast<std::size_t>(a)];
        if (!in_mask(from)) continue;
        NodeState& state = node_[static_cast<std::size_t>(from)];
        if (state.settled_stamp == search_stamp_) continue;
        // Within the phase's tolerance a reduced cost may sit just below zero.
        const double reduced = std::max(
            0.0, net_.cost[static_cast<std::size_t>(incoming)] + state.potential - potential);
        const double candidate = top.key + reduced;
        if (state.distance_stamp != search_stamp_ || candidate < state.distance) {
          state.distance = candidate;
          state.distance_stamp = search_stamp_;
          heap_.push(candidate, from);
        }
      }
    }
    for (const int32_t node : settled_) {
      NodeState& state = node_[static_cast<std::size_t>(node)];
      state.potential -= state.distance;
    }
  }

  // Back to exact gains and full potentials for the finish.
  void leave_fat_path() {
    fat_ = 0.0;
    slack_ = 0.0;
    potentials_valid_ = false;
    repair_all_next_ = true;
  }

  double reduced_cost(int arc) const {
    return net_.cost[static_cast<std::size_t>(arc)] +
           node_[static_cast<std::size_t>(tail_of(arc))].potential -
           node_[static_cast<std::size_t>(net_.head[static_cast<std::size_t>(arc)])].potential;
  }

  // The first half of CancelCycles, for when the potentials are known to be
  // valid on every arc except a few. After a Fat-Path phase those few are the
  // thin arcs the phase skipped, and every flow-generating cycle passes through
  // one of them. For such an arc (v, w) the most generating cycle through it is
  // the arc plus the highest-gain w -> v path over the certified arcs: one
  // early-exit Dijkstra, where finding the same cycle with a label-correcting
  // sweep costs a pass over the whole network. The potential update from that
  // search raises the arc's reduced cost by exactly the path's, so afterwards
  // it reads the cycle's log-gain: still negative means the cycle generates.
  //
  // What this cannot reach -- a cycle through two violating arcs -- is left to
  // the sweep in cancel_cycles, which stays the authority on whether the
  // network is lossy.
  CancelOutcome cancel_violations(Result& result, long long max_iterations) {
    compute_reaches_sink();
    const double tolerance = potential_tolerance_ + slack_;
    violations_.clear();
    for (int node = 0; node < num_nodes_; ++node) {
      if (!in_mask(node)) continue;
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
        if (!usable(a) || !in_mask(net_.head[static_cast<std::size_t>(a)])) continue;
        if (reduced_cost(a) < -tolerance) violations_.push_back(a);
      }
    }
    for (const int32_t arc : violations_) {
      // A cancellation saturates one arc of the cycle, not necessarily this
      // one, so an arc may need a few cycles before it is fixed.
      for (int attempt = 0; attempt < 8; ++attempt) {
        if (!usable(arc) || reduced_cost(arc) >= -tolerance) break;
        const int from = net_.head[static_cast<std::size_t>(arc)];
        const int to = tail_of(arc);
        dijkstra_scan_impl<false, false, true>(from, to, result.stats);
        const bool found = found_target_ && build_path(from, to, cycle_path_) && !cycle_path_.empty();
        apply_potential_update();
        if (!found || reduced_cost(arc) >= -tolerance) break;
        if (result.stats.iterations >= max_iterations) return CancelOutcome::Limit;
        cycle_buffer_.assign(1, arc);
        cycle_buffer_.insert(cycle_buffer_.end(), cycle_path_.begin(), cycle_path_.end());
        ++result.stats.iterations;
        ++result.stats.cycles_cancelled;
        const PushOutcome outcome = cancel_and_route(cycle_buffer_, result);
        if (outcome == PushOutcome::Unbounded) return CancelOutcome::Unbounded;
        if (outcome == PushOutcome::Blocked) break;
      }
    }
    return CancelOutcome::Lossy;
  }

  // CancelCycles: cancel flow-generating cycles reaching the sink until the
  // relabelled residual network is lossy, i.e. until valid potentials exist.
  // Each cycle's excess is carried to the sink with it.
  CancelOutcome cancel_cycles(Result& result, long long max_iterations) {
    int stalled = 0;
    while (true) {
      ++result.stats.cycle_searches;
      if (establish_potentials(result.stats)) return CancelOutcome::Lossy;
      if (!options_.cancel_generating_cycles) return CancelOutcome::Rejected;
      if (result.stats.iterations >= max_iterations) return CancelOutcome::Limit;
      ++result.stats.iterations;
      ++result.stats.cycles_cancelled;
      const PushOutcome outcome = cancel_and_route(cycle_buffer_, result);
      if (outcome == PushOutcome::Unbounded) return CancelOutcome::Unbounded;
      if (outcome == PushOutcome::Pushed) stalled = 0;
      if (outcome == PushOutcome::Blocked && ++stalled > 2) return CancelOutcome::Stalled;
    }
  }

  // Delta_0: an upper bound on how much more can reach the sink, from the
  // cheaper of two cuts. Around the sink it is what its residual in-arcs can
  // deliver. Around the source it is the relabelled residual capacity leaving
  // it, r / mu(s), which bounds the sink flow because the relabelled network
  // is lossy. Either may be infinite on uncapacitated arcs.
  double discrepancy_bound() const {
    const int sink = problem_.sink;
    const int source = problem_.source;
    double sink_cut = 0.0;
    const int32_t sink_end = net_.arc_begin[static_cast<std::size_t>(sink) + 1];
    for (int32_t a = net_.arc_begin[static_cast<std::size_t>(sink)]; a < sink_end; ++a) {
      const int incoming = twin_of(a);
      if (!usable(incoming) || !in_mask(net_.head[static_cast<std::size_t>(a)])) continue;
      sink_cut += net_.residual[static_cast<std::size_t>(incoming)] * gain_of(incoming);
    }
    const double to_sink = std::exp(node_[static_cast<std::size_t>(source)].potential -
                                    node_[static_cast<std::size_t>(sink)].potential);
    double source_cut = 0.0;
    const int32_t source_end = net_.arc_begin[static_cast<std::size_t>(source) + 1];
    for (int32_t a = net_.arc_begin[static_cast<std::size_t>(source)]; a < source_end; ++a) {
      if (!usable(a) || !in_mask(net_.head[static_cast<std::size_t>(a)])) continue;
      source_cut += net_.residual[static_cast<std::size_t>(a)] * to_sink;
    }
    return std::min(sink_cut, source_cut);
  }

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
    if (problem_.edges.size() > static_cast<std::size_t>(INT32_MAX / 2)) {
      throw std::invalid_argument("too many edges");
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
    const std::size_t edges = problem_.edges.size();
    const std::size_t arcs = edges * 2;
    const std::size_t nodes = static_cast<std::size_t>(num_nodes_);

    capacity_scale_ = 1.0;
    for (const EdgeInput& edge : problem_.edges) {
      if (std::isfinite(edge.capacity)) capacity_scale_ = std::max(capacity_scale_, edge.capacity);
    }
    const double epsilon = std::max(options_.epsilon, 0.0);
    capacity_epsilon_ = epsilon * capacity_scale_;
    log_epsilon_ = std::max(epsilon, 1e-15);
    // How negative a reduced cost may drift before the potentials are rebuilt.
    potential_tolerance_ = 1e-9;

    net_.arc_begin.assign(nodes + 1, 0);
    for (const EdgeInput& edge : problem_.edges) {
      ++net_.arc_begin[static_cast<std::size_t>(edge.from) + 1];
      ++net_.arc_begin[static_cast<std::size_t>(edge.to) + 1];
    }
    for (std::size_t i = 0; i < nodes; ++i) net_.arc_begin[i + 1] += net_.arc_begin[i];

    net_.head.resize(arcs);
    net_.residual.resize(arcs);
    net_.cost.resize(arcs);
    net_.twin.resize(arcs);
    net_.code.resize(arcs);
    net_.forward_arc.resize(edges);
    net_.flow.assign(edges, 0.0);
    net_.blocked.assign((arcs + 63) / 64, 0);

    std::vector<int32_t> cursor(net_.arc_begin.begin(), net_.arc_begin.end() - 1);
    for (std::size_t i = 0; i < edges; ++i) {
      const EdgeInput& edge = problem_.edges[i];
      const double log_gain = std::log(edge.multiplier);

      const int32_t forward = cursor[static_cast<std::size_t>(edge.from)]++;
      const int32_t backward = cursor[static_cast<std::size_t>(edge.to)]++;
      net_.forward_arc[i] = forward;

      net_.head[static_cast<std::size_t>(forward)] = edge.to;
      net_.residual[static_cast<std::size_t>(forward)] = edge.capacity;
      net_.cost[static_cast<std::size_t>(forward)] = -log_gain;
      net_.twin[static_cast<std::size_t>(forward)] = backward;
      net_.code[static_cast<std::size_t>(forward)] = static_cast<int32_t>(i) << 1;

      net_.head[static_cast<std::size_t>(backward)] = edge.from;
      net_.residual[static_cast<std::size_t>(backward)] = 0.0;
      net_.cost[static_cast<std::size_t>(backward)] = log_gain;
      net_.twin[static_cast<std::size_t>(backward)] = forward;
      net_.code[static_cast<std::size_t>(backward)] = (static_cast<int32_t>(i) << 1) | 1;
    }

    // Stamped scratch: a search touches only the nodes it reaches, so nothing
    // here is cleared between iterations.
    node_.assign(nodes, NodeState());
    reaches_sink_.assign(nodes, 0);

  }

  std::size_t working_bytes() const {
    std::size_t bytes = net_.bytes();
    bytes += node_.capacity() * sizeof(NodeState);
    bytes += reaches_sink_.capacity();
    bytes += heap_.capacity() * sizeof(HeapEntry);
    bytes += settled_.capacity() * sizeof(int32_t);
    bytes += queue_.capacity() * sizeof(int32_t);
    bytes += in_queue_.capacity();
    bytes += visit_count_.capacity() * sizeof(int32_t);
    bytes += arc_coefficient_.capacity() * sizeof(double);
    bytes += arc_delta_.capacity() * sizeof(double);
    bytes += repair_seeds_.capacity() * sizeof(int32_t);
    bytes += label_.capacity() * sizeof(double);
    bytes += colour_.capacity() + colour_stamp_.capacity() * sizeof(uint32_t);
    bytes += walk_stack_.capacity() * sizeof(int32_t);
    bytes += violations_.capacity() * sizeof(int32_t);
    bytes += cycle_path_.capacity() * sizeof(int);
    return bytes;
  }

  // ---- arc accessors -------------------------------------------------------

  int edge_of(int arc) const { return net_.code[static_cast<std::size_t>(arc)] >> 1; }
  bool is_forward(int arc) const {
    return (net_.code[static_cast<std::size_t>(arc)] & 1) == 0;
  }
  int twin_of(int arc) const { return net_.twin[static_cast<std::size_t>(arc)]; }
  int tail_of(int arc) const { return net_.head[static_cast<std::size_t>(twin_of(arc))]; }
  double edge_gain(int edge) const {
    return problem_.edges[static_cast<std::size_t>(edge)].multiplier;
  }
  double edge_capacity(int edge) const {
    return problem_.edges[static_cast<std::size_t>(edge)].capacity;
  }
  double gain_of(int arc) const {
    const double gain = edge_gain(edge_of(arc));
    return is_forward(arc) ? gain : 1.0 / gain;
  }

  bool blocked(int arc) const {
    const std::size_t index = static_cast<std::size_t>(arc);
    return (net_.blocked[index >> 6] >> (index & 63)) & 1ULL;
  }
  void block(int arc) {
    const std::size_t index = static_cast<std::size_t>(arc);
    net_.blocked[index >> 6] |= 1ULL << (index & 63);
    ++blocked_arcs_;
    if (stats_ != nullptr) ++stats_->blocked_arcs;
  }

  bool usable(int arc) const {
    return net_.residual[static_cast<std::size_t>(arc)] > capacity_epsilon_ && !blocked(arc);
  }

  // Flow is tracked per edge; both residuals follow from it.
  void set_edge_flow(int edge, double flow) {
    const std::size_t index = static_cast<std::size_t>(edge);
    const double capacity = edge_capacity(edge);
    if (flow < 0.0 && flow > -capacity_epsilon_) flow = 0.0;
    if (flow > capacity && flow < capacity + capacity_epsilon_) flow = capacity;
    net_.flow[index] = flow;
    const int32_t forward = net_.forward_arc[index];
    net_.residual[static_cast<std::size_t>(forward)] =
        std::isfinite(capacity) ? capacity - flow : capacity;
    net_.residual[static_cast<std::size_t>(net_.twin[static_cast<std::size_t>(forward)])] =
        flow * edge_gain(edge);
  }

  // ---- reachability --------------------------------------------------------

  // Nodes that can still reach the sink. Every augmenting path and every cycle
  // worth cancelling lies inside this set, and confining the searches to it is
  // what keeps a gain-amplifying cycle in a dead end from making the labels
  // diverge. The set can only shrink as arcs saturate: a push creates residual
  // arcs only between nodes that are already in it, so no node outside gains a
  // way in. It is therefore recomputed only alongside the potentials.
  void compute_reaches_sink() {
    Timer timer(stats_ != nullptr ? stats_->reach_seconds : discarded_seconds_);
    std::fill(reaches_sink_.begin(), reaches_sink_.end(), static_cast<char>(0));
    mask_is_full_ = false;
    queue_.clear();
    reaches_sink_[static_cast<std::size_t>(problem_.sink)] = 1;
    queue_.push_back(problem_.sink);
    for (std::size_t cursor = 0; cursor < queue_.size(); ++cursor) {
      const int node = queue_[cursor];
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
        // The arcs entering `node` are the twins of the arcs leaving it.
        const int incoming = twin_of(a);
        if (!usable(incoming)) continue;
        const int from = net_.head[static_cast<std::size_t>(a)];
        if (!reaches_sink_[static_cast<std::size_t>(from)]) {
          reaches_sink_[static_cast<std::size_t>(from)] = 1;
          queue_.push_back(from);
        }
      }
    }
    // When every node is in, the per-arc membership test is pure overhead.
    mask_is_full_ = queue_.size() == static_cast<std::size_t>(num_nodes_);
  }

  bool in_mask(int node) const {
    return mask_is_full_ || reaches_sink_[static_cast<std::size_t>(node)] != 0;
  }

  // ---- potentials and cycle detection -------------------------------------

  // A label-correcting sweep over the sink-reaching subgraph. Every node starts
  // at zero, so it finds a gain-amplifying cycle anywhere in that subgraph
  // rather than only one reachable from the source -- a cycle can manufacture
  // flow with the source cut off entirely.
  //
  // Returns true when there is no such cycle, having left `potential_` with
  // every residual reduced cost non-negative. Returns false and fills
  // `cycle_buffer_` otherwise.
  //
  // `from_scratch` restarts every label at zero. Otherwise the previous labels
  // are kept and the sweep is seeded only from the nodes whose constraints a
  // push can have broken, which are the endpoints of the arcs it touched. That
  // is sound because any labelling satisfying label[v] >= label[u] + log g on
  // every residual arc yields valid potentials -- the minimal one is not
  // required -- and relaxation only ever raises labels, so it converges to a
  // feasible labelling from any starting point and diverges exactly when a
  // gain-amplifying cycle makes the system unsatisfiable. Rebuilding from zero
  // each time was costing a full O(V*E) sweep per cancellation and dominated
  // every amplifying network.
  bool establish_potentials(Stats& stats, bool from_scratch = false) {
    Timer timer(stats.cycle_search_seconds);
    compute_reaches_sink();

    // label[v] is the greatest log-gain of any walk ending at v. Potentials are
    // its negation: with label[v] >= label[u] + log g for every residual arc,
    // the reduced cost -log g + pi[u] - pi[v] is exactly label[v] - label[u] -
    // log g >= 0.
    const std::size_t nodes = static_cast<std::size_t>(num_nodes_);
    if (from_scratch || !potentials_initialized_ || force_full_potentials_) {
      label_.assign(nodes, kNegInf);
      potentials_initialized_ = true;
      force_full_potentials_ = false;
      from_scratch = true;
    }
    in_queue_.assign(nodes, 0);
    visit_count_.assign(nodes, 0);
    queue_.clear();

    if (from_scratch) {
      repair_all_next_ = false;
      for (int node = 0; node < num_nodes_; ++node) {
        if (!in_mask(node)) continue;
        label_[static_cast<std::size_t>(node)] = 0.0;
        in_queue_[static_cast<std::size_t>(node)] = 1;
        queue_.push_back(node);
      }
    } else if (repair_all_next_) {
      // Start from the current potentials and seed every node. Used between
      // Fat-Path phases, where the potentials are good everywhere except on
      // the arcs the previous phase was not allowed to use: one pass over the
      // arcs finds those, and the sweep only works where they were.
      repair_all_next_ = false;
      repair_seeds_.clear();
      for (int node = 0; node < num_nodes_; ++node) {
        if (!in_mask(node)) continue;
        label_[static_cast<std::size_t>(node)] = -node_[static_cast<std::size_t>(node)].potential;
        in_queue_[static_cast<std::size_t>(node)] = 1;
        queue_.push_back(node);
      }
    } else {
      for (const int32_t node : repair_seeds_) {
        if (!in_mask(node) || in_queue_[static_cast<std::size_t>(node)]) continue;
        if (label_[static_cast<std::size_t>(node)] == kNegInf) {
          label_[static_cast<std::size_t>(node)] = 0.0;
        }
        in_queue_[static_cast<std::size_t>(node)] = 1;
        queue_.push_back(node);
      }
      repair_seeds_.clear();
      if (queue_.empty()) {
        potentials_from_labels();
        return true;
      }
    }

    if (!sweep_sequential(stats)) return resolve_sweep_failure();
    potentials_from_labels();
    if (!from_scratch && !potentials_are_feasible()) {
      // The seeded repair did not reach a feasible labelling after all. Rather
      // than trust it, start over from zero. This is a safety net on the fast
      // path, not the expected route, and it costs one pass over the arcs.
      force_full_potentials_ = true;
      return establish_potentials(stats, /*from_scratch=*/true);
    }
    return true;
  }

  // Every residual arc inside the mask must satisfy
  // label[v] >= label[u] + log g, which is exactly "reduced cost non-negative".
  bool potentials_are_feasible() {
    Timer timer(stats_ != nullptr ? stats_->verify_seconds : discarded_seconds_);
    for (int node = 0; node < num_nodes_; ++node) {
      if (!in_mask(node)) continue;
      const double base = label_[static_cast<std::size_t>(node)];
      if (base == kNegInf) return false;
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
        if (!usable(a)) continue;
        const int next = net_.head[static_cast<std::size_t>(a)];
        if (!in_mask(next)) continue;
        if (base - net_.cost[static_cast<std::size_t>(a)] >
            label_[static_cast<std::size_t>(next)] + 1e-9 + slack_) {
          return false;
        }
      }
    }
    return true;
  }

  // Decides what a failed sweep actually means. Returns true when it turns out
  // there is no gain-amplifying cycle and potentials were established anyway.
  bool resolve_sweep_failure() {
    if (cycle_confirmed_) {
      cycle_confirmed_ = false;
      return false;  // cycle_buffer_ already holds a verified cycle.
    }
    cycle_buffer_ = extract_cycle(cycle_seed_);
    if (!cycle_buffer_.empty()) return false;
    // The parent pointers did not close a cycle. Fall back to full
    // Bellman-Ford rounds, which are authoritative either way: they either find
    // a cycle or leave behind a feasible labelling.
    cycle_buffer_ = find_cycle_by_rounds();
    if (!cycle_buffer_.empty()) return false;
    potentials_from_labels();
    return true;
  }

  void potentials_from_labels() {
    for (int node = 0; node < num_nodes_; ++node) {
      if (!in_mask(node)) continue;
      const double label = label_[static_cast<std::size_t>(node)];
      if (label != kNegInf) node_[static_cast<std::size_t>(node)].potential = -label;
    }
    potentials_valid_ = true;
    if (stats_ != nullptr) ++stats_->potential_repairs;
  }

  // Scans the parent forest for a cycle, in one O(V) pass with three-colour
  // marking. Any cycle in it is gain-amplifying: the labels along a parent
  // chain strictly increase, so a chain that closes can only do so by gaining.
  //
  // This is what makes cycle detection affordable. Waiting for a node to be
  // relaxed more than V times, which is the textbook test, means up to V passes
  // over every arc after the cycle has already formed -- on a dense amplifying
  // network that was 14 million relaxations per cycle found. The parent forest
  // closes within a few relaxations of the cycle appearing, so checking it
  // every V relaxations detects the cycle almost immediately and costs O(1)
  // amortised per relaxation.
  int find_parent_cycle_node() {
    const std::size_t nodes = static_cast<std::size_t>(num_nodes_);
    if (colour_.size() != nodes) {
      colour_.assign(nodes, 0);
      colour_stamp_.assign(nodes, 0);
    }
    ++colour_epoch_;
    for (int start = 0; start < num_nodes_; ++start) {
      if (node_[static_cast<std::size_t>(start)].parent < 0) continue;
      if (colour_stamp_[static_cast<std::size_t>(start)] == colour_epoch_) continue;
      walk_stack_.clear();
      int node = start;
      while (node >= 0 && colour_stamp_[static_cast<std::size_t>(node)] != colour_epoch_) {
        colour_stamp_[static_cast<std::size_t>(node)] = colour_epoch_;
        colour_[static_cast<std::size_t>(node)] = 1;  // on the current chain
        walk_stack_.push_back(node);
        const int arc = node_[static_cast<std::size_t>(node)].parent;
        node = arc < 0 ? -1 : tail_of(arc);
      }
      const bool closed = node >= 0 &&
                          colour_stamp_[static_cast<std::size_t>(node)] == colour_epoch_ &&
                          colour_[static_cast<std::size_t>(node)] == 1;
      for (const int32_t visited : walk_stack_) {
        colour_[static_cast<std::size_t>(visited)] = 2;  // settled
      }
      if (closed) return node;
    }
    return -1;
  }

  // Returns false when a gain-amplifying cycle is found; the node it runs
  // through is left in cycle_seed_.
  bool sweep_sequential(Stats& stats) {
    Timer timer(stats.sweep_seconds);
    std::size_t cursor = 0;
    long long relaxations = 0;
    long long since_check = 0;
    long long check_interval = std::max<long long>(32, num_nodes_);
    // Inside a gain-scaled phase a label only has to come within q of every
    // constraint, so improvements smaller than that are not worth propagating
    // and only cycles whose gain survives rounding are found.
    const double improvement = std::max(log_epsilon_, slack_);
    while (cursor < queue_.size()) {
      const int node = queue_[cursor++];
      in_queue_[static_cast<std::size_t>(node)] = 0;
      const double base = label_[static_cast<std::size_t>(node)];
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
        if (net_.residual[static_cast<std::size_t>(a)] <= capacity_epsilon_ || blocked(a)) continue;
        const int next = net_.head[static_cast<std::size_t>(a)];
        if (!in_mask(next)) continue;
        const double candidate = base - net_.cost[static_cast<std::size_t>(a)];
        if (candidate > label_[static_cast<std::size_t>(next)] + improvement) {
          label_[static_cast<std::size_t>(next)] = candidate;
          node_[static_cast<std::size_t>(next)].parent = a;
          ++relaxations;
          if (++since_check >= check_interval) {
            since_check = 0;
            const int seed = find_parent_cycle_node();
            if (seed >= 0) {
              // Confirm it before acting. Relaxation requires an improvement of
              // at least log_epsilon, so a chain can close on a cycle whose
              // gain is not actually above one; treating that as a generalized
              // augmenting path would leave the solver unable to cancel what it
              // claims to have found.
              std::vector<int> cycle = extract_cycle(seed);
              if (!cycle.empty()) {
                cycle_buffer_ = std::move(cycle);
                cycle_confirmed_ = true;
                cycle_seed_ = seed;
                stats.relaxations += relaxations;
                repair_seeds_.push_back(seed);
                for (std::size_t i = cursor > 0 ? cursor - 1 : 0; i < queue_.size(); ++i) {
                  repair_seeds_.push_back(queue_[i]);
                }
                return false;
              }
              // A false alarm: keep sweeping, and back off so the scan does not
              // repeat at the same rate.
              check_interval = std::min<long long>(check_interval * 4, 1LL << 30);
            }
          }
          if (!in_queue_[static_cast<std::size_t>(next)]) {
            if (++visit_count_[static_cast<std::size_t>(next)] > num_nodes_) {
              cycle_seed_ = next;
              stats.relaxations += relaxations;
              // The sweep stops here with work still queued. Those nodes are
              // where the labelling may still be infeasible, so they are handed
              // to the next repair along with the seeds from the push.
              repair_seeds_.push_back(next);
              for (std::size_t i = cursor > 0 ? cursor - 1 : 0; i < queue_.size(); ++i) {
                repair_seeds_.push_back(queue_[i]);
              }
              return false;
            }
            in_queue_[static_cast<std::size_t>(next)] = 1;
            queue_.push_back(next);
          }
        }
      }
      // Reclaim the consumed prefix so the queue does not grow without bound.
      if (cursor > 4096 && cursor * 2 > queue_.size()) {
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor = 0;
      }
    }
    stats.relaxations += relaxations;
    return true;
  }

  // Full Bellman-Ford rounds, used only to extract a cycle the queue-based
  // sweep detected but whose parent pointers had already moved on.
  std::vector<int> find_cycle_by_rounds() {
    const std::size_t nodes = static_cast<std::size_t>(num_nodes_);
    label_.assign(nodes, kNegInf);
    for (NodeState& state : node_) state.parent = -1;
    for (int node = 0; node < num_nodes_; ++node) {
      if (in_mask(node)) label_[static_cast<std::size_t>(node)] = 0.0;
    }
    // At q = 0 -- the certificate, and the exact finish -- this is plain
    // Bellman-Ford. Inside a gain-scaled phase it looks only for cycles whose
    // gain survives rounding, the same as the sweep.
    const double improvement = std::max(log_epsilon_, slack_);

    bool changed = true;
    for (int round = 0; round < num_nodes_ && changed; ++round) {
      changed = false;
      for (int node = 0; node < num_nodes_; ++node) {
        if (!in_mask(node)) continue;
        const double base = label_[static_cast<std::size_t>(node)];
        const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
        for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
          if (!usable(a)) continue;
          const int next = net_.head[static_cast<std::size_t>(a)];
          if (!in_mask(next)) continue;
          const double candidate = base - net_.cost[static_cast<std::size_t>(a)];
          if (candidate > label_[static_cast<std::size_t>(next)] + improvement) {
            label_[static_cast<std::size_t>(next)] = candidate;
            node_[static_cast<std::size_t>(next)].parent = a;
            changed = true;
          }
        }
      }
    }
    if (!changed) return {};

    // The rounds did not settle, so some cycle is generating. Prefer one whose
    // gain survives rounding; failing that, any genuinely generating one, so
    // that a gain-scaled phase never mistakes an unsettled labelling for
    // valid potentials.
    for (const bool strict : {true, false}) {
      if (!strict && !(slack_ > 0.0)) break;
      for (int node = 0; node < num_nodes_; ++node) {
        if (!in_mask(node)) continue;
        const double base = label_[static_cast<std::size_t>(node)];
        const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
        for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
          if (!usable(a)) continue;
          const int next = net_.head[static_cast<std::size_t>(a)];
          if (!in_mask(next)) continue;
          if (base - net_.cost[static_cast<std::size_t>(a)] <=
              label_[static_cast<std::size_t>(next)] + improvement) {
            continue;
          }
          const std::vector<int> cycle = extract_cycle(next, strict);
          if (!cycle.empty()) return cycle;
          if (stats_ != nullptr) ++stats_->cycle_extract_failures;
        }
      }
    }
    return {};
  }

  // Follows parent pointers from `start` until a node repeats, which closes a
  // cycle, and returns it in forward order.
  std::vector<int> extract_cycle(int start, bool strict = true) {
    if (stamp_at_.size() != static_cast<std::size_t>(num_nodes_)) {
      stamp_at_.assign(static_cast<std::size_t>(num_nodes_), 0);
    }
    ++stamp_;
    int node = start;
    for (int step = 0; step <= num_nodes_ + 1; ++step) {
      if (stamp_at_[static_cast<std::size_t>(node)] == stamp_) break;
      stamp_at_[static_cast<std::size_t>(node)] = stamp_;
      const int arc = node_[static_cast<std::size_t>(node)].parent;
      if (arc < 0) return {};  // This chain leaves the parent graph; try another.
      node = tail_of(arc);
    }
    if (stamp_at_[static_cast<std::size_t>(node)] != stamp_) return {};

    std::vector<int> cycle;
    const int anchor = node;
    int walker = anchor;
    do {
      const int arc = node_[static_cast<std::size_t>(walker)].parent;
      if (arc < 0) return {};
      cycle.push_back(arc);
      walker = tail_of(arc);
      if (cycle.size() > static_cast<std::size_t>(num_nodes_) + 1) return {};
    } while (walker != anchor);
    std::reverse(cycle.begin(), cycle.end());

    // Parent pointers outlive the search that set them, so a chain can describe
    // a cycle whose arcs have since been saturated. Such a stale cycle cannot be
    // cancelled, and acting on one used to retire a live arc -- which deleted
    // capacity the optimum needed and hid it from the certificate too.
    for (const int arc : cycle) {
      if (!usable(arc)) return {};
    }
    double total = 0.0;
    for (const int arc : cycle) total -= net_.cost[static_cast<std::size_t>(arc)];
    // Not actually gain-amplifying -- or, inside a gain-scaled phase, not by
    // more than q. That is precisely the set of cycles the q-tolerant sweep
    // cannot converge around: each lap raises every label on the cycle by its
    // total log-gain, and a relaxation needs more than q, so the sweep settles
    // exactly when no cycle gains more than q in total. Cycles below that are
    // the rounding error the refinement and the exact finish remove;
    // cancelling them inside a phase costs a hundredfold more cycles on a
    // network with many near-tied paths.
    const double threshold = strict ? std::max(log_epsilon_, slack_) : log_epsilon_;
    if (!(total > threshold)) return {};
    return cycle;
  }

  // ---- path search ---------------------------------------------------------

  double distance_of(int node) const {
    return node_[static_cast<std::size_t>(node)].distance_stamp == search_stamp_
               ? node_[static_cast<std::size_t>(node)].distance
               : kInf;
  }

  void set_distance(int node, double value) {
    node_[static_cast<std::size_t>(node)].distance = value;
    node_[static_cast<std::size_t>(node)].distance_stamp = search_stamp_;
  }

  // The highest-gain residual path, as an early-exit Dijkstra over reduced
  // costs. This is where the time goes on a large network, and it is the reason
  // for the potentials: a label-correcting sweep per augmentation costs a pass
  // over every arc, while this settles only the nodes nearer than the sink and
  // stops.
  //
  // Returns false when a reduced cost came out negative, meaning the potentials
  // have drifted or were invalidated; the caller rebuilds them.
  // Computes distances and the settled set. The caller then augments and
  // applies the potential update.
  //
  // `kFat` restricts the search to the fat_-fat arcs of a Fat-Path phase, and
  // `kRounded` runs it in the gain-rounded network: keys are the reduced costs
  // rounded up to whole multiples of slack_, so the search tolerates reduced
  // costs down to -slack_ and leaves none below it.
  bool dijkstra_scan(int source, int target, Stats& stats) {
    if (fat_ > 0.0) {
      return slack_ > 0.0 ? dijkstra_scan_impl<true, true, false>(source, target, stats)
                          : dijkstra_scan_impl<true, false, false>(source, target, stats);
    }
    return slack_ > 0.0 ? dijkstra_scan_impl<false, true, false>(source, target, stats)
                        : dijkstra_scan_impl<false, false, false>(source, target, stats);
  }

  // `kLenient` searches only the arcs the potentials still certify, skipping a
  // negative reduced cost instead of giving up on the search. That is the
  // highest-gain route over those arcs, used to carry a cancelled cycle's
  // excess while the potentials are known to be broken elsewhere.
  template <bool kFat, bool kRounded, bool kLenient>
  bool dijkstra_scan_impl(int source, int target, Stats& stats) {
    key_unit_ = kRounded ? slack_ : 1.0;
    const double inverse_unit = kRounded ? 1.0 / slack_ : 1.0;
    const double tolerance = potential_tolerance_ + (kRounded ? slack_ : 0.0);
    const double target_potential = node_[static_cast<std::size_t>(target)].potential;
    double skipped = 0.0;  // the fattest arc left out, relative to fat_
    ++search_stamp_;
    ++stats.dijkstra_searches;
    heap_.clear();
    settled_.clear();
    set_distance(source, 0.0);
    node_[static_cast<std::size_t>(source)].parent = -1;
    heap_.push(0.0, static_cast<int32_t>(source));

    double stop = 0.0;
    bool found = false;
    long long relaxations = 0;

    while (!heap_.empty()) {
      const HeapEntry top = heap_.pop();
      ++stats.heap_pops;
      if (node_[static_cast<std::size_t>(top.node)].settled_stamp == search_stamp_) continue;
      node_[static_cast<std::size_t>(top.node)].settled_stamp = search_stamp_;
      settled_.push_back(top.node);
      ++stats.settled_nodes;
      stop = top.key;
      if (top.node == target) {
        found = true;
        break;
      }

      const double base = top.key;
      const double base_potential = node_[static_cast<std::size_t>(top.node)].potential;
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(top.node) + 1];
      stats.arc_scans += end - net_.arc_begin[static_cast<std::size_t>(top.node)];
      const bool check_blocked = blocked_arcs_ > 0;
      const bool check_mask = !mask_is_full_;
      // An arc out of this node is fat when r / mu(v) >= fat_, i.e. when its
      // residual reaches fat_ * mu(v). One exp per settled node, not per arc.
      const double fat_floor =
          kFat ? fat_ * std::exp(target_potential - base_potential) : 0.0;
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(top.node)]; a < end; ++a) {
        const double residual = net_.residual[static_cast<std::size_t>(a)];
        if (residual <= capacity_epsilon_) continue;
        if (check_blocked && blocked(a)) continue;
        const int32_t next = net_.head[static_cast<std::size_t>(a)];
        if (check_mask && !in_mask(next)) continue;
        NodeState& state = node_[static_cast<std::size_t>(next)];
        if (state.settled_stamp == search_stamp_) continue;
        if constexpr (kFat) {
          if (residual < fat_floor) {
            skipped = std::max(skipped, residual / fat_floor);
            continue;
          }
        }
        double reduced = net_.cost[static_cast<std::size_t>(a)] + base_potential - state.potential;
        if (reduced < -tolerance) {
          if constexpr (kLenient) continue;
          // Drifted far enough to matter: the potentials are no longer a valid
          // reweighting, so stop and let them be rebuilt rather than quietly
          // returning a path that is not the highest-gain one. This asks for a
          // rebuild from zero, not a seeded repair -- a seeded repair has
          // nothing to go on here and would hand back the same bad potentials.
          potentials_valid_ = false;
          force_full_potentials_ = true;
          stats.relaxations += relaxations;
          return false;
        }
        if constexpr (kRounded) {
          // The rounded gain b^(-k): k whole steps of q, never negative. The
          // small allowance keeps an exact multiple from rounding up a step.
          reduced = std::ceil(reduced * inverse_unit - 1e-9);
        }
        if (reduced < 0.0) reduced = 0.0;  // Harmless rounding; keep it monotone.
        const double candidate = base + reduced;
        if (state.distance_stamp != search_stamp_ || candidate < state.distance) {
          state.distance = candidate;
          state.distance_stamp = search_stamp_;
          state.parent = a;
          ++relaxations;
          heap_.push(candidate, next);
        }
      }
    }
    stats.relaxations += relaxations;
    stop_distance_ = stop;
    found_target_ = found;
    if constexpr (kFat) {
      if (!found) skipped_fatness_ = std::max(skipped_fatness_, skipped * fat_);
    } else {
      (void)skipped;
    }
    return true;
  }

  // Re-tighten the potentials over the settled nodes. Shifting every potential
  // by a constant changes no reduced cost, so subtracting the stop distance
  // lets the untouched nodes keep their value and leaves the update
  // proportional to the part of the graph the search actually visited.
  void apply_potential_update() {
    // In the rounded network distances are counted in steps of q.
    for (const int32_t node : settled_) {
      NodeState& state = node_[static_cast<std::size_t>(node)];
      state.potential += (state.distance - stop_distance_) * key_unit_;
    }
  }

  bool build_path(int source, int target, std::vector<int>& path) const {
    path.clear();
    int node = target;
    int guard = 0;
    while (node != source) {
      if (++guard > num_nodes_ + 1) {
        path.clear();
        return true;
      }
      const int arc = node_[static_cast<std::size_t>(node)].parent;
      if (arc < 0) {
        path.clear();
        return true;
      }
      path.push_back(arc);
      node = tail_of(arc);
    }
    std::reverse(path.begin(), path.end());
    return true;
  }

  // Label-correcting path search from one source, for when potentials are off.
  bool label_correcting_path(int source, int target, std::vector<int>& path, Stats& stats) {
    ++stats.label_correcting_searches;
    const std::size_t nodes = static_cast<std::size_t>(num_nodes_);
    label_.assign(nodes, kNegInf);
    in_queue_.assign(nodes, 0);
    visit_count_.assign(nodes, 0);
    for (NodeState& state : node_) state.parent = -1;
    label_[static_cast<std::size_t>(source)] = 0.0;
    queue_.clear();
    queue_.push_back(source);
    in_queue_[static_cast<std::size_t>(source)] = 1;

    if (!sweep_sequential(stats)) {
      path.clear();
      return false;
    }
    path.clear();
    if (label_[static_cast<std::size_t>(target)] == kNegInf) return true;
    return build_path(source, target, path);
  }

  bool find_max_gain_path(int source, int target, std::vector<int>& path, Stats& stats) {
    Timer timer(stats.path_search_seconds);
    if (options_.use_potentials && potentials_valid_) {
      if (!dijkstra_scan(source, target, stats)) {
        return false;  // Potentials invalidated; the caller rebuilds them.
      }
      path.clear();
      const bool found = found_target_;
      if (found && !build_path(source, target, path)) {
        apply_potential_update();
        return false;
      }
      apply_potential_update();
      return true;  // An empty path means no route to the sink, which is an answer.
    }
    return label_correcting_path(source, target, path, stats);
  }

  // The highest-gain residual path over the arcs whose reduced cost is still
  // non-negative. The potential update keeps those arcs certified.
  bool find_route(int source, int target, std::vector<int>& path) {
    if (stats_ == nullptr) return false;
    dijkstra_scan_impl<false, false, true>(source, target, *stats_);
    path.clear();
    const bool found = found_target_ && build_path(source, target, path) && !path.empty();
    apply_potential_update();
    return found;
  }

  // Any residual path, used to carry a cycle's excess to the sink when the
  // highest-gain search cannot produce one.
  bool find_any_path(int source, int target, std::vector<int>& path) {
    for (NodeState& state : node_) state.parent = -1;
    ++search_stamp_;
    queue_.clear();
    queue_.push_back(source);
    node_[static_cast<std::size_t>(source)].settled_stamp = search_stamp_;
    for (std::size_t cursor = 0; cursor < queue_.size(); ++cursor) {
      const int node = queue_[cursor];
      if (node == target) return build_path(source, target, path);
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
        if (!usable(a)) continue;
        const int next = net_.head[static_cast<std::size_t>(a)];
        if (!in_mask(next) || node_[static_cast<std::size_t>(next)].settled_stamp == search_stamp_) {
          continue;
        }
        node_[static_cast<std::size_t>(next)].settled_stamp = search_stamp_;
        node_[static_cast<std::size_t>(next)].parent = a;
        queue_.push_back(next);
      }
    }
    path.clear();
    return false;
  }

  // ---- pushing -------------------------------------------------------------

  struct Bottleneck {
    double amount = 0.0;
    int arc = -1;
    bool unbounded = false;
  };

  // The distinct input edges a walk touches. A simple path cannot repeat an
  // edge, so the common case skips the dedup entirely.
  void collect_edges(const std::vector<int>& walk, bool may_repeat) {
    scratch_edges_.clear();
    scratch_edges_.reserve(walk.size());
    for (const int arc : walk) scratch_edges_.push_back(edge_of(arc));
    if (!may_repeat) return;
    std::sort(scratch_edges_.begin(), scratch_edges_.end());
    scratch_edges_.erase(std::unique(scratch_edges_.begin(), scratch_edges_.end()),
                         scratch_edges_.end());
  }

  // The constriction: what runs out first when x units enter the head of the
  // walk. `coefficient[i]` is how many units enter walk[i] per unit of x, the
  // running product of the multipliers ahead of it.
  //
  // The limit is computed per input edge on the NET change to its flow, not per
  // arc. A walk may use an edge in both directions -- a cycle running one way
  // and the route carrying its excess running the other -- and those uses
  // cancel rather than compete, because pushing into the backward arc hands
  // capacity back to the forward one. Charging each direction separately stays
  // feasible but is far too pessimistic: it shrinks every augmentation to a
  // fraction of what the edge allows, and the cancellation loop then creeps
  // toward the optimum geometrically instead of saturating something.
  Bottleneck bottleneck_of(const std::vector<int>& walk, const std::vector<double>& coefficient,
                           bool may_repeat) {
    Bottleneck result;
    result.amount = kInf;
    result.unbounded = true;

    if (!may_repeat) {
      // One pass, no accumulator: each edge appears once and in one direction.
      for (std::size_t i = 0; i < walk.size(); ++i) {
        const int arc = walk[i];
        const double residual = net_.residual[static_cast<std::size_t>(arc)];
        if (!std::isfinite(residual)) continue;
        const double limit = residual / coefficient[i];
        if (limit < result.amount) {
          result.amount = limit;
          result.arc = arc;
          result.unbounded = false;
        }
      }
      return result;
    }

    if (arc_coefficient_.size() != net_.residual.size()) {
      arc_coefficient_.assign(net_.residual.size(), 0.0);
    }
    collect_edges(walk, true);
    for (std::size_t i = 0; i < walk.size(); ++i) {
      arc_coefficient_[static_cast<std::size_t>(walk[i])] += coefficient[i];
    }

    for (const int edge : scratch_edges_) {
      const int forward = net_.forward_arc[static_cast<std::size_t>(edge)];
      const int backward = net_.twin[static_cast<std::size_t>(forward)];
      const double gain = edge_gain(edge);
      const double entering = arc_coefficient_[static_cast<std::size_t>(forward)];
      const double retracted = arc_coefficient_[static_cast<std::size_t>(backward)];
      // Units of this edge's flow added per unit of x. Pushing `retracted * x`
      // into the backward arc removes `retracted * x / gain` units of flow.
      const double net = entering - retracted / gain;

      double limit;
      int limiting;
      if (net > 0.0) {
        const double residual = net_.residual[static_cast<std::size_t>(forward)];
        if (!std::isfinite(residual)) continue;  // Uncapacitated: no limit here.
        limit = residual / net;
        limiting = forward;
      } else if (net < 0.0) {
        // The edge's flow goes down and cannot pass zero. The backward residual
        // is gain * flow, so the flow itself is that over the gain.
        limit = net_.residual[static_cast<std::size_t>(backward)] / (-net * gain);
        limiting = backward;
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
      const int forward = net_.forward_arc[static_cast<std::size_t>(edge)];
      arc_coefficient_[static_cast<std::size_t>(forward)] = 0.0;
      arc_coefficient_[static_cast<std::size_t>(net_.twin[static_cast<std::size_t>(forward)])] =
          0.0;
    }
    return result;
  }

  // Pushes along a walk. `cycle_length > 0` means the first that many arcs form
  // a flow-generating cycle and the rest is the route carrying the excess the
  // cycle produces. Both halves share one bottleneck computation, so the excess
  // is always fully routed and conservation is never broken.
  PushOutcome push_walk(const std::vector<int>& walk, std::size_t cycle_length, Result& result) {
    Timer timer(result.stats.push_seconds);
    const bool may_repeat = cycle_length > 0;

    coefficient_.assign(walk.size(), 1.0);
    double cycle_gain = 1.0;
    for (std::size_t i = 0; i < cycle_length; ++i) {
      coefficient_[i] = cycle_gain;
      cycle_gain *= gain_of(walk[i]);
    }
    if (cycle_length > 0 && !(cycle_gain > 1.0)) return PushOutcome::Blocked;
    double running = cycle_length > 0 ? cycle_gain - 1.0 : 1.0;
    for (std::size_t i = cycle_length; i < walk.size(); ++i) {
      coefficient_[i] = running;
      running *= gain_of(walk[i]);
    }

    const Bottleneck limit = bottleneck_of(walk, coefficient_, may_repeat);
    if (limit.unbounded) return PushOutcome::Unbounded;
    if (!(limit.amount > 0.0)) {
      // Retire the constriction only when it genuinely cannot carry anything.
      // Retiring a live arc deletes capacity the optimum may need and hides it
      // from the certificate, which is how a suboptimal answer once came back
      // labelled optimal.
      if (!usable(limit.arc)) block(limit.arc);
      return PushOutcome::Blocked;
    }

    Augmentation augmentation;
    augmentation.kind =
        cycle_length > 0 ? Augmentation::Kind::GeneratingCycle : Augmentation::Kind::Path;
    augmentation.flow_in = limit.amount;
    augmentation.cycle_length = cycle_length;
    augmentation.step_begin = result.steps.size();
    augmentation.step_count = walk.size();

    if (arc_delta_.size() != net_.residual.size() && may_repeat) {
      arc_delta_.assign(net_.residual.size(), 0.0);
    }

    double carried = limit.amount;
    for (std::size_t i = 0; i < walk.size(); ++i) {
      if (i == cycle_length && cycle_length > 0) {
        augmentation.excess = carried - limit.amount;
        carried = augmentation.excess;
      }
      const int arc = walk[i];
      if (options_.collect_paths) {
        Step step;
        step.edge = edge_of(arc);
        step.forward = is_forward(arc);
        step.flow_in = carried;
        result.steps.push_back(step);
      }
      if (may_repeat) {
        arc_delta_[static_cast<std::size_t>(arc)] += carried;
        // Pushing here creates residual capacity on the twin, which is the only
        // way a potential constraint can become violated.
        repair_seeds_.push_back(net_.head[static_cast<std::size_t>(arc)]);
        repair_seeds_.push_back(tail_of(arc));
      } else {
        // A simple path touches each edge once, so apply it straight away.
        const int edge = edge_of(arc);
        const double delta = is_forward(arc) ? carried : -carried * gain_of(arc);
        set_edge_flow(edge, net_.flow[static_cast<std::size_t>(edge)] + delta);
      }
      carried *= gain_of(arc);
    }

    if (may_repeat) {
      // An edge used both ways would pass through an infeasible intermediate
      // state if each traversal were applied as it came, so the amounts are
      // accumulated and the net applied once per edge.
      for (const int edge : scratch_edges_) {
        const int forward = net_.forward_arc[static_cast<std::size_t>(edge)];
        const int backward = net_.twin[static_cast<std::size_t>(forward)];
        const double delta = arc_delta_[static_cast<std::size_t>(forward)] -
                             arc_delta_[static_cast<std::size_t>(backward)] / edge_gain(edge);
        arc_delta_[static_cast<std::size_t>(forward)] = 0.0;
        arc_delta_[static_cast<std::size_t>(backward)] = 0.0;
        set_edge_flow(edge, net_.flow[static_cast<std::size_t>(edge)] + delta);
      }
      // The walk was not a highest-gain path, so the potentials no longer hold.
      potentials_valid_ = false;
    }

    if (!options_.collect_paths) {
      augmentation.step_begin = 0;
      augmentation.step_count = 0;
    }
    augmentation.flow_out = carried;
    augmentation.gain = carried / limit.amount;
    augmentation.bottleneck_edge = edge_of(limit.arc);
    augmentation.bottleneck_forward = is_forward(limit.arc);
    augmentation.bottleneck_step = 0;
    for (std::size_t i = 0; i < walk.size(); ++i) {
      if (walk[i] == limit.arc) {
        augmentation.bottleneck_step = i;
        break;
      }
    }
    if (options_.collect_paths) result.augmentations.push_back(augmentation);
    return PushOutcome::Pushed;
  }

  PushOutcome cancel_and_route(const std::vector<int>& cycle, Result& result) {
    if (cycle.empty()) return PushOutcome::Blocked;
    const int origin = tail_of(cycle.front());
    route_buffer_.clear();
    // A cycle is only found when no feasible potential exists, so the true
    // highest-gain route would cost a full sweep. But the potentials are
    // broken only on some arcs -- after a Fat-Path phase, on the arcs that
    // phase was not allowed to use -- so the highest-gain route over the rest
    // is one early-exit Dijkstra. That matters: the excess usually belongs on
    // the remainder of the path the cycle rerouted, and an arbitrary route
    // creates new flow-generating cycles as fast as they are cancelled. Only
    // when no certified route exists does it fall back to the fewest hops --
    // and before any canonical labels exist, when the cycles are the input's
    // own and the potentials say nothing yet, the fewest hops measures faster.
    if (origin != problem_.sink &&
        !(routes_certified_ && find_route(origin, problem_.sink, route_buffer_)) &&
        !find_any_path(origin, problem_.sink, route_buffer_)) {
      return PushOutcome::Blocked;
    }
    walk_buffer_.assign(cycle.begin(), cycle.end());
    walk_buffer_.insert(walk_buffer_.end(), route_buffer_.begin(), route_buffer_.end());
    return push_walk(walk_buffer_, cycle.size(), result);
  }

  // ---- finishing -----------------------------------------------------------

  void finalize(Result& result, bool unbounded, bool hit_limit) {
    const std::size_t edges = problem_.edges.size();
    result.edge_flow.assign(edges, 0.0);
    for (std::size_t i = 0; i < edges; ++i) {
      double flow = net_.flow[i];
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
    for (std::size_t i = 0; i < edges; ++i) {
      const EdgeInput& edge = problem_.edges[i];
      const double flow = result.edge_flow[i];
      net[static_cast<std::size_t>(edge.from)] -= flow;
      net[static_cast<std::size_t>(edge.to)] += flow * edge.multiplier;
    }
    for (int node = 0; node < num_nodes_; ++node) {
      if (node == problem_.source || node == problem_.sink) continue;
      result.certificate.max_conservation_error =
          std::max(result.certificate.max_conservation_error,
                   std::fabs(net[static_cast<std::size_t>(node)]));
    }

    result.max_flow = net[static_cast<std::size_t>(problem_.sink)];
    result.source_outflow = -net[static_cast<std::size_t>(problem_.source)];

    if (unbounded) {
      result.status = "unbounded";
      result.optimal = false;
      result.max_flow = kInf;
      result.message =
          "the network admits unbounded flow: a path or flow-generating cycle reaching the sink "
          "has no finite capacity";
      return;
    }

    // Optimality certificate, recomputed from scratch over the final residual
    // network. A generalized flow is maximum exactly when there is no residual
    // augmenting path and no flow-generating cycle that can reach the sink.
    // These are deliberately the slow, authoritative checks rather than the
    // accelerated ones the main loop runs on. The loop may use heuristics --
    // seeded repairs, a parent-forest cycle scan -- and a certificate that
    // shared them could only confirm the loop's own blind spots. Both run once.
    potentials_valid_ = false;
    compute_reaches_sink();
    // An augmenting path exists exactly when the sink is still reachable from
    // the source through residual arcs, which is plain reachability.
    path_buffer_.clear();
    result.certificate.no_augmenting_path =
        !find_any_path(problem_.source, problem_.sink, path_buffer_);
    // Full Bellman-Ford rounds over the sink-reaching subgraph: either a
    // gain-amplifying cycle, or none exists.
    result.certificate.no_generating_cycle_reaching_sink = find_cycle_by_rounds().empty();

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
  long long blocked_arcs_ = 0;
  Stats* stats_ = nullptr;
  double discarded_seconds_ = 0.0;

  Layout net_;

  // Path-search state, stamped so nothing is cleared between searches.
  std::vector<NodeState> node_;
  MinHeap heap_;
  std::vector<int32_t> settled_;
  double stop_distance_ = 0.0;
  bool found_target_ = false;
  uint32_t search_stamp_ = 0;
  bool potentials_valid_ = false;

  // Label-correcting state, only allocated when a sweep runs.
  std::vector<double> label_;
  std::vector<char> in_queue_;
  std::vector<int32_t> visit_count_;
  std::vector<int32_t> queue_;
  std::vector<long long> stamp_at_;
  long long stamp_ = 0;
  int cycle_seed_ = -1;
  bool cycle_confirmed_ = false;
  std::vector<uint8_t> colour_;
  std::vector<uint32_t> colour_stamp_;
  std::vector<int32_t> walk_stack_;
  uint32_t colour_epoch_ = 0;

  std::vector<char> reaches_sink_;
  bool mask_is_full_ = false;
  std::vector<int32_t> repair_seeds_;
  bool potentials_initialized_ = false;
  bool force_full_potentials_ = false;

  // Walk scratch, reused across augmentations.
  std::vector<int> path_buffer_;
  std::vector<int> route_buffer_;
  std::vector<int> walk_buffer_;
  std::vector<int> cycle_buffer_;
  std::vector<int> cycle_path_;
  std::vector<int32_t> violations_;
  std::vector<double> coefficient_;
  std::vector<int> scratch_edges_;
  // Only a walk that can repeat an edge needs these, so they stay empty on a
  // network without flow-generating cycles.
  std::vector<double> arc_coefficient_;
  std::vector<double> arc_delta_;

  double capacity_scale_ = 1.0;
  double capacity_epsilon_ = 0.0;
  double log_epsilon_ = 1e-15;
  double potential_tolerance_ = 1e-9;

  // Fat-Path state. fat_ > 0 restricts the path search to fat arcs; slack_ > 0
  // is q = log b, the gain-rounding quantum. Both are zero outside the phases.
  double fat_ = 0.0;
  double slack_ = 0.0;
  double key_unit_ = 1.0;
  double skipped_fatness_ = 0.0;
  bool repair_all_next_ = false;
  // Set once canonical labels exist; from then on the potentials describe the
  // network well enough to route a cancelled cycle's excess by them.
  bool routes_certified_ = false;
};

}  // namespace

Result solve(const Problem& problem, const Options& options) {
  Solver solver(problem, options);
  return solver.run();
}

const char* version() { return "0.3.0"; }

}  // namespace gmf
