// Smoke test for the WebAssembly build: node test_node.mjs
// Run from the assembled package directory, where gmaxflow.js sits beside this
// file.
import assert from "node:assert/strict";

import createSolver from "./index.mjs";

const solver = await createSolver();

{
  const answer = solver.solve({
    source: "s",
    sink: "t",
    adjacency: {
      s: [{ to: "a", capacity: 10, multiplier: 1.0 }],
      a: [{ to: "t", capacity: 5, multiplier: 2.0 }],
    },
  });
  assert.equal(answer.status, "optimal");
  assert.equal(answer.optimal, true);
  assert.equal(answer.max_flow, 10);
  assert.equal(answer.source_outflow, 5);
  assert.equal(answer.paths.length, 1);
  assert.deepEqual(answer.paths[0].nodes, ["s", "a", "t"]);
  assert.equal(answer.paths[0].bottleneck.edge, 1);
  assert.equal(answer.certificate.no_augmenting_path, true);
}

{
  // A lossy chain: three halvings of a capacity-8 path.
  assert.equal(
    solver.maxFlow({
      source: 0,
      sink: 3,
      edges: [
        { from: 0, to: 1, capacity: 8, multiplier: 0.5 },
        { from: 1, to: 2, capacity: 8, multiplier: 0.5 },
        { from: 2, to: 3, capacity: 8, multiplier: 0.5 },
      ],
    }),
    1,
  );
}

{
  // A flow-generating cycle manufactures flow with the source cut off.
  const answer = solver.solve({
    source: "s",
    sink: "t",
    edges: [
      { from: "s", to: "a", capacity: 0, multiplier: 1 },
      { from: "a", to: "b", capacity: 1, multiplier: 3 },
      { from: "b", to: "a", capacity: 10, multiplier: 1 },
      { from: "a", to: "t", capacity: 100, multiplier: 1 },
    ],
  });
  assert.equal(answer.max_flow, 2);
  assert.equal(answer.paths[0].kind, "flow_generating_cycle");
}

{
  // Reverse residual arcs are needed here; a greedy pass would stop at 2.
  const answer = solver.solve({
    source: 0,
    sink: 3,
    edges: [
      { from: 0, to: 1, capacity: 1, multiplier: 1 },
      { from: 0, to: 2, capacity: 1, multiplier: 1 },
      { from: 1, to: 3, capacity: 1, multiplier: 1 },
      { from: 2, to: 3, capacity: 1, multiplier: 2 },
      { from: 1, to: 2, capacity: 1, multiplier: 1.5 },
    ],
  });
  assert.equal(answer.status, "optimal");
  assert.ok(Math.abs(answer.max_flow - 3) < 1e-9, `got ${answer.max_flow}`);
}

{
  assert.equal(solver.solve({ source: "s", sink: "t", edges: [{ from: "s", to: "t" }] }).status,
    "unbounded");
  assert.throws(() => solver.solve({ sink: "t", edges: [] }), /generalized-maxflow/);
  assert.match(solver.version(), /^\d+\.\d+\.\d+$/);
  const text = solver.solveText(
    JSON.stringify({ source: 0, sink: 1, edges: [{ from: 0, to: 1, capacity: 2 }] }),
  );
  assert.equal(JSON.parse(text).max_flow, 2);
}

console.log("wasm smoke test passed");
