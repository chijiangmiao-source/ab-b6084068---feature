"""Unit tests for validation, the solver and the adjacency export.

Run with: ``python3 tests/harness.py test_solver`` from the repo root.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from harness import assert_raises  # noqa: E402

from app.errors import TopologyError, ValidationError  # noqa: E402
from app.solver import build_adjacency, check_feasibility, solve  # noqa: E402
from app.validation import MAX_COST, parse_problem  # noqa: E402

CORE = Path(os.environ.get(
    "STEINER_CORE", Path(__file__).resolve().parents[1] / "core" / "steiner"
))
assert CORE.exists(), f"native core missing: {CORE}"


def make(nodes, edges, endpoints, max_edges=None):
    body = {"nodes": nodes, "edges": edges, "endpoints": endpoints}
    if max_edges is not None:
        body["max_edges"] = max_edges
    return parse_problem(body)


def edge(id_, s, t, c):
    return {"id": id_, "source": s, "target": t, "cost": c}


# --------------------------------------------------------------------------
# Validation
# --------------------------------------------------------------------------

class TestValidation:
    def test_minimal_ok(self):
        p = make(["a", "b"], [edge("e1", "a", "b", 1)], ["a", "b"])
        assert len(p.edges) == 1
        assert p.endpoints == [0, 1]

    def test_parallel_edges_allowed(self):
        p = make(
            ["a", "b"],
            [edge("e1", "a", "b", 3), edge("e2", "b", "a", 2)],
            ["a", "b"],
        )
        assert len(p.edges) == 2

    def test_body_must_be_object(self):
        with assert_raises(ValidationError) as ctx:
            parse_problem(["not", "an", "object"])
        assert ctx.value.code == "INVALID_BODY"

    def test_missing_nodes(self):
        with assert_raises(ValidationError) as ctx:
            parse_problem({"edges": [], "endpoints": ["a", "b"]})
        assert ctx.value.code == "MISSING_FIELD"
        assert ctx.value.pointer == "/nodes"

    def test_node_count_too_small(self):
        with assert_raises(ValidationError) as ctx:
            parse_problem({"nodes": ["a"], "edges": [],
                           "endpoints": ["a", "a"]})
        assert ctx.value.code == "NODE_COUNT_OUT_OF_RANGE"

    def test_duplicate_node(self):
        with assert_raises(ValidationError) as ctx:
            parse_problem({"nodes": ["a", "a"], "edges": [],
                           "endpoints": ["a", "a"]})
        assert ctx.value.code == "DUPLICATE_NODE"
        assert ctx.value.pointer == "/nodes/1"

    def test_duplicate_edge_id(self):
        with assert_raises(ValidationError) as ctx:
            make(
                ["a", "b"],
                [edge("x", "a", "b", 1), edge("x", "a", "b", 2)],
                ["a", "b"],
            )
        assert ctx.value.code == "DUPLICATE_EDGE_ID"
        assert ctx.value.pointer == "/edges/1/id"

    def test_self_loop_rejected(self):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("e", "a", "a", 1)], ["a", "b"])
        assert ctx.value.code == "SELF_LOOP"
        assert ctx.value.pointer == "/edges/0"

    def check_bad_cost(self, bad, code):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("e", "a", "b", bad)], ["a", "b"])
        assert ctx.value.code == code, (bad, code, ctx.value.code)
        assert ctx.value.pointer == "/edges/0/cost"

    def test_bad_costs(self):
        for bad, code in [
            (0, "NON_POSITIVE_COST"),
            (-4, "NON_POSITIVE_COST"),
            (True, "INVALID_COST"),
            (2.5, "INVALID_COST"),
            ("7", "INVALID_COST"),
            (None, "INVALID_COST"),
        ]:
            self.check_bad_cost(bad, code)

    def test_cost_upper_bound(self):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("e", "a", "b", MAX_COST + 1)],
                 ["a", "b"])
        assert ctx.value.code == "COST_OUT_OF_RANGE"

    def test_cost_at_upper_bound_ok(self):
        p = make(["a", "b"], [edge("e", "a", "b", MAX_COST)], ["a", "b"])
        assert p.edges[0].cost == MAX_COST

    def test_non_ascii_id(self):
        with assert_raises(ValidationError) as ctx:
            make(["a", "é"], [edge("e", "a", "é", 1)],
                 ["a", "é"])
        assert ctx.value.code == "NON_ASCII_ID"

    def test_empty_id(self):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("", "a", "b", 1)], ["a", "b"])
        assert ctx.value.code == "EMPTY_ID"

    def test_unknown_endpoint(self):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("e", "a", "b", 1)], ["a", "z"])
        assert ctx.value.code == "UNKNOWN_ENDPOINT"
        assert ctx.value.pointer == "/endpoints/1"

    def test_unknown_edge_node(self):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("e", "a", "z", 1)], ["a", "b"])
        assert ctx.value.code == "UNKNOWN_NODE"

    def test_duplicate_endpoint(self):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("e", "a", "b", 1)], ["a", "a"])
        assert ctx.value.code == "DUPLICATE_ENDPOINT"

    def test_endpoint_count_bounds(self):
        nodes = [f"n{i}" for i in range(11)]
        with assert_raises(ValidationError) as ctx:
            parse_problem({
                "nodes": nodes,
                "edges": [edge("e0", "n0", "n1", 1)],
                "endpoints": nodes[:11],
            })
        assert ctx.value.code == "ENDPOINT_COUNT_OUT_OF_RANGE"

    def test_edges_count_bounds(self):
        with assert_raises(ValidationError) as ctx:
            parse_problem(
                {"nodes": ["a", "b"], "edges": [], "endpoints": ["a", "b"]}
            )
        assert ctx.value.code == "EDGE_COUNT_OUT_OF_RANGE"

    def test_missing_edge_field(self):
        with assert_raises(ValidationError) as ctx:
            parse_problem({
                "nodes": ["a", "b"],
                "edges": [{"id": "e", "source": "a", "target": "b"}],
                "endpoints": ["a", "b"],
            })
        assert ctx.value.code == "MISSING_FIELD"
        assert ctx.value.pointer == "/edges/0/cost"

    def test_max_edges_defaults_to_none(self):
        p = make(["a", "b"], [edge("e1", "a", "b", 1)], ["a", "b"])
        assert p.max_edges is None

    def check_bad_max_edges(self, bad, code):
        with assert_raises(ValidationError) as ctx:
            make(["a", "b"], [edge("e1", "a", "b", 1)], ["a", "b"],
                 max_edges=bad)
        assert ctx.value.code == code, (bad, ctx.value.code)
        assert ctx.value.pointer == "/max_edges"

    def test_bad_max_edges(self):
        for bad, code in [
            (0, "MAX_EDGES_OUT_OF_RANGE"),
            (-1, "MAX_EDGES_OUT_OF_RANGE"),
            (221, "MAX_EDGES_OUT_OF_RANGE"),
            (True, "INVALID_MAX_EDGES"),
            (2.0, "INVALID_MAX_EDGES"),
            ("3", "INVALID_MAX_EDGES"),
        ]:
            self.check_bad_max_edges(bad, code)
        # An explicit JSON null reaches the field (unlike omission).
        with assert_raises(ValidationError) as ctx:
            parse_problem({
                "nodes": ["a", "b"],
                "edges": [edge("e1", "a", "b", 1)],
                "endpoints": ["a", "b"],
                "max_edges": None,
            })
        assert ctx.value.code == "INVALID_MAX_EDGES"
        assert ctx.value.pointer == "/max_edges"

    def test_max_edges_at_bounds_ok(self):
        p = make(["a", "b"], [edge("e1", "a", "b", 1)], ["a", "b"],
                 max_edges=220)
        assert p.max_edges == 220


# --------------------------------------------------------------------------
# Solver: edge-segment budget (max_edges)
# --------------------------------------------------------------------------

class TestBudget:
    def path_vs_direct(self, L):
        nodes = ["a", "b", "c", "d"]
        edges = [
            edge("p1", "a", "b", 1),
            edge("p2", "b", "c", 1),
            edge("p3", "c", "d", 1),
            edge("dir", "a", "d", 9),
        ]
        return make(nodes, edges, ["a", "d"], max_edges=L)

    def test_unconstrained_prefers_cheap_path(self):
        cost, _, ids = solve(self.path_vs_direct(None))
        assert (cost, ids) == (3, ("p1", "p2", "p3"))

    def test_budget_forces_direct_edge(self):
        cost, selected, ids = solve(self.path_vs_direct(1))
        assert cost == 9
        assert ids == ("dir",)
        assert len(selected) == 1

    def test_budget_equal_to_minimum_tree_size(self):
        cost, _, ids = solve(self.path_vs_direct(3))
        assert (cost, ids) == (3, ("p1", "p2", "p3"))

    def test_large_budget_matches_unconstrained(self):
        cost, _, ids = solve(self.path_vs_direct(59))
        assert (cost, ids) == (3, ("p1", "p2", "p3"))

    def test_budget_infeasible_reports_minimum(self):
        # Three terminals on a chain need at least 2 segments.
        p = make(
            ["a", "b", "c"],
            [edge("e1", "a", "b", 1), edge("e2", "b", "c", 1)],
            ["a", "b", "c"],
            max_edges=1,
        )
        with assert_raises(TopologyError) as ctx:
            solve(p)
        assert ctx.value.code == "BUDGET_INFEASIBLE"
        assert ctx.value.min_edges == 2
        assert ctx.value.pointer == "/max_edges"

    def test_budget_infeasible_minimum_uses_relay_path(self):
        # Terminals a, d are connected only by a 3-edge chain and two cheap
        # branches; the shortest feasible tree has 3 edges.
        nodes = ["a", "b", "c", "d"]
        edges = [
            edge("ab", "a", "b", 100),
            edge("bc", "b", "c", 100),
            edge("cd", "c", "d", 100),
        ]
        p = make(nodes, edges, ["a", "d"], max_edges=2)
        with assert_raises(TopologyError) as ctx:
            solve(p)
        assert ctx.value.code == "BUDGET_INFEASIBLE"
        assert ctx.value.min_edges == 3

    def test_budget_changes_equicost_witness(self):
        # A two-edge relay path and one direct edge cost the same total.
        # Unconstrained lex arbitration picks the relay (ids aaa.. < zzz);
        # a 1-segment budget must instead pick the direct edge -- this cannot
        # be obtained by truncating the unconstrained witness.
        nodes = ["a", "b", "x"]
        edges = [
            edge("aaa", "a", "x", 3),
            edge("bbb", "x", "b", 3),
            edge("zzz", "a", "b", 6),
        ]
        cost_u, _, ids_u = solve(make(nodes, edges, ["a", "b"]))
        assert (cost_u, ids_u) == (6, ("aaa", "bbb"))
        cost_b, selected, ids_b = solve(
            make(nodes, edges, ["a", "b"], max_edges=1)
        )
        assert cost_b == 6
        assert ids_b == ("zzz",)
        assert len(selected) == 1

    def test_budget_tie_arbitration_within_budget(self):
        # Two 1-edge options tie on cost and budget; lex rule still decides.
        nodes = ["a", "b"]
        edges = [edge("zz", "a", "b", 4), edge("aa", "a", "b", 4)]
        cost, _, ids = solve(make(nodes, edges, ["a", "b"], max_edges=1))
        assert (cost, ids) == (4, ("aa",))


# --------------------------------------------------------------------------
# Solver: optimality, relays, tie arbitration, infeasibility
# --------------------------------------------------------------------------

class TestSolver:
    def test_single_edge(self):
        p = make(["a", "b"], [edge("e1", "a", "b", 7)], ["a", "b"])
        cost, selected, ids = solve(p)
        assert cost == 7
        assert ids == ("e1",)
        assert len(selected) == 1

    def test_mst_when_all_nodes_are_endpoints(self):
        nodes = ["a", "b", "c", "d"]
        edges = [
            edge("ab", "a", "b", 1),
            edge("bc", "b", "c", 2),
            edge("cd", "c", "d", 1),
            edge("da", "d", "a", 5),
            edge("ac", "a", "c", 9),
        ]
        p = make(nodes, edges, nodes)
        cost, _, ids = solve(p)
        assert cost == 4
        assert set(ids) == {"ab", "bc", "cd"}

    def test_steiner_relay_cheaper(self):
        nodes = ["a", "b", "c", "h"]
        edges = [
            edge("ha", "h", "a", 1),
            edge("hb", "h", "b", 1),
            edge("hc", "h", "c", 1),
            edge("ab", "a", "b", 3),
            edge("bc", "b", "c", 3),
            edge("ac", "a", "c", 3),
        ]
        p = make(nodes, edges, ["a", "b", "c"])
        cost, _, ids = solve(p)
        assert cost == 3
        assert set(ids) == {"ha", "hb", "hc"}

    def test_parallel_edge_cheapest_chosen(self):
        nodes = ["a", "b"]
        edges = [edge("a-exp", "a", "b", 9), edge("a-cheap", "a", "b", 2)]
        p = make(nodes, edges, ["a", "b"])
        cost, _, ids = solve(p)
        assert cost == 2
        assert ids == ("a-cheap",)

    def test_tie_break_lexicographic_ids(self):
        nodes = ["a", "b", "c"]
        edges = [
            edge("zz-1", "a", "b", 5),
            edge("zz-2", "b", "c", 5),
            edge("aa-1", "a", "c", 5),
            edge("aa-2", "c", "b", 5),
        ]
        p = make(nodes, edges, ["a", "b", "c"])
        cost, _, ids = solve(p)
        assert cost == 10
        assert ids == ("aa-1", "aa-2")

    def test_tie_break_independent_of_request_order(self):
        nodes = ["a", "b", "c"]
        edges_shuffled = [
            edge("aa-1", "a", "c", 5),
            edge("zz-2", "b", "c", 5),
            edge("zz-1", "a", "b", 5),
            edge("aa-2", "c", "b", 5),
        ]
        p = make(nodes, edges_shuffled, ["a", "b", "c"])
        _, _, ids = solve(p)
        assert ids == ("aa-1", "aa-2")

    def test_tie_different_tree_shapes(self):
        nodes = ["t1", "t2", "x", "y"]
        edges = [
            edge("e-x1", "t1", "x", 2),
            edge("e-x2", "t2", "x", 2),
            edge("e-y1", "t1", "y", 2),
            edge("e-y2", "t2", "y", 2),
        ]
        p = make(nodes, edges, ["t1", "t2"])
        _, _, ids = solve(p)
        assert ids == ("e-x1", "e-x2")

    def test_dangling_endpoint(self):
        p = make(["a", "b", "c"], [edge("e", "a", "b", 1)], ["a", "c"])
        with assert_raises(TopologyError) as ctx:
            solve(p)
        assert ctx.value.code == "DANGLING_ENDPOINT"
        assert ctx.value.pointer == "/endpoints/1"

    def test_check_feasibility_passes_when_connected(self):
        p = make(["a", "b"], [edge("e", "a", "b", 1)], ["a", "b"])
        check_feasibility(p)

    def test_endpoints_in_separate_components(self):
        p = make(
            ["a", "b", "c", "d"],
            [edge("e1", "a", "b", 1), edge("e2", "c", "d", 1)],
            ["a", "c"],
        )
        with assert_raises(TopologyError) as ctx:
            solve(p)
        assert ctx.value.code == "ENDPOINTS_UNCONNECTED"
        assert ctx.value.components == [["a"], ["c"]]
        assert ctx.value.pointer == "/endpoints/1"

    def test_isolated_non_terminal_is_fine(self):
        p = make(
            ["a", "b", "loner"], [edge("e", "a", "b", 3)], ["a", "b"]
        )
        cost, _, ids = solve(p)
        assert cost == 3 and ids == ("e",)

    def test_adjacency_export(self):
        p = make(
            ["a", "b", "h"],
            [edge("e1", "a", "h", 2), edge("e2", "h", "b", 4)],
            ["a", "b"],
        )
        _, selected, _ = solve(p)
        adj = build_adjacency(p.nodes, selected)
        assert set(adj) == {"a", "b", "h"}
        assert adj["a"] == [{"to": "h", "edge": "e1", "cost": 2}]
        assert adj["b"] == [{"to": "h", "edge": "e2", "cost": 4}]
        assert adj["h"] == [
            {"to": "a", "edge": "e1", "cost": 2},
            {"to": "b", "edge": "e2", "cost": 4},
        ]

    def test_witness_cost_matches_edges(self):
        # The exported edges must sum exactly to the reported cost.
        nodes = [f"n{i}" for i in range(6)]
        edges = [
            edge("e0", "n0", "n1", 4),
            edge("e1", "n1", "n2", 3),
            edge("e2", "n2", "n3", 2),
            edge("e3", "n3", "n4", 5),
            edge("e4", "n4", "n5", 1),
            edge("e5", "n1", "n4", 20),
        ]
        p = make(nodes, edges, ["n0", "n3", "n5"])
        cost, selected, _ = solve(p)
        assert sum(e.cost for e in selected) == cost
