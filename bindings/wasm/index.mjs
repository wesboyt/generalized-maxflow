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

  return {
    solve,
    solveText,
    maxFlow,
    version: () => readVersion(),
    runtime,
  };
}
