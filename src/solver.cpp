#include "gmf/gmf.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
  // Per edge.
  std::vector<int32_t> forward_arc;
  std::vector<double> gain;
  std::vector<double> capacity;
  std::vector<double> flow;
  // One bit per arc; an arc is retired only when nothing can move through it.
  std::vector<uint64_t> blocked;

  std::size_t bytes() const {
    return arc_begin.capacity() * sizeof(int32_t) + head.capacity() * sizeof(int32_t) +
           residual.capacity() * sizeof(double) + cost.capacity() * sizeof(double) +
           twin.capacity() * sizeof(int32_t) + code.capacity() * sizeof(int32_t) +
           forward_arc.capacity() * sizeof(int32_t) + gain.capacity() * sizeof(double) +
           capacity.capacity() * sizeof(double) + flow.capacity() * sizeof(double) +
           blocked.capacity() * sizeof(uint64_t);
  }
};

struct HeapEntry {
  double key;
  int32_t node;
};

struct HeapGreater {
  bool operator()(const HeapEntry& lhs, const HeapEntry& rhs) const { return lhs.key > rhs.key; }
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

    while (true) {
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
    net_.gain.resize(edges);
    net_.capacity.resize(edges);
    net_.flow.assign(edges, 0.0);
    net_.blocked.assign((arcs + 63) / 64, 0);

    std::vector<int32_t> cursor(net_.arc_begin.begin(), net_.arc_begin.end() - 1);
    for (std::size_t i = 0; i < edges; ++i) {
      const EdgeInput& edge = problem_.edges[i];
      const double log_gain = std::log(edge.multiplier);
      net_.gain[i] = edge.multiplier;
      net_.capacity[i] = edge.capacity;

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
    distance_.assign(nodes, 0.0);
    distance_stamp_.assign(nodes, 0);
    settled_stamp_.assign(nodes, 0);
    parent_.assign(nodes, -1);
    potential_.assign(nodes, 0.0);
    reaches_sink_.assign(nodes, 0);

    thread_count_ = 1;
#if GMF_ENABLE_THREADS
    thread_count_ = options_.threads;
    if (thread_count_ == 0) {
      thread_count_ = static_cast<int>(std::thread::hardware_concurrency());
      if (thread_count_ <= 0) thread_count_ = 1;
    }
    thread_count_ = std::max(1, thread_count_);
#endif
  }

  std::size_t working_bytes() const {
    std::size_t bytes = net_.bytes();
    bytes += distance_.capacity() * sizeof(double);
    bytes += distance_stamp_.capacity() * sizeof(uint32_t);
    bytes += settled_stamp_.capacity() * sizeof(uint32_t);
    bytes += parent_.capacity() * sizeof(int32_t);
    bytes += potential_.capacity() * sizeof(double);
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
    return bytes;
  }

  // ---- arc accessors -------------------------------------------------------

  int edge_of(int arc) const { return net_.code[static_cast<std::size_t>(arc)] >> 1; }
  bool is_forward(int arc) const {
    return (net_.code[static_cast<std::size_t>(arc)] & 1) == 0;
  }
  int twin_of(int arc) const { return net_.twin[static_cast<std::size_t>(arc)]; }
  int tail_of(int arc) const { return net_.head[static_cast<std::size_t>(twin_of(arc))]; }
  double gain_of(int arc) const {
    const double gain = net_.gain[static_cast<std::size_t>(edge_of(arc))];
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
    const double capacity = net_.capacity[index];
    if (flow < 0.0 && flow > -capacity_epsilon_) flow = 0.0;
    if (flow > capacity && flow < capacity + capacity_epsilon_) flow = capacity;
    net_.flow[index] = flow;
    const int32_t forward = net_.forward_arc[index];
    net_.residual[static_cast<std::size_t>(forward)] =
        std::isfinite(capacity) ? capacity - flow : capacity;
    net_.residual[static_cast<std::size_t>(net_.twin[static_cast<std::size_t>(forward)])] =
        flow * net_.gain[index];
  }

  // ---- reachability --------------------------------------------------------

  // Nodes that can still reach the sink. Every augmenting path and every cycle
  // worth cancelling lies inside this set, and confining the searches to it is
  // what keeps a gain-amplifying cycle in a dead end from making the labels
  // diverge. The set can only shrink as arcs saturate: a push creates residual
  // arcs only between nodes that are already in it, so no node outside gains a
  // way in. It is therefore recomputed only alongside the potentials.
  void compute_reaches_sink() {
    std::fill(reaches_sink_.begin(), reaches_sink_.end(), static_cast<char>(0));
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
  }

  bool in_mask(int node) const { return reaches_sink_[static_cast<std::size_t>(node)] != 0; }

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
      for (int node = 0; node < num_nodes_; ++node) {
        if (!in_mask(node)) continue;
        label_[static_cast<std::size_t>(node)] = 0.0;
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

#if GMF_ENABLE_THREADS
    if (thread_count_ > 1 && !sweep_parallel(stats)) {
      ++stats.parallel_fallbacks;
      if (!sweep_sequential(stats)) return resolve_sweep_failure();
      potentials_from_labels();
      return true;
    }
    if (thread_count_ > 1) {
      potentials_from_labels();
      return true;
    }
#endif
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
  bool potentials_are_feasible() const {
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
            label_[static_cast<std::size_t>(next)] + 1e-9) {
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
      if (label != kNegInf) potential_[static_cast<std::size_t>(node)] = -label;
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
      if (parent_[static_cast<std::size_t>(start)] < 0) continue;
      if (colour_stamp_[static_cast<std::size_t>(start)] == colour_epoch_) continue;
      walk_stack_.clear();
      int node = start;
      while (node >= 0 && colour_stamp_[static_cast<std::size_t>(node)] != colour_epoch_) {
        colour_stamp_[static_cast<std::size_t>(node)] = colour_epoch_;
        colour_[static_cast<std::size_t>(node)] = 1;  // on the current chain
        walk_stack_.push_back(node);
        const int arc = parent_[static_cast<std::size_t>(node)];
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
    std::size_t cursor = 0;
    long long relaxations = 0;
    long long since_check = 0;
    long long check_interval = std::max<long long>(32, num_nodes_);
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
        if (candidate > label_[static_cast<std::size_t>(next)] + log_epsilon_) {
          label_[static_cast<std::size_t>(next)] = candidate;
          parent_[static_cast<std::size_t>(next)] = a;
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

#if GMF_ENABLE_THREADS
  // The same sweep, fanned out across threads: each round expands the whole
  // current frontier at once. Only the potential-establishing sweeps go through
  // here, so on a network without amplifying cycles this runs once.
  bool sweep_parallel(Stats& stats) {
    const std::size_t nodes = static_cast<std::size_t>(num_nodes_);
    if (atomic_label_ == nullptr || atomic_capacity_ < nodes) {
      atomic_label_ = std::make_unique<std::atomic<double>[]>(nodes);
      atomic_parent_ = std::make_unique<std::atomic<int32_t>[]>(nodes);
      queued_ = std::make_unique<std::atomic<bool>[]>(nodes);
      atomic_capacity_ = nodes;
    }
    for (std::size_t i = 0; i < nodes; ++i) {
      atomic_label_[i].store(label_[i], std::memory_order_relaxed);
      atomic_parent_[i].store(-1, std::memory_order_relaxed);
      queued_[i].store(false, std::memory_order_relaxed);
    }

    std::vector<int32_t> frontier(queue_.begin(), queue_.end());
    std::vector<std::vector<int32_t>> next(static_cast<std::size_t>(thread_count_));
    std::atomic<long long> relaxations{0};
    int rounds = 0;

    while (!frontier.empty()) {
      if (++rounds > num_nodes_ + 1) return false;  // gain-amplifying cycle
      for (std::vector<int32_t>& bucket : next) bucket.clear();

      const auto worker = [&](int slot) {
        std::vector<int32_t>& local = next[static_cast<std::size_t>(slot)];
        long long local_relaxations = 0;
        for (std::size_t i = static_cast<std::size_t>(slot); i < frontier.size();
             i += static_cast<std::size_t>(thread_count_)) {
          const int node = frontier[i];
          const double base =
              atomic_label_[static_cast<std::size_t>(node)].load(std::memory_order_relaxed);
          if (base == kNegInf) continue;
          const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
          for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
            if (net_.residual[static_cast<std::size_t>(a)] <= capacity_epsilon_ || blocked(a)) {
              continue;
            }
            const int next_node = net_.head[static_cast<std::size_t>(a)];
            if (!in_mask(next_node)) continue;
            const double candidate = base - net_.cost[static_cast<std::size_t>(a)];
            std::atomic<double>& slot_label = atomic_label_[static_cast<std::size_t>(next_node)];
            double current = slot_label.load(std::memory_order_relaxed);
            while (candidate > current + log_epsilon_) {
              if (slot_label.compare_exchange_weak(current, candidate,
                                                   std::memory_order_relaxed)) {
                atomic_parent_[static_cast<std::size_t>(next_node)].store(
                    a, std::memory_order_relaxed);
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
      for (const std::vector<int32_t>& bucket : next) {
        for (const int32_t node : bucket) {
          queued_[static_cast<std::size_t>(node)].store(false, std::memory_order_relaxed);
          frontier.push_back(node);
        }
      }
    }

    stats.relaxations += relaxations.load();
    for (std::size_t i = 0; i < nodes; ++i) {
      label_[i] = atomic_label_[i].load(std::memory_order_relaxed);
      parent_[i] = atomic_parent_[i].load(std::memory_order_relaxed);
    }
    return true;
  }
#endif

  // Full Bellman-Ford rounds, used only to extract a cycle the queue-based
  // sweep detected but whose parent pointers had already moved on.
  std::vector<int> find_cycle_by_rounds() {
    const std::size_t nodes = static_cast<std::size_t>(num_nodes_);
    label_.assign(nodes, kNegInf);
    std::fill(parent_.begin(), parent_.end(), -1);
    for (int node = 0; node < num_nodes_; ++node) {
      if (in_mask(node)) label_[static_cast<std::size_t>(node)] = 0.0;
    }

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
          if (candidate > label_[static_cast<std::size_t>(next)] + log_epsilon_) {
            label_[static_cast<std::size_t>(next)] = candidate;
            parent_[static_cast<std::size_t>(next)] = a;
            changed = true;
          }
        }
      }
    }
    if (!changed) return {};

    for (int node = 0; node < num_nodes_; ++node) {
      if (!in_mask(node)) continue;
      const double base = label_[static_cast<std::size_t>(node)];
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
        if (!usable(a)) continue;
        const int next = net_.head[static_cast<std::size_t>(a)];
        if (!in_mask(next)) continue;
        if (base - net_.cost[static_cast<std::size_t>(a)] <=
            label_[static_cast<std::size_t>(next)] + log_epsilon_) {
          continue;
        }
        const std::vector<int> cycle = extract_cycle(next);
        if (!cycle.empty()) return cycle;
        if (stats_ != nullptr) ++stats_->cycle_extract_failures;
      }
    }
    return {};
  }

  // Follows parent pointers from `start` until a node repeats, which closes a
  // cycle, and returns it in forward order.
  std::vector<int> extract_cycle(int start) {
    if (stamp_at_.size() != static_cast<std::size_t>(num_nodes_)) {
      stamp_at_.assign(static_cast<std::size_t>(num_nodes_), 0);
    }
    ++stamp_;
    int node = start;
    for (int step = 0; step <= num_nodes_ + 1; ++step) {
      if (stamp_at_[static_cast<std::size_t>(node)] == stamp_) break;
      stamp_at_[static_cast<std::size_t>(node)] = stamp_;
      const int arc = parent_[static_cast<std::size_t>(node)];
      if (arc < 0) return {};  // This chain leaves the parent graph; try another.
      node = tail_of(arc);
    }
    if (stamp_at_[static_cast<std::size_t>(node)] != stamp_) return {};

    std::vector<int> cycle;
    const int anchor = node;
    int walker = anchor;
    do {
      const int arc = parent_[static_cast<std::size_t>(walker)];
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
    if (!(total > log_epsilon_)) return {};  // Not actually gain-amplifying.
    return cycle;
  }

  // ---- path search ---------------------------------------------------------

  double distance_of(int node) const {
    return distance_stamp_[static_cast<std::size_t>(node)] == search_stamp_
               ? distance_[static_cast<std::size_t>(node)]
               : kInf;
  }

  void set_distance(int node, double value) {
    distance_[static_cast<std::size_t>(node)] = value;
    distance_stamp_[static_cast<std::size_t>(node)] = search_stamp_;
  }

  // The highest-gain residual path, as an early-exit Dijkstra over reduced
  // costs. This is where the time goes on a large network, and it is the reason
  // for the potentials: a label-correcting sweep per augmentation costs a pass
  // over every arc, while this settles only the nodes nearer than the sink and
  // stops.
  //
  // Returns false when a reduced cost came out negative, meaning the potentials
  // have drifted or were invalidated; the caller rebuilds them.
  bool dijkstra_path(int source, int target, std::vector<int>& path, Stats& stats) {
    ++search_stamp_;
    ++stats.dijkstra_searches;
    heap_.clear();
    settled_.clear();
    set_distance(source, 0.0);
    parent_[static_cast<std::size_t>(source)] = -1;
    heap_.push_back({0.0, static_cast<int32_t>(source)});

    double stop = 0.0;
    bool found = false;
    long long relaxations = 0;

    while (!heap_.empty()) {
      std::pop_heap(heap_.begin(), heap_.end(), HeapGreater());
      const HeapEntry top = heap_.back();
      heap_.pop_back();
      ++stats.heap_pops;
      if (settled_stamp_[static_cast<std::size_t>(top.node)] == search_stamp_) continue;
      settled_stamp_[static_cast<std::size_t>(top.node)] = search_stamp_;
      settled_.push_back(top.node);
      stop = top.key;
      if (top.node == target) {
        found = true;
        break;
      }

      const double base = top.key;
      const double base_potential = potential_[static_cast<std::size_t>(top.node)];
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(top.node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(top.node)]; a < end; ++a) {
        if (net_.residual[static_cast<std::size_t>(a)] <= capacity_epsilon_ || blocked(a)) continue;
        const int32_t next = net_.head[static_cast<std::size_t>(a)];
        if (!in_mask(next)) continue;
        if (settled_stamp_[static_cast<std::size_t>(next)] == search_stamp_) continue;
        double reduced = net_.cost[static_cast<std::size_t>(a)] + base_potential -
                         potential_[static_cast<std::size_t>(next)];
        if (reduced < -potential_tolerance_) {
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
        if (reduced < 0.0) reduced = 0.0;  // Harmless rounding; keep it monotone.
        const double candidate = base + reduced;
        if (candidate < distance_of(next)) {
          set_distance(next, candidate);
          parent_[static_cast<std::size_t>(next)] = a;
          ++relaxations;
          heap_.push_back({candidate, next});
          std::push_heap(heap_.begin(), heap_.end(), HeapGreater());
        }
      }
    }
    stats.relaxations += relaxations;

    // Re-tighten the potentials over the settled nodes. Shifting every
    // potential by a constant changes no reduced cost, so subtracting the stop
    // distance lets the untouched nodes keep their value and leaves the update
    // proportional to the part of the graph the search actually visited.
    for (const int32_t node : settled_) {
      potential_[static_cast<std::size_t>(node)] +=
          distance_[static_cast<std::size_t>(node)] - stop;
    }

    path.clear();
    if (!found) return true;  // No path; that is an answer, not a failure.
    return build_path(source, target, path);
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
      const int arc = parent_[static_cast<std::size_t>(node)];
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
    std::fill(parent_.begin(), parent_.end(), -1);
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
      if (dijkstra_path(source, target, path, stats)) return true;
      return false;  // Potentials invalidated; the caller rebuilds them.
    }
    return label_correcting_path(source, target, path, stats);
  }

  // Any residual path, used to carry a cycle's excess to the sink when the
  // highest-gain search cannot produce one.
  bool find_any_path(int source, int target, std::vector<int>& path) {
    std::fill(parent_.begin(), parent_.end(), -1);
    ++search_stamp_;
    queue_.clear();
    queue_.push_back(source);
    settled_stamp_[static_cast<std::size_t>(source)] = search_stamp_;
    for (std::size_t cursor = 0; cursor < queue_.size(); ++cursor) {
      const int node = queue_[cursor];
      if (node == target) return build_path(source, target, path);
      const int32_t end = net_.arc_begin[static_cast<std::size_t>(node) + 1];
      for (int32_t a = net_.arc_begin[static_cast<std::size_t>(node)]; a < end; ++a) {
        if (!usable(a)) continue;
        const int next = net_.head[static_cast<std::size_t>(a)];
        if (!in_mask(next) || settled_stamp_[static_cast<std::size_t>(next)] == search_stamp_) {
          continue;
        }
        settled_stamp_[static_cast<std::size_t>(next)] = search_stamp_;
        parent_[static_cast<std::size_t>(next)] = a;
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
      const double gain = net_.gain[static_cast<std::size_t>(edge)];
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
                             arc_delta_[static_cast<std::size_t>(backward)] /
                                 net_.gain[static_cast<std::size_t>(edge)];
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
    // A cycle is only found when no feasible potential exists, so the
    // highest-gain route would cost a full sweep. The route only has to carry
    // the excess: taking the shortest one costs a single breadth-first pass,
    // and any routing it gets wrong is what the reverse residual arcs of the
    // following path augmentations are for.
    if (origin != problem_.sink && !find_any_path(origin, problem_.sink, route_buffer_)) {
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
  int thread_count_ = 1;
  long long blocked_arcs_ = 0;
  Stats* stats_ = nullptr;

  Layout net_;

  // Dijkstra state, stamped so nothing is cleared between searches.
  std::vector<double> distance_;
  std::vector<uint32_t> distance_stamp_;
  std::vector<uint32_t> settled_stamp_;
  std::vector<int32_t> parent_;
  std::vector<double> potential_;
  std::vector<HeapEntry> heap_;
  std::vector<int32_t> settled_;
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
  std::vector<int32_t> repair_seeds_;
  bool potentials_initialized_ = false;
  bool force_full_potentials_ = false;

  // Walk scratch, reused across augmentations.
  std::vector<int> path_buffer_;
  std::vector<int> route_buffer_;
  std::vector<int> walk_buffer_;
  std::vector<int> cycle_buffer_;
  std::vector<double> coefficient_;
  std::vector<int> scratch_edges_;
  // Only a walk that can repeat an edge needs these, so they stay empty on a
  // network without flow-generating cycles.
  std::vector<double> arc_coefficient_;
  std::vector<double> arc_delta_;

#if GMF_ENABLE_THREADS
  std::unique_ptr<std::atomic<double>[]> atomic_label_;
  std::unique_ptr<std::atomic<int32_t>[]> atomic_parent_;
  std::unique_ptr<std::atomic<bool>[]> queued_;
  std::size_t atomic_capacity_ = 0;
#endif

  double capacity_scale_ = 1.0;
  double capacity_epsilon_ = 0.0;
  double log_epsilon_ = 1e-15;
  double potential_tolerance_ = 1e-9;
};

}  // namespace

Result solve(const Problem& problem, const Options& options) {
  Solver solver(problem, options);
  return solver.run();
}

const char* version() { return "0.2.0"; }

}  // namespace gmf
