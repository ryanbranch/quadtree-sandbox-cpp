// tile_size_histogram: three modes of analysis over all balanced quadtrees of depth k.
//
// --has  (default): for each tile size 2^i, count trees containing at least one tile of that size.
// --total:          for each tile size 2^i, count total tiles of that size summed across all trees.
// --dist:           for each tile size 2^i, print histogram of (j -> # trees with exactly j tiles of that size).
//
// Usage: tile_size_histogram [--has|--total|--dist] [k]
//   k defaults to iterating 0..5 if omitted; a single k value restricts to that k only.

#include "quadtree_internal.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace quadtree_internal;

// ---- mode: --has and --total share the same DP structure ----
//
// lacks_map[sig][d]  = # subtrees rooted at sig with NO leaf at depth d
// total_map[sig][d]  = total # of depth-d leaves summed across all subtrees rooted at sig

struct HasTotalState {
    std::unordered_map<u64, std::vector<u256>> lacks_map;
    std::unordered_map<u64, std::vector<u256>> total_map;
    std::unordered_map<u64, u256>              counts;
};

static void run_has_total(int k, bool do_has, bool do_total) {
    HasTotalState st;
    // Seed at depth k (leaf at max depth).
    {
        u64 leaf = QuadtreeIndex::leaf_sig_int(k);
        std::vector<u256> lv(k + 1, u256(1));
        lv[k] = u256(0);
        st.lacks_map[leaf] = lv;
        std::vector<u256> tv(k + 1, u256(0));
        tv[k] = u256(1);
        st.total_map[leaf] = tv;
        st.counts[leaf] = u256(1);
    }

    for (int depth = k - 1; depth >= 0; depth--) {
        std::unordered_map<u64, std::vector<u256>> next_lacks;
        std::unordered_map<u64, std::vector<u256>> next_total;
        std::unordered_map<u64, u256>              next_counts;

        // Seed leaf at this depth into next_* only.
        {
            u64 leaf = QuadtreeIndex::leaf_sig_int(depth);
            std::vector<u256> lv(k + 1, u256(1));
            lv[depth] = u256(0);
            next_lacks[leaf] = lv;

            std::vector<u256> tv(k + 1, u256(0));
            tv[depth] = u256(1);
            next_total[leaf] = tv;

            next_counts[leaf] = u256(1);
        }

        std::vector<u64> sigs;
        sigs.reserve(st.counts.size());
        for (auto& kv : st.counts) sigs.push_back(kv.first);

        for (u64 nw : sigs) {
            u256 nw_c = st.counts[nw];
            const auto& nw_l = st.lacks_map[nw];
            const auto& nw_t = st.total_map[nw];
            for (u64 ne : sigs) {
                if (!QuadtreeIndex::compatible(sig_byte(nw,7), sig_byte(nw,6),
                                               sig_byte(ne,5), sig_byte(ne,4))) continue;
                u256 ne_c = st.counts[ne];
                const auto& ne_l = st.lacks_map[ne];
                const auto& ne_t = st.total_map[ne];
                for (u64 sw : sigs) {
                    if (!QuadtreeIndex::compatible(sig_byte(nw,3), sig_byte(nw,2),
                                                   sig_byte(sw,1), sig_byte(sw,0))) continue;
                    u256 sw_c = st.counts[sw];
                    const auto& sw_l = st.lacks_map[sw];
                    const auto& sw_t = st.total_map[sw];
                    for (u64 se : sigs) {
                        if (!QuadtreeIndex::compatible(sig_byte(ne,3), sig_byte(ne,2),
                                                       sig_byte(se,1), sig_byte(se,0))) continue;
                        if (!QuadtreeIndex::compatible(sig_byte(sw,7), sig_byte(sw,6),
                                                       sig_byte(se,5), sig_byte(se,4))) continue;
                        u64 p = parent_sig(nw, ne, sw, se);
                        u256 combo = nw_c * ne_c * sw_c * st.counts[se];
                        next_counts[p] += combo;

                        const auto& se_l = st.lacks_map[se];
                        const auto& se_t = st.total_map[se];

                        auto& pl = next_lacks[p];
                        auto& pt = next_total[p];
                        if (pl.empty()) { pl.assign(k + 1, u256(0)); pt.assign(k + 1, u256(0)); }

                        for (int d = 0; d <= k; d++) {
                            pl[d] += nw_l[d] * ne_l[d] * sw_l[d] * se_l[d];
                            // total tiles: sum of (tiles in each child * product of tree counts of other children)
                            pt[d] += (nw_t[d] * ne_c + nw_c * ne_t[d]) * (sw_c * st.counts[se])
                                   + (nw_c * ne_c) * (sw_t[d] * st.counts[se] + sw_c * se_t[d]);
                        }
                    }
                }
            }
        }

        st.lacks_map = std::move(next_lacks);
        st.total_map = std::move(next_total);
        st.counts    = std::move(next_counts);
    }

    // Aggregate across all root signatures.
    u256 total(0);
    std::vector<u256> total_lacks(k + 1, u256(0));
    std::vector<u256> total_tiles(k + 1, u256(0));
    for (auto& kv : st.counts) {
        total += kv.second;
        const auto& lv = st.lacks_map[kv.first];
        const auto& tv = st.total_map[kv.first];
        for (int d = 0; d <= k; d++) {
            total_lacks[d] += lv[d];
            total_tiles[d] += tv[d];
        }
    }

    std::printf("k=%d  total trees = %s\n\n", k, u256_to_string(total).c_str());

    if (do_has) {
        std::printf("  [--has] trees containing at least one tile of each size:\n");
        std::printf("  %-10s  %s\n", "tile size", "trees with >= 1 tile of this size");
        std::printf("  %-10s  %s\n", "---------", "----------------------------------");
        for (int i = 0; i <= k; i++) {
            int tile_size_exp = k - i;
            u256 has = total - total_lacks[i];
            std::string lbl = "2^" + std::to_string(tile_size_exp);
            std::printf("  %-10s  %s\n", lbl.c_str(), u256_to_string(has).c_str());
        }
        std::printf("\n");
    }

    if (do_total) {
        std::printf("  [--total] total tiles of each size summed across all trees:\n");
        std::printf("  %-10s  %s\n", "tile size", "total tiles across all trees");
        std::printf("  %-10s  %s\n", "---------", "----------------------------");
        for (int i = 0; i <= k; i++) {
            int tile_size_exp = k - i;
            std::string lbl = "2^" + std::to_string(tile_size_exp);
            std::printf("  %-10s  %s\n", lbl.c_str(), u256_to_string(total_tiles[i]).c_str());
        }
        std::printf("\n");
    }
}

// ---- mode: --dist ----
//
// For each target depth d, we build a polynomial DP:
//   poly_map[sig] = vector<u256> where poly_map[sig][j] = # subtrees rooted at sig with exactly j leaves at depth d.
//
// Combining four children: polynomial convolution (multiply four polynomials together,
// weighted by the number of ways to arrange the subtrees).
// Max degree per child at depth d: (2^(k-d))^2 / 4  (one quadrant's worth of tiles).
// Full polynomial degree: (2^(k-d))^2.

static void run_dist(int k) {
    std::printf("k=%d\n\n", k);

    for (int target_depth = 0; target_depth <= k; target_depth++) {
        int tile_size_exp = k - target_depth;
        int max_tiles = 1;
        for (int x = 0; x < 2 * target_depth; x++) max_tiles *= 2;
        // max_tiles = (2^target_depth)^2 = 4^target_depth

        // poly_map[sig]: polynomial of length (max_tiles+1)
        std::unordered_map<u64, std::vector<u256>> poly_map;
        std::unordered_map<u64, u256>              counts;

        // Seed at depth k.
        {
            u64 leaf = QuadtreeIndex::leaf_sig_int(k);
            std::vector<u256> pv(max_tiles + 1, u256(0));
            // A leaf at depth k has 1 tile of depth k if target_depth==k, else 0.
            int leaf_count = (target_depth == k) ? 1 : 0;
            if (leaf_count <= max_tiles) pv[leaf_count] = u256(1);
            poly_map[leaf] = std::move(pv);
            counts[leaf] = u256(1);
        }

        for (int depth = k - 1; depth >= 0; depth--) {
            std::unordered_map<u64, std::vector<u256>> next_poly;
            std::unordered_map<u64, u256>              next_counts;

            // Seed leaf at this depth into next_*.
            {
                u64 leaf = QuadtreeIndex::leaf_sig_int(depth);
                std::vector<u256> pv(max_tiles + 1, u256(0));
                int leaf_count = (target_depth == depth) ? 1 : 0;
                if (leaf_count <= max_tiles) pv[leaf_count] = u256(1);
                next_poly[leaf] = std::move(pv);
                next_counts[leaf] = u256(1);
            }

            std::vector<u64> sigs;
            sigs.reserve(counts.size());
            for (auto& kv : counts) sigs.push_back(kv.first);

            for (u64 nw : sigs) {
                u256 nw_c = counts[nw];
                const auto& nw_p = poly_map[nw];
                for (u64 ne : sigs) {
                    if (!QuadtreeIndex::compatible(sig_byte(nw,7), sig_byte(nw,6),
                                                   sig_byte(ne,5), sig_byte(ne,4))) continue;
                    u256 ne_c = counts[ne];
                    const auto& ne_p = poly_map[ne];
                    for (u64 sw : sigs) {
                        if (!QuadtreeIndex::compatible(sig_byte(nw,3), sig_byte(nw,2),
                                                       sig_byte(sw,1), sig_byte(sw,0))) continue;
                        u256 sw_c = counts[sw];
                        const auto& sw_p = poly_map[sw];
                        for (u64 se : sigs) {
                            if (!QuadtreeIndex::compatible(sig_byte(ne,3), sig_byte(ne,2),
                                                           sig_byte(se,1), sig_byte(se,0))) continue;
                            if (!QuadtreeIndex::compatible(sig_byte(sw,7), sig_byte(sw,6),
                                                           sig_byte(se,5), sig_byte(se,4))) continue;
                            u64 p = parent_sig(nw, ne, sw, se);
                            next_counts[p] += nw_c * ne_c * sw_c * counts[se];

                            const auto& se_p = poly_map[se];

                            // Convolve all four child polynomials.
                            // To keep intermediate sizes bounded, convolve pairwise.
                            // nw * ne first, then sw * se, then combine.
                            int deg = max_tiles;
                            std::vector<u256> tmp_ab(deg + 1, u256(0));
                            for (int a = 0; a <= deg; a++) {
                                if (nw_p[a].is_zero()) continue;
                                for (int b = 0; a + b <= deg; b++) {
                                    if (ne_p[b].is_zero()) continue;
                                    tmp_ab[a + b] += nw_p[a] * ne_p[b];
                                }
                            }
                            std::vector<u256> tmp_cd(deg + 1, u256(0));
                            for (int c = 0; c <= deg; c++) {
                                if (sw_p[c].is_zero()) continue;
                                for (int d2 = 0; c + d2 <= deg; d2++) {
                                    if (se_p[d2].is_zero()) continue;
                                    tmp_cd[c + d2] += sw_p[c] * se_p[d2];
                                }
                            }

                            auto& pp = next_poly[p];
                            if (pp.empty()) pp.assign(deg + 1, u256(0));
                            for (int ab = 0; ab <= deg; ab++) {
                                if (tmp_ab[ab].is_zero()) continue;
                                for (int cd = 0; ab + cd <= deg; cd++) {
                                    if (tmp_cd[cd].is_zero()) continue;
                                    pp[ab + cd] += tmp_ab[ab] * tmp_cd[cd];
                                }
                            }
                        }
                    }
                }
            }

            poly_map = std::move(next_poly);
            counts   = std::move(next_counts);
        }

        // Aggregate polynomials across all root signatures.
        u256 total(0);
        std::vector<u256> dist(max_tiles + 1, u256(0));
        for (auto& kv : counts) {
            total += kv.second;
            const auto& pv = poly_map[kv.first];
            for (int j = 0; j <= max_tiles; j++)
                dist[j] += pv[j];
        }

        std::printf("  [--dist] tile size 2^%d (depth %d), max tiles = %d:\n",
                    tile_size_exp, target_depth, max_tiles);
        std::printf("  %-8s  %s\n", "j tiles", "# trees with exactly j tiles of this size");
        std::printf("  %-8s  %s\n", "-------", "-----------------------------------------");
        for (int j = 0; j <= max_tiles; j++) {
            if (!dist[j].is_zero())
                std::printf("  %-8d  %s\n", j, u256_to_string(dist[j]).c_str());
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv) {
    bool do_has   = false;
    bool do_total = false;
    bool do_dist  = false;
    int  single_k = -1;  // -1 means "iterate all k from 0..5"

    for (int i = 1; i < argc; i++) {
        if      (std::strcmp(argv[i], "--has")   == 0) do_has   = true;
        else if (std::strcmp(argv[i], "--total") == 0) do_total = true;
        else if (std::strcmp(argv[i], "--dist")  == 0) do_dist  = true;
        else {
            char* end;
            long v = std::strtol(argv[i], &end, 10);
            if (*end == '\0' && v >= 0 && v <= 5) single_k = (int)v;
            else {
                std::fprintf(stderr, "usage: tile_size_histogram [--has] [--total] [--dist] [k (0..5)]\n");
                return 1;
            }
        }
    }

    // Default to --has if no mode specified.
    if (!do_has && !do_total && !do_dist) do_has = true;

    int k_lo = (single_k >= 0) ? single_k : 0;
    int k_hi = (single_k >= 0) ? single_k : 5;

    for (int k = k_lo; k <= k_hi; k++) {
        if (do_has || do_total) {
            run_has_total(k, do_has, do_total);
        }
        if (do_dist) {
            run_dist(k);
        }
    }

    return 0;
}
