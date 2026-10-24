"""Tests for the Python package. Run with: pytest tests/test_python.py"""

from __future__ import annotations

import json
import math

import pytest

import gmaxflow


def test_single_edge_applies_its_multiplier():
    assert gmaxflow.max_flow(
        {
            "source": "s",
            "sink": "t",
            "edges": [{"from": "s", "to": "t", "capacity": 10, "multiplier": 0.5}],
        }
    ) == pytest.approx(5.0)


def test_gain_downstream_of_the_constriction_is_counted():
    answer = gmaxflow.solve(
        {
            "source": "s",
            "sink": "t",
            "adjacency": {
                "s": [{"to": "a", "capacity": 10, "multiplier": 1.0}],
                "a": [{"to": "t", "capacity": 5, "multiplier": 2.0}],
            },
        }
    )
    assert answer["status"] == "optimal"
    assert answer["optimal"] is True
    assert answer["max_flow"] == pytest.approx(10.0)
    assert answer["source_outflow"] == pytest.approx(5.0)
    assert len(answer["paths"]) == 1
    path = answer["paths"][0]
    assert path["nodes"] == ["s", "a", "t"]
    assert path["bottleneck"]["from"] == "a"
    assert path["bottleneck"]["to"] == "t"


def test_paths_report_the_chain_rule():
    answer = gmaxflow.solve(
        {
            "source": 0,
            "sink": 2,
            "edges": [
                {"from": 0, "to": 1, "capacity": 8, "multiplier": 0.5},
                {"from": 1, "to": 2, "capacity": 8, "multiplier": 0.5},
            ],
        }
    )
    (path,) = answer["paths"]
    carried = path["flow_in"]
    for step in path["steps"]:
        assert step["flow_in"] == pytest.approx(carried)
        assert step["flow_out"] == pytest.approx(carried * step["multiplier"])
        carried = step["flow_out"]
    assert carried == pytest.approx(path["flow_out"])
    assert answer["max_flow"] == pytest.approx(2.0)


def test_a_flow_generating_cycle_is_reported_as_such():
    answer = gmaxflow.solve(
        {
            "source": "s",
            "sink": "t",
            "edges": [
                {"from": "s", "to": "a", "capacity": 0, "multiplier": 1},
                {"from": "a", "to": "b", "capacity": 1, "multiplier": 3},
                {"from": "b", "to": "a", "capacity": 10, "multiplier": 1},
                {"from": "a", "to": "t", "capacity": 100, "multiplier": 1},
            ],
        }
    )
    assert answer["max_flow"] == pytest.approx(2.0)
    assert answer["paths"][0]["kind"] == "flow_generating_cycle"
    assert answer["paths"][0]["excess"] == pytest.approx(2.0)


def test_every_answer_carries_a_certificate():
    answer = gmaxflow.solve(
        {"source": 0, "sink": 1, "edges": [{"from": 0, "to": 1, "capacity": 3, "multiplier": 1}]}
    )
    certificate = answer["certificate"]
    assert certificate["no_augmenting_path"] is True
    assert certificate["no_generating_cycle_reaching_sink"] is True
    assert certificate["max_conservation_error"] == pytest.approx(0.0)
    assert certificate["max_capacity_violation"] == pytest.approx(0.0)


def test_a_missing_capacity_is_unbounded():
    graph = {"source": "s", "sink": "t", "edges": [{"from": "s", "to": "t", "multiplier": 2}]}
    answer = gmaxflow.solve(graph)
    assert answer["status"] == "unbounded"
    assert math.isinf(answer["max_flow"])
    assert math.isinf(gmaxflow.max_flow(graph))


def test_options_are_forwarded():
    graph = {
        "source": 0,
        "sink": 1,
        "edges": [{"from": 0, "to": 1, "capacity": 4, "multiplier": 0.5}],
    }
    assert gmaxflow.solve(graph, collect_paths=False)["paths"] == []
    # The caller's graph is left alone.
    assert "options" not in graph


def test_json_text_is_accepted():
    request = json.dumps(
        {"source": 0, "sink": 1, "edges": [{"from": 0, "to": 1, "capacity": 2, "multiplier": 1}]}
    )
    assert gmaxflow.max_flow(request) == pytest.approx(2.0)
    assert json.loads(gmaxflow.solve_json(request))["max_flow"] == pytest.approx(2.0)


def test_a_bad_graph_raises():
    with pytest.raises(gmaxflow.InvalidGraphError):
        gmaxflow.solve({"sink": "t", "edges": []})
    with pytest.raises(gmaxflow.InvalidGraphError):
        gmaxflow.solve(
            {
                "source": "s",
                "sink": "t",
                "edges": [{"from": "s", "to": "t", "capacity": -1, "multiplier": 1}],
            }
        )
    with pytest.raises(gmaxflow.InvalidGraphError):
        gmaxflow.solve({"source": "s", "sink": "t"})
    with pytest.raises(json.JSONDecodeError):
        gmaxflow.solve("not json at all")


def test_require_optimal_raises_on_an_uncertified_answer():
    graph = {
        "source": "s",
        "sink": "t",
        "edges": [
            {"from": "s", "to": "t", "capacity": 1, "multiplier": 0.9},
            {"from": "s", "to": "t", "capacity": 1, "multiplier": 0.8},
        ],
    }
    # Two parallel edges need two augmentations, so one iteration cannot finish
    # and the answer comes back uncertified.
    with pytest.raises(gmaxflow.NotOptimalError) as raised:
        gmaxflow.solve(graph, max_iterations=1, require_optimal=True)
    assert raised.value.answer["status"] != "optimal"
    # Without the flag the partial answer comes back instead.
    assert gmaxflow.solve(graph, max_iterations=1)["optimal"] is False


def test_the_version_is_a_string():
    assert isinstance(gmaxflow.version(), str)
    assert gmaxflow.version()


def test_labels_outside_ascii_survive_the_native_boundary():
    answer = gmaxflow.solve(
        {
            "source": "São Paulo",
            "sink": "東京",
            "edges": [{"from": "São Paulo", "to": "東京", "capacity": 3, "multiplier": 0.5}],
        }
    )
    assert answer["max_flow"] == pytest.approx(1.5)
    assert answer["paths"][0]["nodes"] == ["São Paulo", "東京"]


def test_fat_path_and_plain_augmentation_agree():
    graph = {
        "source": 0,
        "sink": 4,
        "edges": [
            [0, 1, 100, 0.9], [1, 4, 100, 0.9],
            [0, 2, 1, 0.99], [2, 4, 1, 0.99],
            [0, 3, 10, 0.95], [3, 4, 10, 0.95],
            [1, 3, 5, 1.2], [3, 2, 5, 1.1],
        ],
    }
    scaled = gmaxflow.solve(graph)
    plain = gmaxflow.solve(graph, fat_path=False)
    unrounded = gmaxflow.solve(graph, gain_scaling=False)
    assert scaled["optimal"] and plain["optimal"] and unrounded["optimal"]
    assert scaled["max_flow"] == pytest.approx(plain["max_flow"], rel=1e-9)
    assert unrounded["max_flow"] == pytest.approx(plain["max_flow"], rel=1e-9)
    assert scaled["stats"]["scaling_phases"] >= 1
    assert plain["stats"]["scaling_phases"] == 0


def test_the_native_solver_version_matches_the_package():
    from importlib import metadata

    try:
        installed = metadata.version("generalized-maxflow")
    except metadata.PackageNotFoundError:
        pytest.skip("running from a checkout, not an installed distribution")
    assert gmaxflow.version() == installed


@pytest.mark.skipif(
    not __import__("os").environ.get("GMAXFLOW_TEST_INSTALLED"),
    reason="set GMAXFLOW_TEST_INSTALLED=1 to check the installed wheel itself",
)
def test_the_installed_package_carries_its_own_native_library():
    import os

    from gmaxflow import _native

    here = os.path.dirname(os.path.abspath(__file__))
    checkout = os.path.normcase(os.path.join(os.path.dirname(here), "python"))
    package = os.path.normcase(os.path.dirname(os.path.abspath(gmaxflow.__file__)))
    assert not package.startswith(checkout), "imported the checkout, not the wheel"
    assert os.path.isfile(_native.library_path())
