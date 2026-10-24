# The algorithm

The full write-up now lives in the [README](../README.md#the-algorithm): the
problem statement and notation, the residual network, the chain-rule bottleneck,
Onaga's highest-gain augmentation, node potentials, cycle detection off the
parent forest, the optimality certificate, Tardos and Wayne's Fat-Path with
gain-scaling, complexity, and the measurements
behind the decision not to run any of it concurrently.

[`algorithm-flow.svg`](algorithm-flow.svg) is the figure: how a constricting
edge is identified, and how the update propagates before the next constriction
is chosen.

Implementation notes that did not belong in the README:

- **Where the code is.** `src/solver.cpp` holds the whole solver.
  `establish_potentials` is the sweep that both hunts for flow-generating cycles
  and leaves valid potentials behind; `dijkstra_scan` plus
  `apply_potential_update` is the per-augmentation search; `bottleneck_of` and
  `push_walk` implement the chain-rule limit including the net-change rule for
  an edge a walk uses in both directions; `finalize` computes the certificate.
  The Fat-Path phases are `run_fat_path`; `canonical_labels` is the backward
  Dijkstra that starts each phase, `cancel_violations` the Dijkstra half of
  CancelCycles and `cancel_cycles` the sweep half, and the fat test and the
  gain rounding are the `kFat` and `kRounded` instantiations of
  `dijkstra_scan_impl`.

- **Where the Fat-Path tuning came from.** Each of the less obvious choices in
  `run_fat_path` replaced a measured failure: fatness read off stale potentials
  (99 000 cancellations on a 1 000-node lossy graph, fixed by canonical labels);
  cycle excess routed by fewest hops (new cycles as fast as old ones went);
  cancelling every positive cycle inside a rounded phase rather than only those
  gaining more than q (4 883 cancellations instead of 41); and a mean-gain
  rather than total-gain threshold for that test, which rejected exactly the
  cycles the q-tolerant sweep diverges on and forced an O(nm) Bellman-Ford
  fallback per cycle (5.0 s instead of 0.33 s).

- **Why the certificate uses the slow routines.** `find_cycle_by_rounds` and a
  plain reachability search are deliberately not the accelerated paths the main
  loop takes. A certificate that shared the loop's heuristics could only confirm
  the loop's own blind spots, which is exactly how a flow 26% below optimum once
  came back labelled `optimal`.

- **Reading the step log.** `Step` stores only the edge, the direction and the
  flow entering; the endpoints, multiplier and delivered amount are derived with
  `step_from`, `step_to`, `step_multiplier` and `step_flow_out`. On a walk that
  uses an edge in both directions the per-step amounts partly cancel, so that
  edge's reported `flow` changes by less than the steps alone suggest.

- **Regression instances.** Every entry in `tests/test_regression.cpp` is a graph
  that once produced a wrong answer, carrying the optimum from the linear
  program and a note on the defect it caught. They are the most useful thing to
  read before changing the cycle or bottleneck logic.
