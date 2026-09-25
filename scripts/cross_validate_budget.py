#!/usr/bin/env python3
"""Brute-force cross-validation for the max_edges semantics in core/steiner.

Offline test harness only (never used by the service). On thousands of small
random graphs it enumerates every edge subset and compares, for every segment
budget from 1..n plus the unconstrained header, the kernel against brute force:

  * constrained optimum cost,
  * lexicographically smallest sorted edge-id witness under the budget
    (a budget can force a different witness than the unconstrained answer;
    truncation of the unconstrained witness cannot produce it),
  * BUDGET_INFEASIBLE with the true minimum feasible segment count,
  * byte-compatible UNCONNECTED / OK behaviour without a budget field.

Run: python3 scripts/cross_validate_budget.py [trials]
"""

from __future__ import annotations

import random
import subprocess
import sys
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "core" / "steiner"


def brute(n, edges, terms, budget):
    """Enumerate every edge subset.

    Returns (best_within_budget, min_edge_count_overall) where best is the
    lexicographically smallest (cost, sorted indices) connecting subset
    satisfying the budget (None if none does), and min_edge_count is the
    shortest connecting subset over ALL costs (None when disconnected).
    """
    m = len(edges)
    best = None
    min_edges = None
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
        root = find(terms[0])
        if not all(find(t) == root for t in terms):
            continue
        key = (cost, tuple(sorted(chosen)))
        count = len(chosen)
        if min_edges is None or count < min_edges:
            min_edges = count
        if budget is None or count <= budget:
            if best is None or key < best:
                best = key
    return best, min_edges


def run_core(n, edges, terms, budget):
    header = f"{n} {len(edges)} {len(terms)}"
    if budget is not None:
        header += f" {budget}"
    lines = [header, " ".join(str(t) for t in terms)]
    for u, v, w in edges:
        lines.append(f"{u} {v} {w}")
    proc = subprocess.run(
        [str(BINARY)], input="\n".join(lines) + "\n",
        capture_output=True, text=True, timeout=60,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"core failed: {proc.stderr}")
    out = proc.stdout.split()
    if out[0] == "UNCONNECTED":
        return "UNCONNECTED", None
    if out[0] == "BUDGET_INFEASIBLE":
        return "BUDGET_INFEASIBLE", int(out[1])
    assert out[0] == "OK", out
    cost, count = int(out[1]), int(out[2])
    idx = tuple(int(x) for x in out[3:3 + count])
    assert len(idx) == count and idx == tuple(sorted(idx))
    return "OK", (cost, idx)


def random_instance(rng):
    n = rng.randint(2, 7)
    pairs = [(u, v) for u in range(n) for v in range(u + 1, n)]
    rng.shuffle(pairs)
    m = rng.randint(1, min(len(pairs), 10))
    edges = [(u, v, rng.randint(1, 6)) for u, v in pairs[:m]]
    # Occasionally add parallel edges under distinct ids.
    for u, v, _w in list(edges):
        if rng.random() < 0.15 and len(edges) < 12:
            edges.append((u, v, rng.randint(1, 6)))
    k = rng.randint(2, min(n, 5))
    terms = sorted(rng.sample(range(n), k))
    return n, edges, terms


def main():
    trials = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    rng = random.Random(20260925)
    for trial in range(trials):
        n, edges, terms = random_instance(rng)
        for budget in [None] + list(range(1, n + 1)):
            best, min_edges = brute(n, edges, terms, budget)
            status, detail = run_core(n, edges, terms, budget)
            if min_edges is None:
                assert status == "UNCONNECTED", \
                    (n, edges, terms, budget, status, detail)
            elif budget is not None and budget < min_edges:
                assert (status, detail) == ("BUDGET_INFEASIBLE", min_edges), \
                    (n, edges, terms, budget, status, detail, min_edges)
            else:
                assert (status, detail) == ("OK", best), \
                    (n, edges, terms, budget, status, detail, best, min_edges)
        if trial % 250 == 0:
            print(f"...{trial} trials ok")
    print(f"ALL {trials} TRIALS OK "
          f"(each probed unconstrained and with budgets 1..{n})")


if __name__ == "__main__":
    main()
