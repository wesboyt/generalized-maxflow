"""Command line entry point: ``gmaxflow graph.json`` or JSON on stdin."""

from __future__ import annotations

import argparse
import json
import sys
from typing import Optional, Sequence

from . import solve_json, version


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="gmaxflow",
        description="Generalized maximum flow: maximum flow with per-edge gain multipliers.",
    )
    parser.add_argument(
        "graph",
        nargs="?",
        default="-",
        help="a JSON file holding the graph; omit it to read stdin",
    )
    parser.add_argument("-o", "--output", help="write the answer here instead of stdout")
    parser.add_argument("-c", "--compact", action="store_true", help="emit one line of JSON")
    parser.add_argument("-v", "--version", action="version", version=version())
    arguments = parser.parse_args(argv)

    if arguments.graph == "-":
        text = sys.stdin.read()
    else:
        with open(arguments.graph, encoding="utf-8") as handle:
            text = handle.read()
    if not text.strip():
        parser.error("no input: pass a file or pipe JSON on stdin")

    try:
        request = json.loads(text)
    except json.JSONDecodeError as problem:
        print(json.dumps({"status": "invalid_input", "error": str(problem)}), file=sys.stdout)
        return 1
    if isinstance(request, dict):
        options = dict(request.get("options") or {})
        options["pretty"] = not arguments.compact
        request["options"] = options

    answer = solve_json(json.dumps(request))

    if arguments.output:
        with open(arguments.output, "w", encoding="utf-8") as handle:
            handle.write(answer + "\n")
    else:
        sys.stdout.write(answer + "\n")

    try:
        return 1 if json.loads(answer).get("status") == "invalid_input" else 0
    except json.JSONDecodeError:
        return 1


if __name__ == "__main__":
    sys.exit(main())
