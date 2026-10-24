# generalized-maxflow

Maximum flow through a network whose edges **change the amount that passes
through them**.

Every edge carries a capacity and a multiplier. Pushing `x` units into an edge
consumes `x` units of its capacity and delivers `multiplier * x` units at the
far end, so flow can leak away (multiplier below 1), be conserved (exactly 1) or
be amplified (above 1). This is the *generalized maximum flow* problem, and it
is what you want for power transmission with line losses, currency conversion,
evaporation and spoilage in logistics, or any pipeline whose stages have a
yield.

The interface is a JSON adjacency list, the same for every binding:

```json
{
  "source": "s",
  "sink": "t",
  "adjacency": {
    "s": [{"to": "a", "capacity": 10, "multiplier": 1.0}],
    "a": [{"to": "t", "capacity": 5,  "multiplier": 2.0}]
  }
}
```

```json
{
  "status": "optimal",
  "max_flow": 10,
  "source_outflow": 5,
  "paths": [
    {
      "kind": "path",
      "flow_in": 5, "flow_out": 10, "gain": 2,
      "nodes": ["s", "a", "t"],
      "steps": [
        {"edge": 0, "from": "s", "to": "a", "direction": "forward", "multiplier": 1, "flow_in": 5, "flow_out": 5},
        {"edge": 1, "from": "a", "to": "t", "direction": "forward", "multiplier": 2, "flow_in": 5, "flow_out": 10}
      ],
      "bottleneck": {"edge": 1, "from": "a", "to": "t", "direction": "forward", "step": 1}
    }
  ],
  "certificate": {"no_augmenting_path": true, "no_generating_cycle_reaching_sink": true, ...}
}
```

Note what the answer is **not**: 5. The constriction is `a -> t`, which admits
only 5 units, but those 5 units are doubled on the way through. A generalized
flow solver has to account for gain downstream of the constriction, which is
what makes the bottleneck of a path `min over edges of residual / (product of
multipliers ahead of it)` rather than the smallest residual capacity.

## Install

```bash
pip install generalized-maxflow
```

```bash
npm install generalized-maxflow   # WebAssembly build, browser and Node
```

From source you need CMake 3.18 and a C++17 compiler:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/gmaxflow graph.json
```

## Use it

Python:

```python
import gmaxflow

answer = gmaxflow.solve(graph)          # graph is a dict or JSON text
print(answer["max_flow"])

for path in answer["paths"]:
    print(path["nodes"], path["flow_in"], "->", path["flow_out"],
          "constricted at", path["bottleneck"]["edge"])

gmaxflow.max_flow(graph, threads=0)     # 0 = every core
```

JavaScript / WebAssembly:

```js
import createSolver from "generalized-maxflow";

const solver = await createSolver();
const answer = solver.solve(graph);     // takes and returns plain objects
console.log(answer.max_flow);
```

Command line (reads a file or stdin, writes JSON):

```bash
gmaxflow graph.json
cat graph.json | gmaxflow --compact --threads 0
```

C++:

```cpp
#include "gmf/gmf.hpp"

gmf::Problem problem;
problem.num_nodes = 3;
problem.source = 0;
problem.sink = 2;
problem.edges = {{0, 1, 10.0, 1.0}, {1, 2, 5.0, 2.0}};

const gmf::Result result = gmf::solve(problem);
// result.max_flow == 10, result.augmentations holds the filled paths
```

## Input

| field | meaning |
| --- | --- |
| `source`, `sink` | node labels; any string or number. Aliases: `src`/`start`, `dst`/`target`/`end` |
| `adjacency` | `{tail: [edge, ...]}`, or an array whose index is the tail |
| `edges` | a flat list; each edge names its own `from` |
| `nodes` | optional, fixes the node ordering |
| `options` | see below |

An edge is an object with `to`, `capacity` and `multiplier` (aliases: `head`/`v`,
`cap`, `gain`/`mult`; `from`/`tail`/`u` when it is not implied by the adjacency
key), or a positional array — `[to, capacity, multiplier]` inside an adjacency
list, `[from, to, capacity, multiplier]` in an `edges` list. A missing or null
capacity means uncapacitated. Capacities must be non-negative and multipliers
finite and strictly positive. `adjacency` and `edges` may both be given; the
edges are pooled.

| option | default | meaning |
| --- | --- | --- |
| `epsilon` | `1e-12` | relative tolerance, scaled by the largest capacity |
| `threads` | `1` | fan the path search out across threads; `0` = every core |
| `max_iterations` | size-derived | stop after this many augmentations |
| `collect_paths` | `true` | set false to drop the `paths` array and save memory |
| `cancel_generating_cycles` | `true` | set false to reject amplifying inputs instead of solving them |
| `pretty` | `false` | indent the output |

## Output

`status` is one of `optimal`, `unbounded`, `iteration_limit`, `suboptimal`,
`generating_cycle_rejected` or `invalid_input`, and `optimal` is a boolean. On
`invalid_input` there is an `error` field and nothing else.

`max_flow` is what arrives at the sink and `source_outflow` is what left the
source; with lossy or amplifying edges these differ, and that is the point.

`paths` is the sequence of augmentations in the order they were filled. Each has
the `nodes` it visits, the `steps` it took, the flow entering and leaving, the
`gain` (their product) and the `bottleneck` — the constricting edge, the one
that ran out and so decided how much this path could carry. A step's
`direction` is `backward` when the augmentation pushed flow *back* over an edge
to re-route earlier flow; see below for why that is necessary.

A `kind` of `flow_generating_cycle` means the entry is not a path from the
source but a cycle whose multipliers multiply to more than 1, together with the
route that carried the flow it manufactured to the sink. `cycle_length` says how
many of the steps belong to the cycle, and `excess` is what the cycle produced.

`certificate` is the proof the answer is maximum, and `stats` reports iterations,
searches and the threads actually used.

## Algorithm

[docs/ALGORITHM.md](docs/ALGORITHM.md) has the derivation. In short:

1. **Residual network.** Each edge becomes two arcs: forward, with the
   remaining capacity and multiplier `g`; and backward, carrying the flow
   already delivered with multiplier `1/g`. Pushing into the backward arc undoes
   flow and recovers it at the original multiplier.
2. **Highest-gain augmenting path.** Repeatedly find the residual path whose
   multipliers multiply to the most and fill it to its constriction. This is
   Onaga's algorithm, and the choice of *maximum* gain is what makes it correct:
   augmenting along a maximum-gain path cannot create a flow-generating cycle,
   so the invariant that makes the termination test valid is preserved.
3. **Flow-generating cycles.** A cycle whose multipliers multiply to more than 1
   manufactures flow out of nothing. If it can reach the sink, it is a
   *generalized augmenting path* and the flow is not maximum. Such cycles are
   saturated and the excess they produce is routed to the sink.
4. **Stop on a certificate**, not on running out of ideas: the flow is maximum
   exactly when the residual network holds neither an augmenting path nor a
   flow-generating cycle that can reach the sink. Both are re-checked from
   scratch at the end, and both appear in the answer.

The search runs in log-gain space — maximising a sum of logarithms rather than a
product — so a long lossy chain does not underflow to zero and get discarded.

Two things are worth knowing before trusting a number from any generalized flow
code, including this one:

- **Greedy does not work.** Taking the best-looking path each time without
  reverse arcs gets stuck, exactly as plain Ford-Fulkerson does, and the answer
  can be arbitrarily far from maximum. The reverse arcs with reciprocal
  multipliers are not an optimization; they are what makes the result correct.
- **It is not strongly polynomial.** Onaga's algorithm, and cycle cancellation
  on top of it, can need many augmentations on adversarial inputs. There is an
  iteration limit, and reaching it is reported as `iteration_limit` rather than
  dressed up as an answer. Capacity-scaling algorithms (Goldberg-Plotkin-Tardos,
  Radzik) have better bounds and are not implemented here.

## How it is checked

- Networks whose multipliers are all 1 are compared against an independent
  Dinic maximum flow implementation; 300 random instances per run.
- `tools/lp_check.py` states the problem as a linear program and solves it with
  scipy's HiGHS, which shares no code with the solver. 2570 random instances
  across lossy and amplifying regimes, up to 80 nodes, agree to 1e-6. This is
  what caught every defect listed in `tests/test_regression.cpp`, including one
  that only showed up on a single 40-node graph after thousands of smaller
  instances had passed.
- `tools/gap_probe.py` rebuilds the residual network from a reported answer in
  Python and independently looks for the two things the certificate claims are
  absent.
- Every random instance is also checked for capacity feasibility and flow
  conservation, and the sequential and multi-threaded engines are checked
  against each other.
- `tests/test_regression.cpp` holds the instances that once produced a wrong
  answer, each with the optimum from the linear program and a note on the defect
  it caught.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/gmf_tests
python tools/lp_check.py --trials 400 --max-multiplier 2.0
python tools/lp_check.py --graph examples/power_grid.json
pytest tests/test_python.py
```

## Examples

`examples/` holds three graphs worth running, each annotated with what it shows:

| file | what it shows |
| --- | --- |
| `power_grid.json` | transmission with line losses: 1012 units leave the plant, 918 reach the city |
| `rerouting.json` | the optimum needs flow pushed *back* over an edge; greedy filling stops at 2 of 3 |
| `currency_arbitrage.json` | exchange rates as multipliers, so a rate cycle whose product exceeds 1 manufactures 69.78 USD from no starting capital |

```bash
./build/gmaxflow examples/currency_arbitrage.json
```

## License

MIT. See [LICENSE](LICENSE).
