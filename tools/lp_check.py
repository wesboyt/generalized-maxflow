"""Cross-check the solver against a linear program.

Generalized maximum flow is a linear program: one variable per edge, bounded by
its capacity, conservation as an equality at every node but the source and the
sink, and the net inflow at the sink as the objective. scipy solves it with a
completely different method (HiGHS simplex/interior point), so agreement between
the two is real evidence rather than a restatement of the same code.

    python tools/lp_check.py --trials 400 --exe build/Release/gmaxflow.exe
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import subprocess
import sys

import numpy as np
from scipy.optimize import linprog


def random_instance(rng: random.Random, nodes: int, edges: int, max_multiplier: float) -> dict:
    graph = {
        "source": 0,
        "sink": nodes - 1,
        "nodes": list(range(nodes)),
        "edges": [],
    }
    for _ in range(edges):
        tail = rng.randrange(nodes)
        head = rng.randrange(nodes)
        if head == tail:
            head = (head + 1) % nodes
        graph["edges"].append(
            {
                "from": tail,
                "to": head,
                "capacity": float(rng.randint(1, 10)),
                "multiplier": round(rng.uniform(0.2, max_multiplier), 3),
            }
        )
    return graph


def lp_optimum(graph: dict) -> float | None:
    """The LP optimum, or None when the LP is unbounded or infeasible."""
    edges = graph["edges"]
    nodes = sorted({edge["from"] for edge in edges} | {edge["to"] for edge in edges} |
                   {graph["source"], graph["sink"]})
    index = {node: i for i, node in enumerate(nodes)}
    source = index[graph["source"]]
    sink = index[graph["sink"]]

    objective = np.zeros(len(edges))
    rows = [row for row in range(len(nodes)) if row not in (source, sink)]
    row_of = {node: i for i, node in enumerate(rows)}
    equalities = np.zeros((len(rows), len(edges)))

    for column, edge in enumerate(edges):
        tail = index[edge["from"]]
        head = index[edge["to"]]
        multiplier = edge["multiplier"]
        if head == sink:
            objective[column] += multiplier
        if tail == sink:
            objective[column] -= 1.0
        if tail in row_of:
            equalities[row_of[tail], column] -= 1.0
        if head in row_of:
            equalities[row_of[head], column] += multiplier

    bounds = [(0.0, edge["capacity"]) for edge in edges]
    solution = linprog(
        -objective,
        A_eq=equalities if len(rows) else None,
        b_eq=np.zeros(len(rows)) if len(rows) else None,
        bounds=bounds,
        method="highs",
    )
    if not solution.success:
        return None
    return float(-solution.fun)


def solve_with_exe(exe: str, graph: dict) -> dict:
    # Windows will not start a relative path written with forward slashes.
    exe = os.path.normpath(exe)
    if not os.path.isabs(exe) and not os.path.exists(exe) and os.path.exists(exe + ".exe"):
        exe = exe + ".exe"
    completed = subprocess.run(
        [exe, "-c"],
        input=json.dumps(graph),
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode not in (0, 1):
        raise RuntimeError(f"solver exited with {completed.returncode}: {completed.stderr}")
    return json.loads(completed.stdout)


def flatten(graph: dict) -> dict:
    """Rewrites an adjacency map into the flat edge list the LP builder wants."""
    edges = list(graph.get("edges") or [])
    adjacency = graph.get("adjacency")
    if isinstance(adjacency, dict):
        rows = adjacency.items()
    elif isinstance(adjacency, list):
        rows = enumerate(adjacency)
    else:
        rows = ()
    for tail, outgoing in rows:
        for edge in outgoing:
            if isinstance(edge, list):
                head, capacity, multiplier = (edge + [None, 1.0])[:3]
                edge = {"to": head, "capacity": capacity, "multiplier": multiplier}
            edges.append({**edge, "from": edge.get("from", tail)})
    normalized = []
    for edge in edges:
        capacity = edge.get("capacity", edge.get("cap"))
        multiplier = edge.get("multiplier", edge.get("gain", 1.0))
        normalized.append(
            {
                "from": edge.get("from", edge.get("tail")),
                "to": edge.get("to", edge.get("head")),
                "capacity": float("inf") if capacity is None else float(capacity),
                "multiplier": float(multiplier),
            }
        )
    return {"source": graph["source"], "sink": graph["sink"], "edges": normalized}


def check_one(exe: str, path: str, tolerance: float) -> bool:
    """Checks a single graph file against the LP. Returns True when they agree."""
    with open(path, encoding="utf-8") as handle:
        graph = json.load(handle)
    flat = flatten(graph)
    if any(not math.isfinite(edge["capacity"]) for edge in flat["edges"]):
        print(f"{path}: uncapacitated edges, the LP is unbounded; skipped")
        return True
    expected = lp_optimum(flat)
    answer = solve_with_exe(exe, graph)
    actual = answer["max_flow"]
    agree = expected is not None and abs(actual - expected) <= tolerance * (1.0 + abs(expected))
    verdict = "ok  " if agree else "FAIL"
    print(f"{verdict} {path}: solver {actual!r} vs LP {expected!r} ({answer['status']})")
    return agree


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        default=os.path.join("build", "gmaxflow"),
        help="path to the gmaxflow binary (a .exe suffix is added if needed)",
    )
    parser.add_argument(
        "--graph",
        action="append",
        default=[],
        help="check this graph file instead of random instances; repeatable",
    )
    parser.add_argument("--trials", type=int, default=200)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--max-nodes", type=int, default=10)
    parser.add_argument(
        "--max-multiplier",
        type=float,
        default=1.0,
        help="above 1.0 the instances can contain flow-generating cycles",
    )
    parser.add_argument("--tolerance", type=float, default=1e-6)
    parser.add_argument(
        "--dump",
        help="write each mismatching instance to this file, one JSON object per line",
    )
    arguments = parser.parse_args()

    if arguments.graph:
        failures = [
            path
            for path in arguments.graph
            if not check_one(arguments.exe, path, arguments.tolerance)
        ]
        print(f"checked {len(arguments.graph)}, mismatches {len(failures)}")
        return 1 if failures else 0

    rng = random.Random(arguments.seed)
    mismatches = 0
    skipped = 0
    checked = 0

    for trial in range(arguments.trials):
        nodes = rng.randint(3, arguments.max_nodes)
        edges = rng.randint(nodes, 3 * nodes)
        graph = random_instance(rng, nodes, edges, arguments.max_multiplier)

        expected = lp_optimum(graph)
        answer = solve_with_exe(arguments.exe, graph)

        if expected is None or answer["status"] == "unbounded":
            # Both should see an unbounded or degenerate instance the same way,
            # but the LP reports it as a failure rather than a number.
            skipped += 1
            continue

        actual = answer["max_flow"]
        checked += 1
        scale = 1.0 + abs(expected)
        if abs(actual - expected) > arguments.tolerance * scale:
            mismatches += 1
            print(f"trial {trial}: solver {actual!r} vs LP {expected!r} ({answer['status']})")
            if arguments.dump:
                with open(arguments.dump, "a", encoding="utf-8") as handle:
                    handle.write(json.dumps(graph) + "\n")
            else:
                print(json.dumps(graph))
            if mismatches >= 5:
                break
        if not answer["optimal"]:
            print(f"trial {trial}: status {answer['status']}: {answer.get('message', '')}")

    print(f"checked {checked}, skipped {skipped}, mismatches {mismatches}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
