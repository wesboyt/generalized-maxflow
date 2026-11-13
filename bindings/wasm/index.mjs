// Generalized maximum flow, compiled to WebAssembly.
//
//   import createSolver from "generalized-maxflow";
//   const solver = await createSolver();
//   console.log(solver.solve(graph).max_flow);
//
// The graph format is documented in the project README. Strings cross into
// WebAssembly as UTF-8; the buffer the solver returns is freed here.
import createGmaxflow from "./gmaxflow.js";

/**
 * Loads the WebAssembly module and returns a solver bound to it.
 * @param {object} [moduleOptions] passed through to the Emscripten runtime
 */
export default async function createSolver(moduleOptions) {
  const runtime = await createGmaxflow(moduleOptions);

  const solveJson = runtime.cwrap("gmf_solve_json", "number", ["string"]);
  const freeBuffer = runtime.cwrap("gmf_free", null, ["number"]);
  const readVersion = runtime.cwrap("gmf_version", "string", []);
  const solveArraysRaw = runtime.cwrap("gmf_solve_arrays_flows", "number", [
    "number", "number", "number", "number",
    "number", "number", "number", "number", "number", "number",
  ]);

  /**
   * Solves a problem given as JSON text and returns the answer as JSON text.
   * @param {string} request
   * @returns {string}
   */
  function solveText(request) {
    if (typeof request !== "string") {
      throw new TypeError("solveText expects JSON text");
    }
    const pointer = solveJson(request);
    if (!pointer) throw new Error("the solver could not allocate its answer");
    try {
      return runtime.UTF8ToString(pointer);
    } finally {
      freeBuffer(pointer);
    }
  }

  /**
   * Solves a problem given as an object (or JSON text) and returns the answer
   * as an object. Throws when the graph is rejected.
   * @param {object|string} graph
   */
  function solve(graph) {
    const request = typeof graph === "string" ? graph : JSON.stringify(graph);
    const answer = JSON.parse(solveText(request));
    if (answer.status === "invalid_input") {
      throw new Error(`generalized-maxflow: ${answer.error}`);
    }
    return answer;
  }

  /**
   * The maximum flow delivered at the sink. Infinity when unbounded.
   * @param {object|string} graph
   * @returns {number}
   */
  function maxFlow(graph) {
    return solve(graph).max_flow;
  }

  /**
   * Solves a graph held as parallel typed arrays, copied straight into the
   * WebAssembly heap: no JSON on either side. Edge e runs from[e] -> to[e];
   * a capacity < 0 or Infinity is unbounded. Returns { status, optimal,
   * maxFlow, edgeFlow } where edgeFlow (a Float64Array, only when
   * options.edgeFlow is true) holds the units entering each edge.
   * status: 0 optimal, 1 not certified, 2 malformed graph, 3 out of memory.
   * @param {number} numNodes
   * @param {number} source
   * @param {number} sink
   * @param {ArrayLike<number>} from
   * @param {ArrayLike<number>} to
   * @param {ArrayLike<number>} capacity
   * @param {ArrayLike<number>} multiplier
   * @param {{edgeFlow?: boolean}} [options]
   */
  function solveArrays(numNodes, source, sink, from, to, capacity, multiplier, options = {}) {
    const m = from.length;
    if (to.length !== m || capacity.length !== m || multiplier.length !== m) {
      throw new RangeError("from, to, capacity and multiplier must have the same length");
    }
    const n = Math.max(m, 1);
    const fromPtr = runtime._malloc(4 * n);
    const toPtr = runtime._malloc(4 * n);
    const capPtr = runtime._malloc(8 * n);
    const mulPtr = runtime._malloc(8 * n);
    const outPtr = runtime._malloc(8);
    const flowPtr = options.edgeFlow ? runtime._malloc(8 * n) : 0;
    try {
      // Views are taken after every malloc: a grown heap detaches older views.
      runtime.HEAP32.set(from, fromPtr >> 2);
      runtime.HEAP32.set(to, toPtr >> 2);
      runtime.HEAPF64.set(Float64Array.from(capacity, (c) => (c === Infinity ? -1 : c)), capPtr >> 3);
      runtime.HEAPF64.set(multiplier, mulPtr >> 3);
      const status = solveArraysRaw(numNodes, source, sink, m, fromPtr, toPtr, capPtr, mulPtr,
                                    outPtr, flowPtr);
      const result = { status, optimal: status === 0, maxFlow: runtime.HEAPF64[outPtr >> 3] };
      if (flowPtr) result.edgeFlow = runtime.HEAPF64.slice(flowPtr >> 3, (flowPtr >> 3) + m);
      return result;
    } finally {
      for (const p of [fromPtr, toPtr, capPtr, mulPtr, outPtr, flowPtr]) if (p) runtime._free(p);
    }
  }

  return {
    solve,
    solveText,
    maxFlow,
    solveArrays,
    version: () => readVersion(),
    runtime,
  };
}
