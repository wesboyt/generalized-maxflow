# The algorithm

## The problem

A directed graph, a source `s` and a sink `t`. Each edge `e = (u, v)` has a
capacity `c(e) >= 0` and a multiplier `g(e) > 0`. A flow `f(e)` in `[0, c(e)]`
means `f(e)` units *enter* the edge at `u` and `g(e) * f(e)` units *arrive* at
`v`. Flow is conserved at every node but `s` and `t`:

```
for every node v other than s and t:
    sum over edges into v of g(e) * f(e)  =  sum over edges out of v of f(e)
```

Maximise what arrives at `t`. With every multiplier equal to 1 this is ordinary
maximum flow. With multipliers below 1 it models loss, and above 1, gain.

This is a linear program, which is why `tools/lp_check.py` can state it directly
and check the answers.

## Why the obvious approach is wrong

The intuitive method is: walk outward from the source, keep for every node the
best gain reaching it, pick the highest-gain path to the sink, fill it, repeat.
Two corrections turn that into a correct algorithm.

### Correction 1: the bottleneck is not the smallest capacity

On a path `s -> a -> t` with capacities 10 and 5 and multipliers 1 and 2, the
answer is 10, not 5. Only 5 units may enter the second edge, but they are
doubled on the way through. So what limits a path is not its smallest residual
capacity; it is

```
x  =  min over edges e on the path of  residual(e) / (product of multipliers before e)
```

and the constricting edge is the one achieving that minimum. The flow entering
each successive edge is the running product, which is the chain rule the
`steps` array in the answer spells out.

### Correction 2: reverse arcs with reciprocal multipliers

Greedy highest-gain filling, with no way to undo a decision, gets stuck in
exactly the way plain Ford-Fulkerson does. Filling the best path first can
consume capacity the optimum needed for a different path, and then no sequence
of remaining paths recovers it.

So the algorithm works on a **residual network**. Each edge contributes two arcs:

| arc | capacity | multiplier |
| --- | --- | --- |
| forward `u -> v` | `c(e) - f(e)` | `g(e)` |
| backward `v -> u` | `g(e) * f(e)` | `1 / g(e)` |

The backward arc's capacity is what the edge *delivered*, and its multiplier is
the reciprocal, because retracting `y` units at `v` returns `y / g(e)` units to
`u`. Pushing `x` into either arc is the single rule

```
residual(a)     -= x
residual(a ^ 1) += x * multiplier(a)
```

An augmentation that uses a backward arc appears in the answer with
`"direction": "backward"`: it is re-routing flow that an earlier augmentation
placed. `tests/test_solver.cpp` has an instance where the optimum is 3 and
greedy filling reaches 2.

A consequence worth noting: **the first augmentation creates arcs with gain
above 1** even in a purely lossy network, since the reverse of a lossy arc is an
amplifying one. So the path search can never assume non-negative costs. In
log-gain space the weights `-log g` are signed, which is why the search is
label-correcting (Bellman-Ford / SPFA) rather than Dijkstra.

## Highest-gain augmentation (Onaga)

With the residual network in place, the loop is:

> while a residual path from `s` to `t` exists, find one whose multipliers
> multiply to the most, fill it to its constriction, repeat.

The choice of *maximum* gain is not a heuristic for speed. It is what keeps the
algorithm correct, via Onaga's theorem:

> If the residual network contains no flow-generating cycle, then augmenting
> along a maximum-gain path leaves it with no flow-generating cycle.

A **flow-generating cycle** is a residual cycle whose multipliers multiply to
more than 1. Going around it once returns more than was put in, so it
manufactures flow out of nothing. The theorem says highest-gain augmentation
preserves their absence, and their absence is what makes "no augmenting path"
mean "optimal". Fill a *lower*-gain path instead and the invariant can break,
after which the termination test no longer certifies anything.

The search maximises the **sum of log multipliers** rather than their product.
That is not only for numerical tidiness: a 50-edge chain of halvings has gain
`2^-50`, and a solver that multiplies doubles underflows long chains toward zero
and discards paths that really do carry flow. `a_long_lossy_chain_keeps_its_precision`
in `tests/test_regression.cpp` pins this down.

## Flow-generating cycles in the input

If the input itself has a flow-generating cycle, there may be nothing to
augment from the source and still flow to deliver. A cycle with gain 3 and a
route to the sink will deliver flow with the source completely cut off, which is
the `a_flow_generating_cycle_is_cancelled_and_its_excess_routed` test.

Such a cycle, together with a residual path from it to the sink, is a
**generalized augmenting path**. The solver saturates the cycle and routes the
excess it produces to the sink. The two halves share one bottleneck computation:
with `x` units entering the first cycle arc and a cycle gain of `G`, the cycle
produces `x * (G - 1)` units of excess, so an arc on the routing path carries
`(G - 1) * (product of multipliers before it) * x`. Collecting a coefficient per
arc and solving for a single `x` satisfies every constraint at once. That
matters: compute the two bottlenecks separately and the excess can exceed what
the route can carry, which would leave flow stranded and break conservation.

### Netting the two directions of an edge

A cycle and the route carrying its excess can use the *same edge in opposite
directions*. Those two uses do not compete for that edge's capacity — they
cancel, because pushing into the backward arc hands capacity back to the forward
one. So the limit has to be computed per input edge, on the net change to its
flow. With `a` the total coefficient on the forward arc, `b` the total on the
backward arc, `g` the multiplier, `r_f = c - f` and `r_b = g * f`:

```
net = a - b / g            units of flow this edge gains per unit of x
net > 0:   x <= r_f / net
net < 0:   x <= r_b / (-net * g)      the flow cannot go below zero
net == 0:  this edge does not constrain x at all
```

Charging each direction separately is *feasible* — it only ever understates what
an edge allows — which is exactly why the error is easy to miss. It produced a
solver that answered "optimal" on thousands of instances and then, on one
40-node graph, repeated a single cycle and route forever with `x` shrinking by a
tenth each time: 500,000 iterations moved the delivered flow by 3e-6 and left it
12% short of the optimum. With the netting rule the same instance finishes in
sixteen iterations, exactly on the linear program's value. It is in
`tests/test_regression.cpp`.

Because an edge may be used both ways, the traversals cannot be applied to the
residuals one at a time as the walk is followed: the forward push alone can
exceed the forward residual, with the later backward push making it feasible
again. The amounts are therefore accumulated per arc and the net is applied once
per edge, so no intermediate state is ever infeasible. A consequence worth
knowing when reading the output: the `flow_in` and `flow_out` on each step are
what physically moves along that walk, and on a shared edge those amounts partly
cancel, so the change in that edge's reported `flow` is smaller than the step
suggests.

The cycle is found with Bellman-Ford over `-log g`, where a flow-generating
cycle is a negative cycle. Two details are easy to get wrong, and both were
caught here by cross-checking against the linear program:

- **The search must be confined to the nodes that can still reach the sink.** A
  gain-amplifying cycle in a dead end is not a generalized augmenting path — it
  can never deliver anything — but it still makes the maximum-gain labels
  diverge. Restricting every search to the sink-reaching subgraph removes it
  from consideration without special-casing, and loses nothing, because every
  path to the sink and every useful cycle lies inside that subgraph.
- **Detection must not be allowed to fail quietly.** The first implementation
  used a queue-based scan and read the cycle off the parent pointers at the
  moment a node was relaxed too many times. Those pointers can lead out of the
  cycle, and the extraction then returned nothing, which the caller read as "no
  cycle" — in other words, as optimality. The fix is full Bellman-Ford rounds
  followed by walking the parent pointers from a still-relaxable arc with a
  visited stamp, trying each candidate until a cycle closes.

## Stopping

The solver does not stop when it runs out of ideas. It stops on a condition that
is necessary and sufficient for a maximum generalized flow, and it reports that
condition in the answer:

> A feasible generalized flow is maximum exactly when its residual network
> contains neither an augmenting path from the source to the sink, nor a
> flow-generating cycle with a path to the sink.

This comes out of LP duality: an improving direction is a flow in the residual
network conserved everywhere but at `s` and `t` that increases the net at `t`.
Decomposing such a direction gives source-to-sink paths, flow-generating cycles
with their excess routed somewhere, flow-absorbing cycles, and bicycles — a
generating cycle joined to an absorbing cycle. Only two kinds of component raise
the net at `t`: a path to the sink, and a generating cycle whose excess reaches
the sink. If a direction improves, one of its components must, so one of those
two must be present.

Both checks are re-run from scratch at the end, over the final residual network,
and both appear as `certificate.no_augmenting_path` and
`certificate.no_generating_cycle_reaching_sink`. `status` is `optimal` only when
both hold. The certificate also carries the largest conservation error and
capacity violation, so a feasibility bug cannot hide behind a confident status.

`tools/gap_probe.py` performs the same two checks in Python, from the reported
edge flows, sharing no code with the solver.

## Complexity, and what this implementation does not do

Each path search is a label-correcting sweep, `O(V * E)` in the worst case and
much faster in practice. The incident arcs of each node are sorted by multiplier
so the sweep settles promising labels before dominated ones.

The number of augmentations is the honest weak point. Onaga's algorithm is not
strongly polynomial, and cycle cancellation on top of it is a greedy loop whose
only guarantee is that each step strictly increases the delivered flow and that
the stopping condition is exact. Adversarial inputs can need many iterations.
There is a limit — `1000 + 20 * (V + E)` by default, settable with
`max_iterations` — and hitting it is reported as `status: "iteration_limit"`
with the uncertified flow, rather than presented as an answer.

The algorithms with real bounds are capacity-scaling ones:
Goldberg-Plotkin-Tardos (1991), and Radzik's and Tardos-Wayne's refinements.
They cancel *all* flow-generating cycles at once by solving a minimum-cost flow
subproblem rather than one cycle at a time. None of that is implemented here.

Two notes on the floating-point reality:

- `epsilon` is relative, scaled by the largest finite capacity. A residual below
  `epsilon * scale` counts as zero, and a gain improvement must exceed `epsilon`
  in log space to count. So "optimal" means optimal up to that tolerance.
- An arc is retired from the search only when nothing at all can move through
  it. An earlier version retired the constricting arc whenever its residual had
  not dropped, which looked like a sensible progress guard and was wrong: a walk
  that pushes an arc *and its twin* legitimately hands that residual back. The
  guard deleted capacity the optimum needed and, worse, hid the remaining
  augmenting path from the certificate, so the solver reported a confidently
  wrong answer. Both instances that exposed it are in `tests/test_regression.cpp`.

## Parallelism

With `threads` above 1 the label-correcting sweep runs as a frontier algorithm:
every round expands the whole current frontier at once across the threads,
updating labels with a compare-and-exchange loop and collecting the next frontier
per thread. That frontier is exactly the set of nodes whose best incoming gain is
still in question after a constriction was removed, so the parallel work grows
and shrinks with the affected region of the graph.

Parent pointers are stored outside the label exchange, so a race can leave one
pointing at a slightly worse predecessor. The extracted path is therefore
re-checked against its own label, and a mismatch falls back to the sequential
sweep for that iteration — recorded as `stats.parallel_fallbacks`. Correctness
never depends on the race going one way. `the_parallel_engine_finds_the_same_maximum`
checks both engines against each other on random instances.

Threads are compiled out of the WebAssembly build, which needs no
`SharedArrayBuffer` and runs the sequential engine.

## References

- K. Onaga, *Dynamic programming of optimum flows in lossy communication nets*,
  IEEE Transactions on Circuit Theory, 1966. The highest-gain augmenting path
  method and the invariant it preserves.
- K. Truemper, *On max flows with gains and pure min-cost flows*, SIAM Journal on
  Applied Mathematics, 1977. The generalized augmenting path optimality
  condition.
- A. V. Goldberg, S. A. Plotkin, É. Tardos, *Combinatorial algorithms for the
  generalized circulation problem*, Mathematics of Operations Research, 1991.
  The first polynomial combinatorial algorithms, and the cancel-all-cycles idea.
- R. K. Ahuja, T. L. Magnanti, J. B. Orlin, *Network Flows*, 1993, chapter 15.
