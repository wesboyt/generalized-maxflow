// The plain C ABI: the JSON interface, plus an array interface for callers
// that already hold the graph in memory, with no C++ or Python types at the
// boundary. The Python package loads it with ctypes and the
// WebAssembly build exports it, so neither links against an interpreter -- one
// wheel per OS and CPU serves every Python version.
//
// Strings cross the boundary as UTF-8. The caller owns what gmf_solve_json
// returns and frees it with gmf_free; a null return means memory ran out.
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include "gmf/gmf.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define GMF_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(_WIN32)
#define GMF_EXPORT __declspec(dllexport)
#else
#define GMF_EXPORT __attribute__((visibility("default")))
#endif

namespace {

char* duplicate(const std::string& text, std::size_t* size) {
  char* buffer = static_cast<char*>(std::malloc(text.size() + 1));
  if (buffer == nullptr) return nullptr;
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  if (size != nullptr) *size = text.size();
  return buffer;
}

char* solve(const std::string& request, std::size_t* size) {
  try {
    return duplicate(gmf::solve_json(request), size);
  } catch (const std::bad_alloc&) {
    return nullptr;
  } catch (...) {
    return duplicate("{\"status\":\"invalid_input\",\"error\":\"unknown error\"}", size);
  }
}

}  // namespace

extern "C" {

// A NUL-terminated request.
GMF_EXPORT char* gmf_solve_json(const char* request) {
  if (request == nullptr) {
    return duplicate("{\"status\":\"invalid_input\",\"error\":\"no request\"}", nullptr);
  }
  return solve(std::string(request), nullptr);
}

// A request of known length, which may hold NUL bytes; the answer's length is
// written to *answer_size when it is not null.
GMF_EXPORT char* gmf_solve_json_n(const char* request, std::size_t request_size,
                                  std::size_t* answer_size) {
  if (request == nullptr && request_size > 0) {
    return duplicate("{\"status\":\"invalid_input\",\"error\":\"no request\"}", answer_size);
  }
  return solve(std::string(request == nullptr ? "" : request, request_size), answer_size);
}

// Arrays in, numbers out: no JSON on either side, for callers that solve many
// graphs from memory they already hold (numpy arrays, typed arrays in a
// WebAssembly heap). Edge e runs from[e] -> to[e] with capacity[e] and
// multiplier[e]; a capacity < 0 (or +inf) means unbounded. Paths are not
// recorded.
//
// *max_flow, when not null, receives the flow delivered at the sink;
// edge_flow, when not null, must hold num_edges doubles and receives the units
// entering each edge (delivered = edge_flow[e] * multiplier[e]).
//
// Returns 0 when the answer is certified optimal, 1 for any other solver status
// (the outputs are still written), 2 for a malformed problem (bad node id,
// negative multiplier, ...), 3 when memory ran out.
GMF_EXPORT int gmf_solve_arrays_flows(int num_nodes, int source, int sink, int num_edges,
                                      const int* from, const int* to, const double* capacity,
                                      const double* multiplier, double* max_flow,
                                      double* edge_flow) {
  try {
    if (num_edges < 0 || (num_edges > 0 && (from == nullptr || to == nullptr ||
                                            capacity == nullptr || multiplier == nullptr))) {
      return 2;
    }
    gmf::Problem problem;
    problem.num_nodes = num_nodes;
    problem.source = source;
    problem.sink = sink;
    problem.edges.resize(static_cast<std::size_t>(num_edges));
    for (int e = 0; e < num_edges; ++e) {
      gmf::EdgeInput& edge = problem.edges[static_cast<std::size_t>(e)];
      edge.from = from[e];
      edge.to = to[e];
      edge.capacity = capacity[e] < 0 ? std::numeric_limits<double>::infinity() : capacity[e];
      edge.multiplier = multiplier[e];
    }
    gmf::Options options;
    options.collect_paths = false;
    const gmf::Result result = gmf::solve(problem, options);
    if (max_flow != nullptr) *max_flow = result.max_flow;
    if (edge_flow != nullptr) {
      for (int e = 0; e < num_edges; ++e) {
        const std::size_t i = static_cast<std::size_t>(e);
        edge_flow[e] = i < result.edge_flow.size() ? result.edge_flow[i] : 0.0;
      }
    }
    return result.optimal ? 0 : 1;
  } catch (const std::bad_alloc&) {
    return 3;
  } catch (...) {
    return 2;
  }
}

// The same without per-edge flows.
GMF_EXPORT int gmf_solve_arrays(int num_nodes, int source, int sink, int num_edges,
                                const int* from, const int* to, const double* capacity,
                                const double* multiplier, double* max_flow) {
  return gmf_solve_arrays_flows(num_nodes, source, sink, num_edges, from, to, capacity,
                                multiplier, max_flow, nullptr);
}

GMF_EXPORT void gmf_free(char* buffer) { std::free(buffer); }

GMF_EXPORT const char* gmf_version() { return gmf::version(); }

}  // extern "C"
