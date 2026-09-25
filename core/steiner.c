/*
 * steiner.c -- minimum Steiner tree core with canonical tie breaking and an
 * optional segment (edge count) budget.
 *
 * Implemented from scratch (no generic optimiser, no edge-set enumeration):
 *   - terminal-subset dynamic programming (Dreyfus-Wagner recurrence),
 *   - node aggregation merge (disjoint-set union, also used to contract
 *     mandatory edges during witness reconstruction),
 *   - multi-source shortest-path closure (one multi-source Dijkstra per
 *     terminal subset, every vertex seeded at once).
 *
 * Segment budget: when a positive budget B_E is supplied, the segment count
 * is part of the DP state -- cur[v][e] is the cheapest tree connecting the
 * terminal subset, touching v and using AT MOST e edges -- and the
 * multi-source shortest-path closure runs on the layered graph whose nodes
 * are (vertex, segments-used) pairs, every arc consuming one segment. The
 * constrained optimum is computed directly by this DP; the unconstrained
 * answer is never truncated afterwards, and no edge sets are enumerated.
 * Merge steps convolve only Pareto frontiers (record lows of the
 * at-most-e cost curve): a dominated label (more segments, no cheaper) can
 * never improve a supertree. A budget of at least n-1 can never bind on any
 * tree, so that case takes the plain unconstrained path unchanged; likewise,
 * when the unconstrained canonical witness already fits the budget it IS the
 * constrained canonical witness (same optimum cost, lexicographically
 * smallest overall), so the budgeted DP only runs when the budget genuinely
 * binds. Nothing is ever truncated to fit the budget.
 *
 * Canonical witness: after the optimal cost B is known, the lexicographically
 * smallest sorted edge-id list among all B-cost witnesses that respect the
 * budget is built by a greedy prefix scan over edges in canonical (input)
 * order. For each candidate edge the solver asks whether a budget-feasible
 * optimal tree exists that (a) contains every edge already required,
 * (b) avoids every edge already rejected, and (c) contains the candidate.
 * That oracle contracts the required edges through DSU node aggregation and
 * runs one (budgeted) Steiner DP on the contracted multigraph, charging the
 * contracted segments against the remaining budget. Keeping only the single
 * best (cost, witness) pair per DP state would not be sound for
 * lexicographic minimisation, so cost DP and witness reconstruction are
 * deliberately separate phases.
 *
 * Infeasibility under budget: if every terminal is reachable in the original
 * graph yet no tree fits the budget, the core reports OVER_BUDGET together
 * with the minimum feasible segment count (a unit-weight run of the same
 * terminal-subset DP minimises exactly the tree size). No partial witness is
 * ever emitted.
 *
 * Input (stdin, text):
 *   line 1: n m k [budget]   (budget omitted or 0 = unconstrained)
 *   line 2: k terminal vertex indices
 *   next m lines: u v cost      (edge input order = canonical edge-id order)
 *
 * Output (stdout):
 *   OK
 *   <cost>
 *   <number of selected edge indices>   (<= budget when one was supplied)
 *   <zero-based edge indices in input order, one per line>
 * or:
 *   OVER_BUDGET
 *   <minimum feasible segment count>
 * or:
 *   UNCONNECTED
 * or:
 *   OVERFLOW
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_N 64
#define MAX_M 256
#define MAX_K 10
#define MAX_STATES (1 << MAX_K)
/* A tree on at most MAX_N vertices never needs more than MAX_N-1 edges. */
#define MAX_E (MAX_N - 1)
/* Saturated infinity; every reachable answer is strictly smaller. */
#define INF (LLONG_MAX / 4)

typedef long long ll;

static int n, m, k;
static int eu[MAX_M], ev[MAX_M];
static ll ew[MAX_M];
static int term[MAX_K];

static ll dp[MAX_STATES][MAX_N];

/* Witness under construction by the greedy prefix scan (global so the scan
 * can be re-run under different oracles within one process). */
static char required[MAX_M];

/* Pareto frontiers of the budgeted DP, per (terminal subset, vertex):
 * edge counts whose cost strictly improves on every smaller count. The
 * at-most-e cost curve is non-increasing in e, so only these record lows
 * can ever participate in an optimal merge. */
static ll front_cost[MAX_STATES][MAX_N][MAX_E + 1];
static short front_edges[MAX_STATES][MAX_N][MAX_E + 1];
static int front_len[MAX_STATES][MAX_N];

/* Forward-star graph rebuilt for every feasibility oracle. */
typedef struct {
    int to;
    int next;
    ll w;
} Arc;
static Arc arcs[2 * MAX_M];
static int first[MAX_N];
static int arc_count;

static void add_arc(int u, int v, ll w) {
    arcs[arc_count].to = v;
    arcs[arc_count].w = w;
    arcs[arc_count].next = first[u];
    first[u] = arc_count++;
}

/* Rebuild the forward-star graph for the full, uncontracted input (the
 * feasibility oracle rewrites `arcs` for its contracted multigraphs, so
 * every top-level DP must restore the full graph first). `unit` selects
 * unit weights (minimum-segment-count runs) over real costs. */
static void build_full_arcs(int unit) {
    memset(first, -1, sizeof(first));
    arc_count = 0;
    for (int j = 0; j < m; j++) {
        ll w = unit ? 1 : ew[j];
        add_arc(eu[j], ev[j], w);
        add_arc(ev[j], eu[j], w);
    }
}

static ll add_sat(ll a, ll b, int *overflow) {
    if (a >= INF || b >= INF || a > INF - b) {
        *overflow = 1;
        return INF;
    }
    return a + b;
}

/* ---- disjoint-set union (node aggregation) ---------------------------- */

static int dsu_parent[MAX_N];
static int dsu_rankk[MAX_N];

/* Independent DSU tracking the greedy scan's required-edge forest; the
 * feasibility oracle mutates dsu_parent/dsu_rankk via dsu_reset(), so the two
 * must not share storage. */
static int scan_parent[MAX_N];
static int scan_rankk[MAX_N];

static void dsu_reset(int count) {
    for (int i = 0; i < count; i++) {
        dsu_parent[i] = i;
        dsu_rankk[i] = 0;
    }
}

static int dsu_find(int x) {
    while (dsu_parent[x] != x) {
        dsu_parent[x] = dsu_parent[dsu_parent[x]];
        x = dsu_parent[x];
    }
    return x;
}

static void dsu_union(int a, int b) {
    int ra = dsu_find(a), rb = dsu_find(b);
    if (ra == rb) return;
    if (dsu_rankk[ra] < dsu_rankk[rb]) {
        int t = ra; ra = rb; rb = t;
    }
    dsu_parent[rb] = ra;
    if (dsu_rankk[ra] == dsu_rankk[rb]) dsu_rankk[ra]++;
}

static int scan_find(int x) {
    while (scan_parent[x] != x) {
        scan_parent[x] = scan_parent[scan_parent[x]];
        x = scan_parent[x];
    }
    return x;
}

static void scan_union(int a, int b) {
    int ra = scan_find(a), rb = scan_find(b);
    if (ra == rb) return;
    if (scan_rankk[ra] < scan_rankk[rb]) {
        int t = ra; ra = rb; rb = t;
    }
    scan_parent[rb] = ra;
    if (scan_rankk[ra] == scan_rankk[rb]) scan_rankk[ra]++;
}

/* ---- binary heap for multi-source Dijkstra ---------------------------- */

typedef struct { ll d; int v; } HeapItem;

/* Dijkstra performs O(V*E) successful relaxations per closure even on the
 * largest instance (the budgeted closure runs on V*(budget+1) layered
 * states); a generous power-of-two arena keeps the static heap safe with a
 * hard guard in heap_push(). */
#define HEAP_CAP (1 << 20)
static HeapItem heap[HEAP_CAP];
static int heap_size;

static void heap_push(ll d, int v) {
    if (heap_size >= HEAP_CAP) {
        fprintf(stderr, "search heap exhausted\n");
        exit(4);
    }
    int i = heap_size++;
    heap[i].d = d;
    heap[i].v = v;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].d <= heap[i].d) break;
        HeapItem t = heap[p]; heap[p] = heap[i]; heap[i] = t;
        i = p;
    }
}

static HeapItem heap_pop(void) {
    HeapItem top = heap[0];
    heap[0] = heap[--heap_size];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, best = i;
        if (l < heap_size && heap[l].d < heap[best].d) best = l;
        if (r < heap_size && heap[r].d < heap[best].d) best = r;
        if (best == i) break;
        HeapItem t = heap[best]; heap[best] = heap[i]; heap[i] = t;
        i = best;
    }
    return top;
}

/*
 * Terminal-subset DP on the graph currently held in `arcs`.
 * Fills dp[mask][v] for the given `cn` vertices and returns
 * min_v dp[all][v]. Every call overwrites all rows it reads.
 */
static ll steiner_dp(int cn, int ck, const int *cterms, int *overflow) {
    int cstates = 1 << ck;
    int full = cstates - 1;

    for (int s = 0; s < cstates; s++)
        for (int v = 0; v < cn; v++) dp[s][v] = INF;
    for (int i = 0; i < ck; i++)
        dp[1 << i][cterms[i]] = 0;

    ll merged[MAX_N];

    for (int mask = 1; mask <= full; mask++) {
        int bits = 0;
        for (int x = mask; x; x &= x - 1) bits++;

        if (bits == 1) {
            for (int v = 0; v < cn; v++) merged[v] = dp[mask][v];
        } else {
            for (int v = 0; v < cn; v++) merged[v] = INF;
            int anchor = mask & -mask;
            /* Node aggregation merge: join two terminal subtrees at one
             * vertex; requiring the anchor bit visits each bipartition once. */
            for (int sub = (mask - 1) & mask; sub; sub = (sub - 1) & mask) {
                if (!(sub & anchor)) continue;
                int other = mask ^ sub;
                for (int v = 0; v < cn; v++) {
                    ll a = dp[sub][v], b = dp[other][v];
                    if (a >= INF || b >= INF) continue;
                    ll val = add_sat(a, b, overflow);
                    if (val < merged[v]) merged[v] = val;
                }
            }
        }

        /* Multi-source shortest-path closure: all vertices are seeded with
         * their merge label at the same time. */
        heap_size = 0;
        for (int v = 0; v < cn; v++) {
            dp[mask][v] = merged[v];
            if (merged[v] < INF) heap_push(merged[v], v);
        }
        while (heap_size > 0) {
            HeapItem top = heap_pop();
            if (top.d != dp[mask][top.v]) continue;  /* stale label */
            for (int a = first[top.v]; a != -1; a = arcs[a].next) {
                int w = arcs[a].to;
                int ovf = 0;
                ll nd = add_sat(top.d, arcs[a].w, &ovf);
                if (ovf) { *overflow = 1; continue; }
                if (nd < dp[mask][w]) {
                    dp[mask][w] = nd;
                    heap_push(nd, w);
                }
            }
        }
    }

    ll best = INF;
    for (int v = 0; v < cn; v++)
        if (dp[full][v] < best) best = dp[full][v];
    return best;
}

/*
 * Edge-budgeted terminal-subset DP on the graph currently held in `arcs`.
 *
 * The segment count is part of the state: cur[v][e] is the minimum cost of
 * a tree that connects the current terminal subset, touches v and uses at
 * most e edges. The multi-source shortest-path closure runs on the layered
 * (vertex, segments-used) graph where every arc consumes exactly one
 * segment. After each closure the labels are distilled into Pareto
 * frontiers; merges convolve only frontier pairs.
 *
 * `cap` prunes every label above it (the feasibility oracle only asks
 * "is a tree of cost <= cap within the remaining segment budget?").
 * Returns the minimum cost using at most E segments, INF if infeasible.
 */
static ll steiner_dp_budgeted(int cn, int ck, const int *cterms, int E,
                              ll cap, int *overflow) {
    static ll cur[MAX_N][MAX_E + 1];
    static ll merged[MAX_N][MAX_E + 1];
    int cstates = 1 << ck;
    int full = cstates - 1;
    if (E > MAX_E) E = MAX_E;  /* defensive; callers already clamp */

    for (int mask = 1; mask <= full; mask++) {
        int bits = 0;
        for (int x = mask; x; x &= x - 1) bits++;

        if (bits == 1) {
            int t = 0;
            while (!((mask >> t) & 1)) t++;
            for (int v = 0; v < cn; v++)
                for (int e = 0; e <= E; e++) merged[v][e] = INF;
            for (int e = 0; e <= E; e++) merged[cterms[t]][e] = 0;
        } else {
            int anchor = mask & -mask;
            for (int v = 0; v < cn; v++)
                for (int e = 0; e <= E; e++) merged[v][e] = INF;
            /* Node aggregation merge, convolving the Pareto frontiers of
             * the two terminal subtrees joined at each vertex. */
            for (int sub = (mask - 1) & mask; sub; sub = (sub - 1) & mask) {
                if (!(sub & anchor)) continue;
                int other = mask ^ sub;
                for (int v = 0; v < cn; v++) {
                    int la = front_len[sub][v];
                    int lb = front_len[other][v];
                    if (la == 0 || lb == 0) continue;
                    const short *ea = front_edges[sub][v];
                    const ll *ca = front_cost[sub][v];
                    const short *eb = front_edges[other][v];
                    const ll *cb = front_cost[other][v];
                    for (int i = 0; i < la; i++) {
                        ll c1 = ca[i];
                        int e1 = ea[i];
                        for (int j = 0; j < lb; j++) {
                            int es = e1 + eb[j];
                            if (es > E) break;  /* frontier sorted ascending */
                            int ovf = 0;
                            ll val = add_sat(c1, cb[j], &ovf);
                            if (ovf) { *overflow = 1; continue; }
                            if (val > cap) continue;
                            if (val < merged[v][es]) merged[v][es] = val;
                        }
                    }
                }
            }
            /* Exact slots -> at-most-e curve. */
            for (int v = 0; v < cn; v++)
                for (int e = 1; e <= E; e++)
                    if (merged[v][e - 1] < merged[v][e])
                        merged[v][e] = merged[v][e - 1];
        }

        /* Multi-source shortest-path closure on the layered graph: every
         * (vertex, segments-used) state is seeded with its merge label and
         * each arc traversal consumes one segment. */
        for (int v = 0; v < cn; v++)
            for (int e = 0; e <= E; e++) cur[v][e] = merged[v][e];
        heap_size = 0;
        for (int v = 0; v < cn; v++)
            for (int e = 0; e <= E; e++)
                if (cur[v][e] < INF) heap_push(cur[v][e], v * (E + 1) + e);
        while (heap_size > 0) {
            HeapItem top = heap_pop();
            int v = top.v / (E + 1);
            int e = top.v % (E + 1);
            if (top.d != cur[v][e]) continue;  /* stale label */
            if (e == E) continue;
            for (int a = first[v]; a != -1; a = arcs[a].next) {
                int w = arcs[a].to;
                int ovf = 0;
                ll nd = add_sat(top.d, arcs[a].w, &ovf);
                if (ovf) { *overflow = 1; continue; }
                if (nd > cap) continue;
                if (nd < cur[w][e + 1]) {
                    cur[w][e + 1] = nd;
                    heap_push(nd, w * (E + 1) + e + 1);
                }
            }
        }

        /* Distil the Pareto frontier: record lows of the at-most-e curve. */
        for (int v = 0; v < cn; v++) {
            int len = 0;
            ll best = INF;
            for (int e = 0; e <= E; e++) {
                if (cur[v][e] < best) {
                    front_edges[mask][v][len] = (short)e;
                    front_cost[mask][v][len] = cur[v][e];
                    len++;
                    best = cur[v][e];
                }
            }
            front_len[mask][v] = len;
        }
    }

    ll best = INF;
    for (int v = 0; v < cn; v++)
        if (cur[v][E] < best) best = cur[v][E];
    return best;
}

/*
 * Feasibility oracle for the greedy scan.
 *
 * Returns 1 iff a witness of total cost <= B that uses at most `budget`
 * segments (budget == 0 means unconstrained) contains every required[]
 * edge, avoids every excluded[] edge, and additionally contains `extra`
 * (-1 for no extra edge). Required edges are contracted by DSU node
 * aggregation; their fixed cost and segment count are charged up front and
 * the remainder is decided by one Steiner DP on the contracted multigraph.
 */
static int can_extend(const char *required, const char *excluded, int extra,
                      ll fixed_cost, int fixed_edges, ll B, int budget) {
    dsu_reset(n);
    ll base = fixed_cost;
    int ovf = 0;
    int used = fixed_edges;

    for (int j = 0; j < m; j++)
        if (required[j]) dsu_union(eu[j], ev[j]);
    if (extra >= 0) {
        dsu_union(eu[extra], ev[extra]);
        base = add_sat(base, ew[extra], &ovf);
        used++;
        if (ovf || base > B) return 0;
    }
    if (budget > 0 && used > budget) return 0;  /* segment budget blown */

    /* Renumber DSU components. */
    int comp_id[MAX_N];
    memset(comp_id, -1, sizeof(comp_id));
    int cn = 0;
    for (int v = 0; v < n; v++) {
        int r = dsu_find(v);
        if (comp_id[r] == -1) comp_id[r] = cn++;
    }

    /* Terminals mapped onto components, de-duplicated in order. */
    int cterms[MAX_K];
    int ck = 0;
    for (int i = 0; i < k; i++) {
        int c = comp_id[dsu_find(term[i])];
        int seen = 0;
        for (int j = 0; j < ck; j++)
            if (cterms[j] == c) { seen = 1; break; }
        if (!seen) cterms[ck++] = c;
    }

    /* Usable, non-excluded edges between distinct components become arcs. */
    memset(first, -1, sizeof(first));
    arc_count = 0;
    for (int j = 0; j < m; j++) {
        if (required[j] || excluded[j] || j == extra) continue;
        int a = comp_id[dsu_find(eu[j])];
        int b = comp_id[dsu_find(ev[j])];
        if (a == b) continue;  /* internal to a contracted component */
        add_arc(a, b, ew[j]);
        add_arc(b, a, ew[j]);
    }

    ll tail;
    if (ck <= 1) {
        tail = 0;  /* terminals already aggregated into one component */
    } else {
        int remaining = (budget > 0) ? budget - used : MAX_E;
        if (remaining >= cn - 1) {
            /* No tree on cn vertices uses more than cn-1 segments, so the
             * budget cannot bind: take the plain unconstrained DP. */
            tail = steiner_dp(cn, ck, cterms, &ovf);
        } else {
            tail = steiner_dp_budgeted(cn, ck, cterms, remaining,
                                       B - base, &ovf);
        }
    }

    if (ovf || tail >= INF) return 0;
    ll total = add_sat(base, tail, &ovf);
    return !ovf && total <= B;
}

/*
 * Greedy prefix scan in canonical edge order. Rebuilds the canonical
 * witness for optimum cost B into the global `required` and returns its
 * edge count. budget == 0 means unconstrained; otherwise the oracle
 * enforces the segment budget. `scan_*` tracks the forest of required
 * edges, so a candidate whose endpoints are already joined can never
 * belong to a tree witness and is rejected without an oracle.
 */
static int greedy_scan(ll B, int budget) {
    char excluded[MAX_M] = {0};
    memset(required, 0, sizeof(required));
    for (int v = 0; v < n; v++) {
        scan_parent[v] = v;
        scan_rankk[v] = 0;
    }
    ll fixed_cost = 0;
    int fixed_edges = 0;
    for (int j = 0; j < m; j++) {
        int would_cycle =
            scan_find(eu[j]) == scan_find(ev[j]);
        if (!would_cycle &&
                fixed_cost <= B - ew[j] &&
                (budget == 0 || fixed_edges < budget) &&
                can_extend(required, excluded, j, fixed_cost, fixed_edges,
                           B, budget)) {
            required[j] = 1;
            fixed_cost += ew[j];
            fixed_edges++;
            scan_union(eu[j], ev[j]);

            /* Witness complete: all terminals joined with fixed cost B. */
            int root = scan_find(term[0]);
            int joined = 1;
            for (int i = 1; i < k; i++)
                if (scan_find(term[i]) != root) { joined = 0; break; }
            if (joined) break;  /* fixed_cost must equal B by optimality */
        } else {
            excluded[j] = 1;
        }
    }

    int count = 0;
    for (int j = 0; j < m; j++) if (required[j]) count++;

    /* Final consistency: the required forest must join every terminal, cost
     * exactly B and respect the segment budget. Any failure here is an
     * internal bug, never a partial witness to the caller -- the process
     * exits non-zero. */
    {
        ll check = 0;
        int joined = 1;
        int root = scan_find(term[0]);
        for (int j = 0; j < m; j++)
            if (required[j]) check += ew[j];
        for (int i = 1; i < k; i++)
            if (scan_find(term[i]) != root) joined = 0;
        if (!joined || check != B || (budget > 0 && count > budget)) {
            fprintf(stderr,
                    "witness reconstruction failed: joined=%d cost=%lld "
                    "B=%lld edges=%d budget=%d\n",
                    joined, check, B, count, budget);
            exit(3);
        }
    }
    return count;
}

static void print_witness(ll B) {
    int count = 0;
    for (int j = 0; j < m; j++) if (required[j]) count++;
    printf("OK\n%lld\n%d\n", B, count);
    for (int j = 0; j < m; j++)
        if (required[j]) printf("%d\n", j);
}

int main(void) {
    char header[128];
    int budget = 0;
    if (fgets(header, sizeof header, stdin) == NULL ||
            sscanf(header, "%d %d %d %d", &n, &m, &k, &budget) < 3) {
        fprintf(stderr, "bad header\n");
        return 2;
    }
    if (n < 1 || n > MAX_N || m < 0 || m > MAX_M || k < 1 || k > MAX_K ||
            budget < 0) {
        fprintf(stderr, "size out of range\n");
        return 2;
    }
    for (int i = 0; i < k; i++) {
        if (scanf("%d", &term[i]) != 1) return 2;
        if (term[i] < 0 || term[i] >= n) return 2;
    }
    for (int j = 0; j < m; j++) {
        long long c;
        if (scanf("%d %d %lld", &eu[j], &ev[j], &c) != 3) return 2;
        if (eu[j] < 0 || eu[j] >= n || ev[j] < 0 || ev[j] >= n || c <= 0) {
            fprintf(stderr, "bad edge %d\n", j);
            return 2;
        }
        ew[j] = (ll)c;
    }

    /* A tree on n vertices never needs more than n-1 segments: a budget at
     * least that large never binds and takes the unconstrained path. */
    int budgeted = budget > 0 && budget < n - 1;

    /* Global optimum on the uncontracted graph, ignoring any budget. */
    build_full_arcs(0);
    int overflow = 0;
    ll B0 = steiner_dp(n, k, term, &overflow);
    if (B0 >= INF) {
        /* Terminals cannot be joined at all (or a cost overflow); no
         * segment budget changes that. */
        printf("%s\n", overflow ? "OVERFLOW" : "UNCONNECTED");
        return 0;
    }

    if (!budgeted) {
        greedy_scan(B0, 0);
        print_witness(B0);
        return 0;
    }

    /* Budgeted: if the unconstrained canonical witness already fits the
     * budget, it is the constrained canonical witness too -- it has the
     * unconstrained optimum cost (hence the constrained one) and is the
     * lexicographically smallest optimal tree overall, hence among the
     * budget-feasible ones. Nothing is truncated; the witness is complete. */
    if (greedy_scan(B0, 0) <= budget) {
        print_witness(B0);
        return 0;
    }

    /* The budget genuinely binds: solve with the segment count folded into
     * the terminal-subset state and the layered shortest-path closure. The
     * scan above rewrote `arcs` with contracted multigraphs; restore the
     * full graph first. */
    build_full_arcs(0);
    ll B = steiner_dp_budgeted(n, k, term, budget, INF, &overflow);
    if (B >= INF) {
        if (overflow) {
            printf("OVERFLOW\n");
            return 0;
        }
        /* Connected (B0 < INF) but no tree fits the budget: a unit-weight
         * run of the same terminal-subset DP minimises exactly the tree
         * size, i.e. the minimum feasible segment count. */
        build_full_arcs(1);
        int ov2 = 0;
        ll min_edges = steiner_dp(n, k, term, &ov2);
        if (min_edges >= INF) {
            printf("UNCONNECTED\n");  /* defensive; B0 ruled this out */
        } else {
            printf("OVER_BUDGET\n%lld\n", min_edges);
        }
        return 0;
    }

    greedy_scan(B, budget);
    print_witness(B);
    return 0;
}
