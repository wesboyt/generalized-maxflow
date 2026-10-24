"""Generalized maximum flow: maximum flow with per-edge gain multipliers.

Every edge carries a capacity and a multiplier. Pushing x units into an edge
consumes x units of its capacity and delivers ``multiplier * x`` units at the
other end, so flow can be lost (a multiplier below 1) or amplified (above 1).
The solver maximises what arrives at the sink.

    >>> import gmaxflow
    >>> gmaxflow.max_flow({
    ...     "source": "s",
    ...     "sink": "t",
    ...     "adjacency": {
    ...         "s": [{"to": "a", "capacity": 10, "multiplier": 1.0}],
    ...         "a": [{"to": "t", "capacity": 5, "multiplier": 2.0}],
    ...     },
    ... })
    10.0

The answer also carries the sequence of paths that were filled, each with the
flow that entered it, the flow it delivered and the edge that constricted it::

    answer = gmaxflow.solve(graph)
    for path in answer["paths"]:
        print(path["nodes"], path["flow_in"], "->", path["flow_out"])
"""

from __future__ import annotations

import json
from typing import Any, Dict, Mapping, Optional, Union

from . import _gmaxflow

__all__ = [
    "solve",
    "solve_json",
    "max_flow",
    "version",
    "InvalidGraphError",
    "NotOptimalError",
]

Graph = Union[Mapping[str, Any], str, bytes]

class InvalidGraphError(ValueError):
    """The graph itself was rejected: a bad endpoint, capacity or multiplier."""


class NotOptimalError(RuntimeError):
    """The solver stopped without certifying the answer as maximum.

    The partial answer is on the exception as ``.answer``.
    """

    def __init__(self, message: str, answer: Dict[str, Any]) -> None:
        super().__init__(message)
        self.answer = answer


def solve_json(request: str) -> str:
    """Solve a problem given as a JSON document and return a JSON document.

    This is the raw interface the C++ library exposes; the bytes are not parsed
    on the way in or out.
    """
    return _gmaxflow.solve_json(request)


def solve(
    graph: Graph,
    *,
    epsilon: Optional[float] = None,
    max_iterations: Optional[int] = None,
    collect_paths: Optional[bool] = None,
    cancel_generating_cycles: Optional[bool] = None,
    require_optimal: bool = False,
) -> Dict[str, Any]:
    """Solve a generalized maximum flow problem.

    `graph` is a mapping (or JSON text) with a ``source``, a ``sink`` and either
    an ``adjacency`` map or an ``edges`` list. Options may be passed here as
    keywords or inside the graph under ``options``; keywords win.

    With `require_optimal`, an answer the solver could not certify raises
    `NotOptimalError` instead of being returned.

    Raises `InvalidGraphError` when the graph is malformed.
    """
    if isinstance(graph, (str, bytes)):
        request = json.loads(graph)
    else:
        request = dict(graph)
    if not isinstance(request, dict):
        raise InvalidGraphError("the graph must be a JSON object")

    options = dict(request.get("options") or {})
    overrides = {
        "epsilon": epsilon,
        "max_iterations": max_iterations,
        "collect_paths": collect_paths,
        "cancel_generating_cycles": cancel_generating_cycles,
    }
    options.update({name: value for name, value in overrides.items() if value is not None})
    if options:
        request["options"] = options

    answer = json.loads(_gmaxflow.solve_json(json.dumps(request)))

    if answer.get("status") == "invalid_input":
        raise InvalidGraphError(answer.get("error", "invalid graph"))
    if require_optimal and not answer.get("optimal"):
        raise NotOptimalError(
            f"{answer.get('status')}: {answer.get('message', '')}".strip(), answer
        )
    return answer


def max_flow(graph: Graph, **options: Any) -> float:
    """The maximum flow delivered at the sink.

    Accepts the same options as `solve`. Returns ``float('inf')`` when the
    network admits unbounded flow.
    """
    return float(solve(graph, **options)["max_flow"])


def version() -> str:
    """The version of the native solver."""
    return _gmaxflow.version()
