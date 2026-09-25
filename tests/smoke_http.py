"""HTTP smoke tests for POST /api/audit and GET /healthz.

Zero third-party dependencies (urllib only). Covers:
  * health probe,
  * a successful minimum-subnet solve with response consistency,
  * canonical tie arbitration (lexicographic edge ids, request-order stable),
  * infeasibility boundaries: dangling endpoint, split components,
  * stable locatable 4xx errors: malformed JSON, non-positive cost,
    self loop, duplicate edge id, unknown node,
  * request isolation: a failing audit must not bleed into the next one,
  * segment budgets: constrained optimum with echoed edge_count, non-binding
    budgets, tie arbitration under budget, budget-insufficient 422 with the
    minimum feasible count, max_edges field validation, and byte-level
    compatibility of legacy (unbudgeted) requests.

Exits non-zero on the first failed check, printing a readable report.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from typing import Any

BASE_URL = os.environ.get("AUDIT_BASE_URL", "http://127.0.0.1:8080")

_failures: list[str] = []


def check(condition: bool, label: str, detail: str = "") -> None:
    if condition:
        print(f"  pass  {label}")
    else:
        _failures.append(f"{label} {detail}".strip())
        print(f"  FAIL  {label} {detail}")


def request(method: str, path: str, body: Any = None,
            raw: bytes | None = None) -> tuple[int, Any]:
    url = BASE_URL + path
    if raw is not None:
        data = raw
        headers = {"Content-Type": "application/json"}
    elif body is not None:
        data = json.dumps(body).encode("utf-8")
        headers = {"Content-Type": "application/json"}
    else:
        data = None
        headers = {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            payload = resp.read()
            status = resp.status
    except urllib.error.HTTPError as exc:
        payload = exc.read()
        status = exc.code
    try:
        return status, json.loads(payload.decode("utf-8"))
    except Exception:
        return status, payload


def wait_for_health(timeout: float = 30.0) -> None:
    deadline = time.time() + timeout
    last: Exception | None = None
    while time.time() < deadline:
        try:
            status, body = request("GET", "/healthz")
            if status == 200 and isinstance(body, dict) and body.get("status") == "ok":
                print("healthz ok")
                return
        except Exception as exc:  # noqa: BLE001 - service may still start
            last = exc
        time.sleep(0.4)
    raise RuntimeError(f"service did not become healthy: {last}")


def edge(id_, s, t, c):
    return {"id": id_, "source": s, "target": t, "cost": c}


def main() -> int:
    print(f"smoke target: {BASE_URL}")
    wait_for_health()

    # --- 1. successful solve with a non-terminal relay -------------------
    print("[1] minimum subnet via non-terminal relay")
    payload = {
        "nodes": ["A", "B", "C", "D", "R"],
        "edges": [
            edge("e1", "A", "R", 5),
            edge("e2", "B", "R", 5),
            edge("e3", "C", "R", 5),
            edge("e4", "D", "R", 5),
            edge("direct-ab", "A", "B", 9),
            edge("relay-cd", "C", "D", 1),
        ],
        "endpoints": ["A", "B", "C", "D"],
    }
    status, body = request("POST", "/api/audit", payload)
    check(status == 200, "status 200", str(body))
    if status == 200:
        check(body["cost"] == 16, "optimal cost 16", f"got {body.get('cost')}")
        check(
            body["edge_set"] == sorted(body["edge_set"]),
            "edge_set ascending",
        )
        check(
            set(body["edge_set"]) == {"e1", "e2", "e3", "relay-cd"},
            "canonical edge set",
            str(body["edge_set"]),
        )
        ids_in_edges = {e["id"] for e in body["edges"]}
        check(
            ids_in_edges == set(body["edge_set"]),
            "edges list matches edge_set",
        )
        total = sum(e["cost"] for e in body["edges"])
        check(total == body["cost"], "edge costs sum to cost",
              f"{total} != {body['cost']}")
        adj = body["adjacency"]
        # Adjacency is induced exactly by the chosen set: symmetric & complete.
        symmetric = all(
            any(a["edge"] == e["id"] and a["to"] == other
                for a in adj.get(self_, []))
            for e in body["edges"]
            for self_, other in (
                (e["source"], e["target"]),
                (e["target"], e["source"]),
            )
        )
        check(symmetric, "adjacency induced and symmetric")
        check(
            not any(key in body for key in ("error", "partial")),
            "no error/partial fields on success",
        )

    # --- 2. canonical tie arbitration ------------------------------------
    print("[2] tie arbitration -> lexicographically smallest witness")
    tie_payload = {
        "nodes": ["a", "b", "c"],
        "edges": [
            edge("zz-1", "a", "b", 5),
            edge("zz-2", "b", "c", 5),
            edge("aa-1", "a", "c", 5),
            edge("aa-2", "c", "b", 5),
        ],
        "endpoints": ["a", "b", "c"],
    }
    status, body = request("POST", "/api/audit", tie_payload)
    check(status == 200, "tie status 200", str(body))
    check(body.get("cost") == 10, "tie cost 10", str(body.get("cost")))
    check(
        body.get("edge_set") == ["aa-1", "aa-2"],
        "lexicographic witness",
        str(body.get("edge_set")),
    )
    # Same problem, edges supplied in a different array order.
    tie_payload["edges"] = [tie_payload["edges"][i] for i in (2, 1, 0, 3)]
    status2, body2 = request("POST", "/api/audit", tie_payload)
    check(
        status2 == 200 and body2.get("edge_set") == ["aa-1", "aa-2"],
        "witness stable under request edge reorder",
        str(body2),
    )

    # --- 3. infeasibility boundaries -------------------------------------
    print("[3] no-solution boundaries")
    status, body = request("POST", "/api/audit", {
        "nodes": ["A", "B", "C"],
        "edges": [edge("e1", "A", "B", 1)],
        "endpoints": ["A", "C"],
    })
    check(status == 422, "dangling endpoint -> 422", f"got {status}")
    check(body.get("code") == "DANGLING_ENDPOINT", "stable error code",
          str(body))
    check(body.get("pointer") == "/endpoints/1", "locatable pointer",
          str(body.get("pointer")))
    check("edge_set" not in body and "adjacency" not in body,
          "no partial subnet on failure")

    status, body = request("POST", "/api/audit", {
        "nodes": ["A", "B", "C", "D"],
        "edges": [edge("e1", "A", "B", 1), edge("e2", "C", "D", 1)],
        "endpoints": ["A", "C"],
    })
    check(status == 422, "split components -> 422", f"got {status}")
    check(body.get("code") == "ENDPOINTS_UNCONNECTED", "unconnected code",
          str(body))
    check(body.get("components") == [["A"], ["C"]], "components reported",
          str(body.get("components")))
    check(body.get("pointer") == "/endpoints/1", "split pointer locatable",
          str(body.get("pointer")))

    # --- 4. stable locatable 4xx errors ----------------------------------
    print("[4] stable validation errors")

    status, body = request("POST", "/api/audit", raw=b"{not json")
    check(status == 400 and body.get("code") == "MALFORMED_JSON",
          "malformed JSON -> 400", str(body))

    status, body = request("POST", "/api/audit", {
        "nodes": ["A", "B"],
        "edges": [edge("e1", "A", "B", 0)],
        "endpoints": ["A", "B"],
    })
    check(status == 400 and body.get("code") == "NON_POSITIVE_COST",
          "non-positive cost -> 400", str(body))
    check(body.get("pointer") == "/edges/0/cost", "cost pointer",
          str(body.get("pointer")))

    status, body = request("POST", "/api/audit", {
        "nodes": ["A", "B"],
        "edges": [edge("e1", "A", "A", 3)],
        "endpoints": ["A", "B"],
    })
    check(status == 400 and body.get("code") == "SELF_LOOP",
          "self loop -> 400", str(body))

    status, body = request("POST", "/api/audit", {
        "nodes": ["A", "B"],
        "edges": [edge("dup", "A", "B", 1), edge("dup", "A", "B", 2)],
        "endpoints": ["A", "B"],
    })
    check(status == 400 and body.get("code") == "DUPLICATE_EDGE_ID",
          "duplicate edge id -> 400", str(body))

    status, body = request("POST", "/api/audit", {
        "nodes": ["A", "B"],
        "edges": [edge("e1", "A", "Z", 1)],
        "endpoints": ["A", "B"],
    })
    check(status == 400 and body.get("code") == "UNKNOWN_NODE",
          "unknown edge node -> 400", str(body))

    status, body = request("GET", "/api/audit")
    check(status == 405, "GET audit -> 405", f"got {status}")

    status, body = request("GET", "/no-such-path")
    check(status == 404, "unknown path -> 404", f"got {status}")

    # --- 5. no state leaks across requests --------------------------------
    print("[5] request isolation")
    bad = {
        "nodes": ["A", "B"], "edges": [edge("e1", "A", "B", -3)],
        "endpoints": ["A", "B"],
    }
    good = {
        "nodes": ["A", "B"], "edges": [edge("e1", "A", "B", 11)],
        "endpoints": ["A", "B"],
    }
    s1, b1 = request("POST", "/api/audit", bad)
    s2, b2 = request("POST", "/api/audit", good)
    s3, b3 = request("POST", "/api/audit", good)
    check(s1 == 400, "bad request rejected", str(s1))
    check(s2 == 200 and b2["cost"] == 11 and b2["edge_set"] == ["e1"],
          "good request after bad is fresh", str(b2))
    check(s3 == 200 and b3 == b2, "identical requests -> identical results",
          f"{b2} != {b3}")

    # --- 6. segment budget: constrained optimum & legacy compat -----------
    print("[6] max_edges constrained optimum / legacy compatibility")
    star_vs_path = {
        "nodes": ["A", "B", "C", "R"],
        "edges": [
            edge("e1", "A", "R", 5),
            edge("e2", "B", "R", 5),
            edge("e3", "C", "R", 5),
            edge("p1", "A", "B", 9),
            edge("p2", "B", "C", 9),
        ],
        "endpoints": ["A", "B", "C"],
    }
    # Legacy request (no max_edges): the 3-segment star wins at cost 15 and
    # the response carries no edge_count field at all.
    status, legacy = request("POST", "/api/audit", star_vs_path)
    check(status == 200, "legacy status 200", str(legacy))
    check(legacy.get("cost") == 15, "legacy cost 15", str(legacy.get("cost")))
    check(legacy.get("edge_set") == ["e1", "e2", "e3"],
          "legacy edge set", str(legacy.get("edge_set")))
    check("edge_count" not in legacy,
          "legacy response has no edge_count field", str(legacy))

    # Non-binding budget: identical subnet, actual segment count echoed.
    status, body = request(
        "POST", "/api/audit", {**star_vs_path, "max_edges": 220})
    check(status == 200, "non-binding status 200", str(body))
    check(
        status == 200
        and body.get("cost") == legacy["cost"]
        and body.get("edge_set") == legacy["edge_set"]
        and body.get("edges") == legacy["edges"]
        and body.get("adjacency") == legacy["adjacency"],
        "non-binding budget matches legacy item by item",
        str(body),
    )
    check(body.get("edge_count") == 3, "edge_count echoes 3", str(body))

    # Binding budget: the 2-segment path wins although it costs more.
    status, body = request(
        "POST", "/api/audit", {**star_vs_path, "max_edges": 2})
    check(status == 200, "budgeted status 200", str(body))
    check(body.get("cost") == 18, "budgeted cost 18", str(body.get("cost")))
    check(body.get("edge_set") == ["p1", "p2"],
          "budgeted edge set", str(body.get("edge_set")))
    check(body.get("edge_count") == 2, "edge_count echoes 2", str(body))
    if status == 200:
        check(len(body["edges"]) == 2 and
              sum(e["cost"] for e in body["edges"]) == 18,
              "budgeted edges consistent", str(body["edges"]))
        check(set(body["adjacency"]) == {"A", "B", "C"},
              "budgeted adjacency skips unused relay",
              str(body.get("adjacency")))

    # Tie arbitration still yields one canonical witness under the budget.
    tie_budget = {
        "nodes": ["a", "b", "c", "r"],
        "edges": [
            edge("q1", "a", "r", 1),
            edge("q2", "r", "b", 1),
            edge("q3", "r", "c", 1),
            edge("zz", "a", "c", 3),
            edge("aa", "a", "b", 3),
            edge("bb", "b", "c", 3),
        ],
        "endpoints": ["a", "b", "c"],
        "max_edges": 2,
    }
    status, body = request("POST", "/api/audit", tie_budget)
    check(status == 200, "budgeted tie status 200", str(body))
    check(body.get("cost") == 6, "budgeted tie cost 6", str(body.get("cost")))
    check(body.get("edge_set") == ["aa", "bb"],
          "budgeted lexicographic witness", str(body.get("edge_set")))
    check(body.get("edge_count") == 2, "budgeted tie edge_count", str(body))

    # --- 7. budget below every feasible subnet -----------------------------
    print("[7] max_edges below the minimum feasible segment count")
    over_budget = {
        "nodes": ["A", "B", "C"],
        "edges": [edge("ab", "A", "B", 1), edge("bc", "B", "C", 1)],
        "endpoints": ["A", "C"],
        "max_edges": 1,
    }
    status, body = request("POST", "/api/audit", over_budget)
    check(status == 422, "over budget -> 422", f"got {status}")
    check(body.get("code") == "EDGE_BUDGET_EXCEEDED",
          "stable over-budget code", str(body))
    check(body.get("min_edges") == 2,
          "minimum feasible segment count reported", str(body))
    check(body.get("pointer") == "/max_edges",
          "over-budget pointer locatable", str(body.get("pointer")))
    check("edge_set" not in body and "adjacency" not in body
          and "edges" not in body,
          "no partial subnet on over-budget", str(body))
    # The same instance without the budget still solves normally.
    del over_budget["max_edges"]
    status, body = request("POST", "/api/audit", over_budget)
    check(status == 200 and body.get("cost") == 2
          and body.get("edge_set") == ["ab", "bc"],
          "unbudgeted twin solves fine", str(body))

    # --- 8. max_edges field validation --------------------------------------
    print("[8] max_edges validation errors")
    valid = {
        "nodes": ["A", "B"],
        "edges": [edge("e1", "A", "B", 1)],
        "endpoints": ["A", "B"],
    }
    for bad, code in [
        (0, "NON_POSITIVE_MAX_EDGES"),
        (-4, "NON_POSITIVE_MAX_EDGES"),
        ("2", "INVALID_MAX_EDGES"),
        (True, "INVALID_MAX_EDGES"),
        (2.5, "INVALID_MAX_EDGES"),
        (None, "INVALID_MAX_EDGES"),
        (221, "MAX_EDGES_OUT_OF_RANGE"),
    ]:
        status, body = request(
            "POST", "/api/audit", {**valid, "max_edges": bad})
        check(status == 400 and body.get("code") == code,
              f"max_edges={bad!r} -> {code}", f"{status} {body}")
        check(body.get("pointer") == "/max_edges",
              f"max_edges={bad!r} pointer", str(body.get("pointer")))

    print("-" * 60)
    if _failures:
        print(f"smoke tests: {len(_failures)} FAILED")
        for f in _failures:
            print("  -", f)
        return 1
    print("smoke tests: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
