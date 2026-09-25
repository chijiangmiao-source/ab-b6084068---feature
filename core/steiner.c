/*
 * steiner.c -- minimum Steiner tree core with canonical tie breaking and an
 * optional edge-segment budget (max_edges).
 *
 * Implemented from scratch (no generic optimiser, no edge-set enumeration):
 *   - terminal-subset dynamic programming (Dreyfus-Wagner recurrence),
 *   - node aggregation merge (disjoint-set union, also used to contract
 *     mandatory edges during witness reconstruction),
 *   - multi-source shortest-path closure over the EXPANDED state space
 *     (vertex, edge count): one multi-source Dijkstra per terminal subset,
 *     every (vertex, count) label seeded at once.
 *
 * Edge budget support is built into the DP itself rather than computed by
 * solving the unconstrained problem and truncating the answer: for every
 * terminal subset and touch vertex the solver keeps a Pareto frontier of
 * (edge count e, minimum cost c) pairs -- no (e', c') with e' <= e and
 * c' <= c is retained. Subtree merges convolve two frontiers (counts add,
 * costs add) and the closure relaxes a frontier label across an edge into
 * state (w, e + 1). The full-mask frontier therefore answers, in one pass:
 *   B   -- global minimum cost (minimum c over the frontier),
 *   h   -- minimum feasible segment count over trees of ANY cost
 *          (minimum e over the frontier; used to diagnose an over-tight
 *          budget without ever returning a partial edge set),
 *   B_L -- minimum cost among trees using at most L segments.
 *
 * Canonical witness: after the target cost is known, the lexicographically
 * smallest sorted edge-id list among all witness trees meeting the budget is
 * built by a greedy prefix scan over edges in canonical (input) order. For
 * each candidate edge the solver asks whether a feasible tree exists that
 * (a) contains every edge already required, (b) avoids every edge already
 * rejected, and (c) contains the candidate. That oracle contracts the
 * required edges through DSU node aggregation and runs one frontier Steiner
 * DP on the contracted multigraph with the remaining segment allowance.
 * Keeping only a single best (cost, witness) pair per DP state would not be
 * sound for lexicographic minimisation, so cost DP and witness reconstruction
 * are deliberately separate phases; the reconstruction uses a polynomial
 * number of DP calls (<= edge count + 1) and never enumerates edge sets.
 *
 * Input (stdin, text):
 *   line 1: n m k [max_edges]
 *           max_edges is OPTIONAL; omit it (or send -1) for the
 *           unconstrained audit, whose output is byte-for-byte compatible
 *           with the historical protocol.
 *   line 2: k terminal vertex indices
 *   next m lines: u v cost      (edge input order = canonical edge-id order)
 *
 * Output (stdout):
 *   OK
 *   <cost>
 *   <number of selected edge indices>
 *   <zero-based edge indices in input order, one per line>
 * or:
 *   UNCONNECTED
 * or:
 *   OVERFLOW
 * or:
 *   BUDGET_INFEASIBLE
 *   <minimum feasible segment count>
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
/* Saturated infinity; every reachable answer is strictly smaller. */
#define INF (LLONG_MAX / 4)
/*
 * Every witness can be chosen acyclic, so any retained subtree uses at most
 * n - 1 edges; a Pareto frontier therefore holds at most MAX_N points
 * (exactly one nondominated cost per edge count).
 */
#define MAXF MAX_N

typedef long long ll;

/* One Pareto point: minimum cost `c` among subtrees using exactly `e` edges
 * on the nondominated frontier of its state. */
typedef struct { ll c; int e; } Point;

static int n, m, k;
static int budget = -1;  /* -1 = unconstrained */
static int eu[MAX_M], ev[MAX_M];
static ll ew[MAX_M];
static int term[MAX_K];

/* Frontier storage for every (terminal subset, touch vertex). */
static Point front[MAX_STATES][MAX_N][MAXF];
static int flen[MAX_STATES][MAX_N];

/* Forward-star graph rebuilt for every feasibility oracle. */
typedef struct {
    int to;
    int next;
    ll w;
} Arc;
static Arc arcs[2 * MAX_M];
static int first[MAX_N];
static int arc_count;

/* Closure working labels: best known cost for (vertex, edge count). */
static ll label[MAX_N][MAX_N];

static void add_arc(int u, int v, ll w) {
    arcs[arc_count].to = v;
    arcs[arc_count].w = w;
    arcs[arc_count].next = first[u];
    first[u] = arc_count++;
}

static ll add_sat(ll a, ll b, int *overflow) {
    if (a >= INF || b >= INF || a > INF - b) {
        *overflow = 1;
        return INF;
    }
    return a + b;
}

/* ---- Pareto frontier helpers ------------------------------------------ */

/*
 * Insert (e, c) into frontier f, keeping only nondominated points: a stored
 * point (e', c') dominates the newcomer when e' <= e and c' <= c. Equal edge
 * counts collapse onto the cheaper cost. Result stays sorted by e ascending
 * with strictly decreasing cost.
 */
static void pf_insert(Point *f, int *len, int e, ll c, int max_e) {
    if (e > max_e || c >= INF) return;
    for (int i = 0; i < *len; i++) {
        if (f[i].e <= e && f[i].c <= c) return;  /* dominated */
    }
    int w = 0;
    for (int i = 0; i < *len; i++) {
        /* Drop points the newcomer dominates. */
        if (!(e <= f[i].e && c <= f[i].c)) f[w++] = f[i];
    }
    *len = w;
    f[(*len)++] = (Point){c, e};
    for (int i = *len - 1; i > 0 && f[i].e < f[i - 1].e; i--) {
        Point t = f[i];
        f[i] = f[i - 1];
        f[i - 1] = t;
    }
}

static void pf_merge_into(Point *dst, int *dlen,
                          const Point *src, int slen, int max_e) {
    for (int i = 0; i < slen; i++)
        pf_insert(dst, dlen, src[i].e, src[i].c, max_e);
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

/* ---- binary heap for multi-source Dijkstra over (vertex, edge count) --- */

typedef struct { ll c; int e, v; } HeapItem;

/* A closure settles at most n*(max_e+1) expanded states; each is pushed a
 * small constant number of times on this graph size. The arena matches the
 * scalar closure's historical capacity to keep the same hard-guard margin;
 * heap_push() aborts if it is ever exhausted. */
#define HEAP_CAP (1 << 18)
static HeapItem heap[HEAP_CAP];
static int heap_size;

static void heap_push(ll c, int e, int v) {
    if (heap_size >= HEAP_CAP) {
        fprintf(stderr, "search heap exhausted\n");
        exit(4);
    }
    int i = heap_size++;
    heap[i].c = c;
    heap[i].e = e;
    heap[i].v = v;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].c <= heap[i].c) break;
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
        if (l < heap_size && heap[l].c < heap[best].c) best = l;
        if (r < heap_size && heap[r].c < heap[best].c) best = r;
        if (best == i) break;
        HeapItem t = heap[best]; heap[best] = heap[i]; heap[i] = t;
        i = best;
    }
    return top;
}

/*
 * Terminal-subset DP on the graph currently held in `arcs`, tracking the
 * edge count as part of every state.
 *
 * For each terminal subset and touch vertex it computes a Pareto frontier of
 * (edge count, minimum cost) points:
 *   1. Node aggregation merge: join two terminal subtrees at one vertex
 *      (every bipartition visited once, anchor bit removes duplicates);
 *      edge counts add, costs add, and the result is Pareto-pruned.
 *   2. Multi-source shortest-path closure: ALL (vertex, edge-count) merge
 *      labels are seeded simultaneously and relaxed by one multi-source
 *      Dijkstra, where crossing an edge moves (v, e) to (w, e + 1).
 *
 * The full-mask frontier across all vertices is written to out[].
 */
static void steiner_dp(int cn, int ck, const int *cterms, int max_e,
                       Point *out, int *out_len, int *overflow) {
    int cstates = 1 << ck;
    int full = cstates - 1;
    *out_len = 0;

    for (int s = 0; s < cstates; s++)
        for (int v = 0; v < cn; v++) flen[s][v] = 0;
    for (int i = 0; i < ck; i++) {
        flen[1 << i][cterms[i]] = 1;
        front[1 << i][cterms[i]][0] = (Point){0, 0};
    }

    /* Merge labels for the subset currently being processed. */
    static Point merged[MAX_N][MAXF];
    static int mlen[MAX_N];

    for (int mask = 1; mask <= full; mask++) {
        int bits = 0;
        for (int x = mask; x; x &= x - 1) bits++;

        for (int v = 0; v < cn; v++) mlen[v] = 0;

        if (bits == 1) {
            for (int v = 0; v < cn; v++)
                pf_merge_into(merged[v], &mlen[v],
                              front[mask][v], flen[mask][v], max_e);
        } else {
            int anchor = mask & -mask;
            for (int sub = (mask - 1) & mask; sub; sub = (sub - 1) & mask) {
                if (!(sub & anchor)) continue;
                int other = mask ^ sub;
                for (int v = 0; v < cn; v++) {
                    const Point *fa = front[sub][v];
                    const Point *fb = front[other][v];
                    int la = flen[sub][v], lb = flen[other][v];
                    for (int i = 0; i < la; i++) {
                        for (int j = 0; j < lb; j++) {
                            int e = fa[i].e + fb[j].e;
                            if (e > max_e) continue;
                            ll c = add_sat(fa[i].c, fb[j].c, overflow);
                            pf_insert(merged[v], &mlen[v], e, c, max_e);
                        }
                    }
                }
            }
        }

        /* Multi-source shortest-path closure over (vertex, edge count). */
        for (int v = 0; v < cn; v++)
            for (int e = 0; e <= max_e; e++) label[v][e] = INF;

        heap_size = 0;
        for (int v = 0; v < cn; v++) {
            for (int i = 0; i < mlen[v]; i++) {
                int e = merged[v][i].e;
                ll c = merged[v][i].c;
                if (c < label[v][e]) {
                    label[v][e] = c;
                    heap_push(c, e, v);
                }
            }
        }
        while (heap_size > 0) {
            HeapItem top = heap_pop();
            if (top.c != label[top.v][top.e]) continue;  /* stale label */
            if (top.e >= max_e) continue;
            for (int a = first[top.v]; a != -1; a = arcs[a].next) {
                int w = arcs[a].to;
                int ovf = 0;
                ll nc = add_sat(top.c, arcs[a].w, &ovf);
                if (ovf) { *overflow = 1; continue; }
                if (nc < label[w][top.e + 1]) {
                    label[w][top.e + 1] = nc;
                    heap_push(nc, top.e + 1, w);
                }
            }
        }

        /* Harvest each vertex's labels into a Pareto frontier. Edge counts
         * are scanned ascending; a point survives only while its cost is
         * strictly below every cost achievable with fewer edges. */
        for (int v = 0; v < cn; v++) {
            flen[mask][v] = 0;
            ll best = INF;
            for (int e = 0; e <= max_e; e++) {
                if (label[v][e] < best) {
                    front[mask][v][flen[mask][v]++] = (Point){label[v][e], e};
                    best = label[v][e];
                }
            }
        }
    }

    for (int v = 0; v < cn; v++)
        pf_merge_into(out, out_len, front[full][v], flen[full][v], max_e);
}

/*
 * Feasibility oracle for the greedy scan.
 *
 * Returns 1 iff a witness tree of total cost <= target_cost contains every
 * required[] edge, avoids every excluded[] edge, additionally contains
 * `extra` (-1 for none), and uses at most edge_limit edges (edge_limit < 0
 * disables the segment check). Required edges are contracted by DSU node
 * aggregation; their fixed cost/edge count are added around a frontier
 * Steiner DP on the contracted multigraph.
 */
static int can_extend(const char *required, const char *excluded, int extra,
                      ll fixed_cost, int fixed_count,
                      ll target_cost, int edge_limit) {
    dsu_reset(n);
    ll base_cost = fixed_cost;
    int base_count = fixed_count;
    int ovf = 0;

    for (int j = 0; j < m; j++)
        if (required[j]) dsu_union(eu[j], ev[j]);
    if (extra >= 0) {
        dsu_union(eu[extra], ev[extra]);
        base_cost = add_sat(base_cost, ew[extra], &ovf);
        base_count++;
        if (ovf || base_cost > target_cost) return 0;
        if (edge_limit >= 0 && base_count > edge_limit) return 0;
    }

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

    /* A tree on cn components uses at most cn-1 further edges. */
    int max_e = cn - 1;
    if (edge_limit >= 0) {
        int remain = edge_limit - base_count;
        if (remain < 0) return 0;
        if (remain < max_e) max_e = remain;
    }

    Point tail[MAXF];
    int tlen = 0;
    if (ck > 1) {
        steiner_dp(cn, ck, cterms, max_e, tail, &tlen, &ovf);
    } else {
        /* Terminals already aggregated into one contracted component. */
        tail[tlen++] = (Point){0, 0};
    }

    if (ovf) return 0;
    for (int i = 0; i < tlen; i++) {
        int te = base_count + tail[i].e;
        if (edge_limit >= 0 && te > edge_limit) continue;
        ll tc = add_sat(base_cost, tail[i].c, &ovf);
        if (!ovf && tc <= target_cost) return 1;
    }
    return 0;
}

int main(void) {
    char header[256];
    if (fgets(header, sizeof(header), stdin) == NULL) {
        fprintf(stderr, "bad header\n");
        return 2;
    }
    int parsed = sscanf(header, "%d %d %d %d", &n, &m, &k, &budget);
    if (parsed < 3) {
        fprintf(stderr, "bad header\n");
        return 2;
    }
    if (parsed == 3) budget = -1;
    if (n < 1 || n > MAX_N || m < 0 || m > MAX_M || k < 1 || k > MAX_K) {
        fprintf(stderr, "size out of range\n");
        return 2;
    }
    if (budget != -1 && (budget < 1 || budget > MAX_M)) {
        fprintf(stderr, "max_edges out of range\n");
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

    /* Global frontier DP on the uncontracted graph. */
    memset(first, -1, sizeof(first));
    arc_count = 0;
    for (int j = 0; j < m; j++) {
        add_arc(eu[j], ev[j], ew[j]);
        add_arc(ev[j], eu[j], ew[j]);
    }
    int overflow = 0;
    Point global_f[MAXF];
    int glen = 0;
    steiner_dp(n, k, term, n - 1, global_f, &glen, &overflow);

    if (glen == 0) {
        printf("%s\n", overflow ? "OVERFLOW" : "UNCONNECTED");
        return 0;
    }

    /* B = global minimum cost; h = minimum feasible segment count over
     * trees of ANY cost; B_L = minimum cost within the segment budget. */
    ll B = global_f[0].c;
    int h = global_f[0].e;
    for (int i = 1; i < glen; i++) {
        if (global_f[i].c < B) B = global_f[i].c;
        if (global_f[i].e < h) h = global_f[i].e;
    }

    if (budget >= 0 && budget < h) {
        /* Connected endpoints, but no subnet meets the segment budget.
         * Report the minimum feasible count; never a partial edge set. */
        printf("BUDGET_INFEASIBLE\n%d\n", h);
        return 0;
    }

    ll target_cost = B;
    if (budget >= 0) {
        target_cost = INF;
        for (int i = 0; i < glen; i++)
            if (global_f[i].e <= budget && global_f[i].c < target_cost)
                target_cost = global_f[i].c;
        if (target_cost >= INF) {
            /* Defensive: budget >= h guarantees a frontier point qualifies. */
            fprintf(stderr, "budget target derivation failed\n");
            return 3;
        }
    }
    int edge_limit = budget;  /* -1 disables the segment check */

    /*
     * Greedy prefix scan in canonical edge order. `scan_*` tracks the forest
     * of required edges, so a candidate whose endpoints are already joined
     * can never belong to a tree witness and is rejected without an oracle.
     */
    char required[MAX_M] = {0};
    char excluded[MAX_M] = {0};
    for (int v = 0; v < n; v++) {
        scan_parent[v] = v;
        scan_rankk[v] = 0;
    }
    ll fixed_cost = 0;
    int fixed_count = 0;
    for (int j = 0; j < m; j++) {
        int would_cycle = scan_find(eu[j]) == scan_find(ev[j]);
        int count_prune =
            edge_limit >= 0 && fixed_count + 1 > edge_limit;
        int cost_prune = 0;
        {
            int ovf = 0;
            cost_prune = add_sat(fixed_cost, ew[j], &ovf) > target_cost;
        }
        if (!would_cycle && !count_prune && !cost_prune &&
                can_extend(required, excluded, j, fixed_cost, fixed_count,
                           target_cost, edge_limit)) {
            required[j] = 1;
            fixed_cost += ew[j];
            fixed_count++;
            scan_union(eu[j], ev[j]);

            /* Witness complete: all terminals joined. */
            int root = scan_find(term[0]);
            int joined = 1;
            for (int i = 1; i < k; i++)
                if (scan_find(term[i]) != root) { joined = 0; break; }
            if (joined) break;  /* fixed_cost must equal target by optimality */
        } else {
            excluded[j] = 1;
        }
    }

    int count = 0;
    for (int j = 0; j < m; j++) if (required[j]) count++;

    /* Final consistency: the required forest must join every terminal, cost
     * exactly the target, and respect the segment budget. Any failure here is
     * an internal bug, never a partial witness to the caller -- the process
     * exits non-zero. */
    {
        ll check = 0;
        int joined = 1;
        int root = scan_find(term[0]);
        for (int j = 0; j < m; j++)
            if (required[j]) check += ew[j];
        for (int i = 1; i < k; i++)
            if (scan_find(term[i]) != root) joined = 0;
        if (!joined || check != target_cost ||
                (edge_limit >= 0 && count > edge_limit)) {
            fprintf(stderr,
                    "witness reconstruction failed: joined=%d cost=%lld "
                    "target=%lld count=%d budget=%d\n",
                    joined, check, target_cost, count, edge_limit);
            return 3;
        }
    }

    printf("OK\n%lld\n%d\n", target_cost, count);
    for (int j = 0; j < m; j++)
        if (required[j]) printf("%d\n", j);
    return 0;
}
