// JSON interface: a graph in, the filled paths and the maximum flow out.
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "gmf/gmf.hpp"
#include "gmf/json.hpp"

namespace gmf {
namespace {

using json::Value;

// Interns node labels in order of first appearance and remembers how each one
// was written, so the answer names nodes the way the request did.
class NodeTable {
 public:
  int intern(const Value& label) {
    const std::string key = key_for(label);
    const auto found = ids_.find(key);
    if (found != ids_.end()) return found->second;
    const int id = static_cast<int>(labels_.size());
    ids_.emplace(key, id);
    labels_.push_back(label.is_number() ? Value(label.as_number()) : Value(label.as_string()));
    return id;
  }

  int size() const { return static_cast<int>(labels_.size()); }
  const Value& label(int id) const { return labels_[static_cast<std::size_t>(id)]; }

  std::vector<std::string> names() const {
    std::vector<std::string> out;
    out.reserve(labels_.size());
    for (const Value& label : labels_) out.push_back(label.as_string());
    return out;
  }

 private:
  // Numbers and their string spelling name the same node, so "3" and 3 match.
  static std::string key_for(const Value& label) {
    return label.is_number() ? json::number_to_string(label.as_number()) : label.as_string();
  }

  std::map<std::string, int> ids_;
  std::vector<Value> labels_;
};

double read_capacity(const Value* value) {
  if (value == nullptr || value->is_null()) return std::numeric_limits<double>::infinity();
  return value->as_number(std::numeric_limits<double>::infinity());
}

// An edge may be written as an object or as a positional array. Within an
// adjacency list the tail is implied, so [to, capacity, multiplier] is enough;
// a free-standing edge needs [from, to, capacity, multiplier].
EdgeInput read_edge(const Value& value, NodeTable& nodes, const Value* implied_from) {
  EdgeInput edge;
  if (value.is_array()) {
    const json::Array& items = value.as_array();
    std::size_t cursor = 0;
    if (implied_from != nullptr) {
      edge.from = nodes.intern(*implied_from);
    } else {
      if (items.empty()) throw std::invalid_argument("edge array is missing its tail");
      edge.from = nodes.intern(items[cursor++]);
    }
    if (items.size() <= cursor) throw std::invalid_argument("edge array is missing its head");
    edge.to = nodes.intern(items[cursor++]);
    edge.capacity = items.size() > cursor ? read_capacity(&items[cursor++])
                                          : std::numeric_limits<double>::infinity();
    edge.multiplier = items.size() > cursor ? items[cursor++].as_number(1.0) : 1.0;
    return edge;
  }
  if (!value.is_object()) throw std::invalid_argument("an edge must be an object or an array");

  const Value* from = value.find_any({"from", "tail", "u"});
  if (from != nullptr) {
    edge.from = nodes.intern(*from);
  } else if (implied_from != nullptr) {
    edge.from = nodes.intern(*implied_from);
  } else {
    throw std::invalid_argument("an edge needs a from/tail/u field");
  }

  const Value* to = value.find_any({"to", "head", "v"});
  if (to == nullptr) throw std::invalid_argument("an edge needs a to/head/v field");
  edge.to = nodes.intern(*to);

  edge.capacity = read_capacity(value.find_any({"capacity", "cap"}));
  const Value* multiplier = value.find_any({"multiplier", "gain", "mult"});
  edge.multiplier = multiplier != nullptr ? multiplier->as_number(1.0) : 1.0;
  return edge;
}

Options read_options(const Value& request) {
  Options options;
  const Value* block = request.find("options");
  const Value& scope = block != nullptr ? *block : request;
  if (const Value* value = scope.find("epsilon")) options.epsilon = value->as_number(options.epsilon);
  if (const Value* value = scope.find_any({"max_iterations", "maxIterations"})) {
    options.max_iterations =
        static_cast<long long>(value->as_number(static_cast<double>(options.max_iterations)));
  }
  if (const Value* value = scope.find_any({"collect_paths", "collectPaths"})) {
    options.collect_paths = value->as_bool(options.collect_paths);
  }
  if (const Value* value =
          scope.find_any({"cancel_generating_cycles", "cancelGeneratingCycles"})) {
    options.cancel_generating_cycles = value->as_bool(options.cancel_generating_cycles);
  }
  if (const Value* value = scope.find_any({"use_potentials", "usePotentials"})) {
    options.use_potentials = value->as_bool(options.use_potentials);
  }
  return options;
}

Problem read_problem(const Value& request, NodeTable& nodes) {
  if (!request.is_object()) throw std::invalid_argument("the request must be a JSON object");

  // Declaring the nodes first fixes their order; otherwise ids follow the order
  // the edges mention them in.
  if (const Value* declared = request.find("nodes")) {
    for (const Value& label : declared->as_array()) nodes.intern(label);
  }

  const Value* source = request.find_any({"source", "src", "start"});
  const Value* sink = request.find_any({"sink", "dst", "target", "end"});
  if (source == nullptr) throw std::invalid_argument("the request needs a source field");
  if (sink == nullptr) throw std::invalid_argument("the request needs a sink field");

  Problem problem;
  problem.source = nodes.intern(*source);
  problem.sink = nodes.intern(*sink);

  bool found_edges = false;
  if (const Value* adjacency = request.find_any({"adjacency", "graph", "adj"})) {
    found_edges = true;
    if (adjacency->is_object()) {
      for (const auto& entry : adjacency->as_object()) {
        const Value from(entry.first);
        for (const Value& edge : entry.second.as_array()) {
          problem.edges.push_back(read_edge(edge, nodes, &from));
        }
      }
    } else if (adjacency->is_array()) {
      // Positional adjacency: index i holds the arcs leaving node i.
      const json::Array& rows = adjacency->as_array();
      for (std::size_t i = 0; i < rows.size(); ++i) {
        const Value from(static_cast<double>(i));
        for (const Value& edge : rows[i].as_array()) {
          problem.edges.push_back(read_edge(edge, nodes, &from));
        }
      }
    } else {
      throw std::invalid_argument("adjacency must be an object or an array");
    }
  }
  if (const Value* edges = request.find_any({"edges", "arcs", "links"})) {
    found_edges = true;
    for (const Value& edge : edges->as_array()) {
      problem.edges.push_back(read_edge(edge, nodes, nullptr));
    }
  }
  if (!found_edges) throw std::invalid_argument("the request needs an adjacency or edges field");

  problem.num_nodes = nodes.size();
  problem.node_names = nodes.names();
  return problem;
}

// The step stores only what cannot be derived; the endpoints, the multiplier
// and the delivered amount are recovered from the problem here.
Value step_to_json(const Problem& problem, const Step& step, const NodeTable& nodes) {
  Value out = Value::object();
  out.set("edge", Value(step.edge));
  out.set("from", nodes.label(step_from(problem, step)));
  out.set("to", nodes.label(step_to(problem, step)));
  out.set("direction", Value(step.forward ? "forward" : "backward"));
  out.set("multiplier", Value(step_multiplier(problem, step)));
  out.set("flow_in", Value(step.flow_in));
  out.set("flow_out", Value(step_flow_out(problem, step)));
  return out;
}

Value augmentation_to_json(const Problem& problem, const Result& result,
                           const Augmentation& augmentation, const NodeTable& nodes) {
  Value out = Value::object();
  out.set("kind", Value(augmentation.kind == Augmentation::Kind::Path
                            ? "path"
                            : "flow_generating_cycle"));
  out.set("flow_in", Value(augmentation.flow_in));
  out.set("flow_out", Value(augmentation.flow_out));
  out.set("gain", Value(augmentation.gain));
  if (augmentation.kind == Augmentation::Kind::GeneratingCycle) {
    out.set("excess", Value(augmentation.excess));
    out.set("cycle_length", Value(augmentation.cycle_length));
  }

  const Step* steps = result.steps.data() + augmentation.step_begin;
  const std::size_t count = augmentation.step_count;

  // The node sequence follows from the steps, so it is not stored.
  Value node_list = Value::array();
  if (count > 0) {
    node_list.push_back(nodes.label(step_from(problem, steps[0])));
    for (std::size_t i = 0; i < count; ++i) {
      node_list.push_back(nodes.label(step_to(problem, steps[i])));
    }
  }
  out.set("nodes", std::move(node_list));

  Value step_list = Value::array();
  for (std::size_t i = 0; i < count; ++i) {
    step_list.push_back(step_to_json(problem, steps[i], nodes));
  }
  out.set("steps", std::move(step_list));

  if (augmentation.bottleneck_step < count) {
    const Step& step = steps[augmentation.bottleneck_step];
    Value bottleneck = Value::object();
    bottleneck.set("edge", Value(augmentation.bottleneck_edge));
    bottleneck.set("from", nodes.label(step_from(problem, step)));
    bottleneck.set("to", nodes.label(step_to(problem, step)));
    bottleneck.set("direction", Value(augmentation.bottleneck_forward ? "forward" : "backward"));
    bottleneck.set("step", Value(augmentation.bottleneck_step));
    out.set("bottleneck", std::move(bottleneck));
  }
  return out;
}

Value result_to_json(const Problem& problem, const Result& result, const NodeTable& nodes) {
  Value out = Value::object();
  out.set("status", Value(result.status));
  out.set("optimal", Value(result.optimal));
  if (!result.message.empty()) out.set("message", Value(result.message));
  out.set("max_flow", Value(result.max_flow));
  out.set("source_outflow", Value(result.source_outflow));
  out.set("source", nodes.label(problem.source));
  out.set("sink", nodes.label(problem.sink));
  out.set("num_nodes", Value(problem.num_nodes));

  Value paths = Value::array();
  for (const Augmentation& augmentation : result.augmentations) {
    paths.push_back(augmentation_to_json(problem, result, augmentation, nodes));
  }
  out.set("paths", std::move(paths));

  Value edges = Value::array();
  for (std::size_t i = 0; i < problem.edges.size(); ++i) {
    const EdgeInput& edge = problem.edges[i];
    const double flow = i < result.edge_flow.size() ? result.edge_flow[i] : 0.0;
    Value entry = Value::object();
    entry.set("index", Value(i));
    entry.set("from", nodes.label(edge.from));
    entry.set("to", nodes.label(edge.to));
    entry.set("capacity", Value(edge.capacity));
    entry.set("multiplier", Value(edge.multiplier));
    entry.set("flow", Value(flow));
    entry.set("delivered", Value(flow * edge.multiplier));
    edges.push_back(std::move(entry));
  }
  out.set("edges", std::move(edges));

  Value certificate = Value::object();
  certificate.set("no_augmenting_path", Value(result.certificate.no_augmenting_path));
  certificate.set("no_generating_cycle_reaching_sink",
                  Value(result.certificate.no_generating_cycle_reaching_sink));
  certificate.set("max_conservation_error", Value(result.certificate.max_conservation_error));
  certificate.set("max_capacity_violation", Value(result.certificate.max_capacity_violation));
  out.set("certificate", std::move(certificate));

  Value stats = Value::object();
  stats.set("iterations", Value(static_cast<double>(result.stats.iterations)));
  stats.set("path_searches", Value(static_cast<double>(result.stats.path_searches)));
  stats.set("cycle_searches", Value(static_cast<double>(result.stats.cycle_searches)));
  stats.set("relaxations", Value(static_cast<double>(result.stats.relaxations)));
  stats.set("dijkstra_searches", Value(static_cast<double>(result.stats.dijkstra_searches)));
  stats.set("label_correcting_searches",
            Value(static_cast<double>(result.stats.label_correcting_searches)));
  stats.set("heap_pops", Value(static_cast<double>(result.stats.heap_pops)));
  stats.set("seconds", Value(result.stats.seconds));
  stats.set("path_search_seconds", Value(result.stats.path_search_seconds));
  stats.set("cycle_search_seconds", Value(result.stats.cycle_search_seconds));
  stats.set("push_seconds", Value(result.stats.push_seconds));
  stats.set("potential_repairs", Value(static_cast<double>(result.stats.potential_repairs)));
  stats.set("working_bytes", Value(static_cast<double>(result.stats.working_bytes)));
  stats.set("blocked_arcs", Value(static_cast<double>(result.stats.blocked_arcs)));
  stats.set("cycle_extract_failures",
            Value(static_cast<double>(result.stats.cycle_extract_failures)));
  out.set("stats", std::move(stats));

  out.set("version", Value(version()));
  return out;
}

Value error_to_json(const std::string& message) {
  Value out = Value::object();
  out.set("status", Value("invalid_input"));
  out.set("optimal", Value(false));
  out.set("error", Value(message));
  out.set("version", Value(version()));
  return out;
}

int read_indent(const Value& request) {
  const Value* block = request.find("options");
  const Value& scope = block != nullptr ? *block : request;
  if (const Value* pretty = scope.find("pretty")) {
    if (pretty->is_number()) return static_cast<int>(pretty->as_number(2));
    return pretty->as_bool(false) ? 2 : -1;
  }
  return -1;
}

}  // namespace

std::string solve_json(const std::string& request_text) {
  int indent = -1;
  try {
    const Value request = json::parse(request_text);
    indent = read_indent(request);
    NodeTable nodes;
    const Problem problem = read_problem(request, nodes);
    const Options options = read_options(request);
    const Result result = solve(problem, options);
    return json::dump(result_to_json(problem, result, nodes), indent);
  } catch (const std::exception& error) {
    return json::dump(error_to_json(error.what()), indent);
  }
}

}  // namespace gmf
