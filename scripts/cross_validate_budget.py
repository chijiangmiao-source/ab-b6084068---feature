#!/usr/bin/env python3
"""Brute-force cross-validation for the edge-budgeted Steiner core.

Offline test harness (never used by the service; the service itself never
enumerates edge sets). On small random graphs with a random segment budget
it enumerates every edge subset, derives the true constrained optimum, the
lexicographically smallest sorted witness and the minimum feasible segment
count, and compares all of them with the output of core/steiner.

Also checks the non-binding boundary: a budget of at least n-1 must produce
exactly the unconstrained answer.
"""

from __future__ import annotations

import random
import subprocess
import sys
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "core" / "steiner"


def brute_force_budget(n, edges, terminals, budget):
    """Return ((cost, tuple(sorted indices)) or None, min_edges or None)."""
    best = None
    min_edges = None
    m = len(edges)
    for bits in range(1 << m):
        chosen = [j for j in range(m) if bits >> j & 1]
        parent = list(range(n))

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        cost = 0
        for j in chosen:
            u, v, w = edges[j]
            cost += w
            ru, rv = find(u), find(v)
            if ru != rv:
                parent[ru] = rv
        root = find(terminals[0])
        if all(find(t) == root for t in terminals):
            if min_edges is None or len(chosen) < min_edges:
                min_edges = len(chosen)
            if len(chosen) <= budget:
                key = (cost, tuple(sorted(chosen)))
                if best is None or key < best:
                    best = key
    return best, min_edges


def run_core_budget(n, edges, terminals, budget):
    """Return (status, cost, idx, min_edges) from the native core."""
    lines = [f"{n} {len(edges)} {len(terminals)} {budget}",
             " ".join(str(t) for t in terminals)]
    for u, v, w in edges:
        lines.append(f"{u} {v} {w}")
    proc = subprocess.run(
        [str(BINARY)], input="\n".join(lines) + "\n",
        capture_output=True, text=True, timeout=30,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"core failed: {proc.stderr}")
    out = proc.stdout.split()
    status = out[0]
    if status == "OK":
        cost = int(out[1])
        count = int(out[2])
        idx = tuple(int(x) for x in out[3:3 + count])
        assert len(idx) == count and idx == tuple(sorted(idx))
        return status, cost, idx, None
    if status == "OVER_BUDGET":
        return status, None, None, int(out[1])
    assert status == "UNCONNECTED", out
    return status, None, None, None


def random_instance(rng):
    n = rng.randint(2, 8)
    max_edges = n * (n - 1) // 2
    m = rng.randint(1, max(1, min(max_edges, rng.randint(1, 12))))
    pairs = [(u, v) for u in range(n) for v in range(u + 1, n)]
    rng.shuffle(pairs)
    edges = []
    # Possibly emit parallel edges (duplicate the same pair under a new id).
    for u, v in pairs[:m]:
        edges.append((u, v, rng.randint(1, 6)))
        if rng.random() < 0.2 and len(edges) < 13:
            edges.append((u, v, rng.randint(1, 6)))
    k = rng.randint(2, min(n, 6))
    terminals = sorted(rng.sample(range(n), k))
    return n, edges, terminals


def main():
    rng = random.Random(20260925)
    trials = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    stats = {"OK": 0, "OVER_BUDGET": 0, "UNCONNECTED": 0}
    for trial in range(trials):
        n, edges, terminals = random_instance(rng)
        budget = rng.randint(1, n + 2)  # binding, non-binding and infeasible
        (expect, min_edges) = brute_force_budget(n, edges, terminals, budget)
        status, cost, idx, core_min = run_core_budget(
            n, edges, terminals, budget
        )
        stats[status] += 1

        if expect is None:
            # Nothing fits the budget: UNCONNECTED iff unreachable at all,
            # otherwise OVER_BUDGET carrying the minimum feasible count.
            ok = (
                (min_edges is None and status == "UNCONNECTED")
                or (min_edges is not None and status == "OVER_BUDGET"
                    and core_min == min_edges)
            )
        else:
            ok = status == "OK" and (cost, idx) == expect
            # The witness must respect the budget by construction.
            ok = ok and len(idx) <= budget

        # A budget of at least n-1 can never bind: identical to unbounded.
        if budget >= n - 1 and expect is not None:
            u_status, u_cost, u_idx, _ = run_core_budget(
                n, edges, terminals, 0
            )
            ok = ok and u_status == "OK" and (u_cost, u_idx) == expect

        if not ok:
            print("MISMATCH on trial", trial)
            print("n=", n, "budget=", budget)
            print("edges (u,v,w):", edges)
            print("terminals:", terminals)
            print("expected:", expect, "min_edges:", min_edges)
            print("core    :", status, cost, idx, core_min)
            sys.exit(1)
        if trial % 500 == 0:
            print(f"...{trial} trials ok {stats}")
    print(f"ALL {trials} BUDGETED TRIALS OK {stats}")


if __name__ == "__main__":
    main()
