"""Independent audit of a solver answer.

Rebuilds the residual network from the reported edge flows and looks for the two
things that are supposed to be absent when a generalized flow is maximum: an
augmenting path from the source, and a flow-generating cycle that can reach the
sink. Nothing here shares code with the C++ solver.

    python tools/gap_probe.py graph.json answer.json
"""

from __future__ import annotations

import json
import math
import sys

EPS = 1e-9


def build_residual(graph: dict, answer: dict):
    """Returns (arcs, nodes). Each arc is (tail, head, residual, gain, label)."""
    flows = {entry["index"]: entry["flow"] for entry in answer["edges"]}
    arcs = []
    nodes = set()
    for index, edge in enumerate(graph["edges"]):
        tail, head = edge["from"], edge["to"]
        capacity, gain = edge["capacity"], edge["multiplier"]
        flow = flows.get(index, 0.0)
        nodes.update((tail, head))
        arcs.append((tail, head, capacity - flow, gain, f"e{index}+"))
        arcs.append((head, tail, flow * gain, 1.0 / gain, f"e{index}-"))
    nodes.update((graph["source"], graph["sink"]))
    return arcs, sorted(nodes, key=str)


def reaches(arcs, nodes, target):
    """Nodes with a residual path to `target`."""
    found = {target}
    changed = True
    while changed:
        changed = False
        for tail, head, residual, _gain, _label in arcs:
            if residual > EPS and head in found and tail not in found:
                found.add(tail)
                changed = True
    return found


def augmenting_path(arcs, source, sink):
    parent = {source: None}
    stack = [source]
    while stack:
        node = stack.pop()
        if node == sink:
            path = []
            while parent[node] is not None:
                arc = parent[node]
                path.append(arc)
                node = arc[0]
            return list(reversed(path))
        for arc in arcs:
            if arc[0] == node and arc[2] > EPS and arc[1] not in parent:
                parent[arc[1]] = arc
                stack.append(arc[1])
    return None


def generating_cycle(arcs, nodes, allowed):
    """Bellman-Ford over -log(gain): a negative cycle amplifies flow."""
    usable = [a for a in arcs if a[2] > EPS and a[0] in allowed and a[1] in allowed]
    distance = {node: 0.0 for node in allowed}
    parent = {node: None for node in allowed}
    updated = None
    for _ in range(len(allowed) + 1):
        updated = None
        for tail, head, residual, gain, label in usable:
            candidate = distance[tail] - math.log(gain)
            if candidate < distance[head] - 1e-12:
                distance[head] = candidate
                parent[head] = (tail, head, residual, gain, label)
                updated = head
        if updated is None:
            return None
    # `updated` is reachable from a negative cycle; walk back into it.
    node = updated
    for _ in range(len(allowed) + 1):
        node = parent[node][0]
    cycle = []
    walker = node
    while True:
        arc = parent[walker]
        cycle.append(arc)
        walker = arc[0]
        if walker == node:
            break
    cycle.reverse()
    product = 1.0
    for arc in cycle:
        product *= arc[3]
    return cycle, product


def main() -> int:
    graph = json.load(open(sys.argv[1], encoding="utf-8"))
    answer = json.load(open(sys.argv[2], encoding="utf-8"))
    arcs, nodes = build_residual(graph, answer)
    source, sink = graph["source"], graph["sink"]

    to_sink = reaches(arcs, nodes, sink)
    print(f"nodes that reach the sink: {sorted(to_sink, key=str)}")

    path = augmenting_path([a for a in arcs if a[1] in to_sink or a[1] == sink], source, sink)
    print(f"augmenting path: {[a[4] for a in path] if path else None}")

    found = generating_cycle(arcs, nodes, to_sink)
    if found is None:
        print("flow-generating cycle reaching the sink: none")
    else:
        cycle, product = found
        print(f"flow-generating cycle reaching the sink: "
              f"{[(a[0], a[1], a[4]) for a in cycle]} gain product {product:.6f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
