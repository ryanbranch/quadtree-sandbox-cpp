#include "quadtree_internal.h"

#include <algorithm>
#include <array>
#include <functional>
#include <stdexcept>
#include <unordered_map>

using namespace quadtree_internal;

// ------------------------------------------------------------------ match_sig memoization

// Key: (sig, row, col) — depth is implicit from row/col/sz in the call tree.
// We pack into a u64 pair for fast hashing: first=sig, second=(row<<16)|col.
struct MatchKey {
    u64 sig;
    uint32_t row_col;  // row<<16 | col
    uint32_t depth;
    bool operator==(const MatchKey& o) const {
        return sig == o.sig && row_col == o.row_col && depth == o.depth;
    }
};
struct MatchKeyHash {
    size_t operator()(const MatchKey& k) const {
        size_t h = std::hash<u64>{}(k.sig);
        h ^= std::hash<uint32_t>{}(k.row_col) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.depth)   + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
using MatchMemo = std::unordered_map<MatchKey, std::vector<RankInterval>, MatchKeyHash>;

// ------------------------------------------------------------------ constrained enumeration

struct RegionConstraint {
    bool any_constraint;
    bool need_leaf;
    bool need_split;
    bool bad;
};

static RegionConstraint inspect_region(const Mask& mask, int row, int col, int sz) {
    RegionConstraint rc{false, false, false, false};
    bool saw_eq = false, saw_lt = false, saw_gt = false;
    for (int r = row; r < row + sz; r++) {
        for (int c = col; c < col + sz; c++) {
            int v = mask.grid[r][c];
            if (v == 0) continue;
            rc.any_constraint = true;
            if (v == sz)      saw_eq = true;
            else if (v <  sz) saw_lt = true;
            else              saw_gt = true;
        }
    }
    if (saw_gt) rc.bad = true;
    if (saw_eq && saw_lt) rc.bad = true;
    if (saw_eq && !saw_lt) rc.need_leaf = true;
    if (saw_lt && !saw_eq) rc.need_split = true;
    return rc;
}

static const ChildComboList& get_child_combo_list(int depth, u64 sig, DirectIndexMemo& m)
{
    auto& cache = m.combo_cache[depth];
    auto it = cache.find(sig);
    if (it != cache.end()) return it->second;

    ChildComboList cl;
    if (!fetch_combo_list_from_children(m.children_index[depth], sig, cl))
        throw std::runtime_error("combo list not found for sig");
    auto [ins, _] = cache.emplace(sig, std::move(cl));
    return ins->second;
}

static std::vector<RankInterval> match_sig(
    int depth, int k, u64 sig,
    int row, int col, int sz,
    const Mask& mask,
    const QNode* required,
    DirectIndexMemo& m,
    MatchMemo& memo);

static u256 signature_total(int depth, u64 sig, DirectIndexMemo& m) {
    return m.counts[depth].at(sig);
}

static u256 local_rank_of(int depth, u64 sig, const QNode& tree, DirectIndexMemo& m) {
    if (tree.is_leaf()) return 0;

    const auto& cl = get_child_combo_list(depth, sig, m);
    const auto& child_sigs = m.sigs[depth + 1];
    const auto& prev       = m.counts[depth + 1];

    u64 nw_s = QuadtreeIndex::node_sig_int(*tree.children[0]);
    u64 ne_s = QuadtreeIndex::node_sig_int(*tree.children[1]);
    u64 sw_s = QuadtreeIndex::node_sig_int(*tree.children[2]);
    u64 se_s = QuadtreeIndex::node_sig_int(*tree.children[3]);

    auto idx_of = [&](u64 s) -> int {
        auto it = std::lower_bound(child_sigs.begin(), child_sigs.end(), s);
        return (int)(it - child_sigs.begin());
    };
    int t_nw = idx_of(nw_s), t_ne = idx_of(ne_s), t_sw = idx_of(sw_s), t_se = idx_of(se_s);

    for (size_t i = 0; i < cl.combos.size(); i++) {
        if (cl.combos[i][0] == t_nw && cl.combos[i][1] == t_ne
         && cl.combos[i][2] == t_sw && cl.combos[i][3] == t_se) {
            u256 ne_c = prev.at(ne_s), sw_c = prev.at(sw_s), se_c = prev.at(se_s);
            u256 n = cl.starts[i];
            n += local_rank_of(depth+1, nw_s, *tree.children[0], m) * (ne_c * sw_c * se_c);
            n += local_rank_of(depth+1, ne_s, *tree.children[1], m) * (sw_c * se_c);
            n += local_rank_of(depth+1, sw_s, *tree.children[2], m) * se_c;
            n += local_rank_of(depth+1, se_s, *tree.children[3], m);
            return n;
        }
    }
    u256 sentinel;
    for (int i = 0; i < 4; i++) sentinel.limbs[i] = ~(uint64_t)0;
    return sentinel;
}

static void offset_into(std::vector<RankInterval>& out,
                        const std::vector<RankInterval>& in,
                        u256 base) {
    for (auto& iv : in) out.push_back({base + iv.lo, base + iv.hi});
}

static std::vector<RankInterval> cross_intervals(
    const std::array<std::vector<RankInterval>, 4>& child_ivs,
    const std::array<u256, 4>& child_spans)
{
    std::array<u256, 4> strides;
    strides[3] = 1;
    strides[2] = child_spans[3];
    strides[1] = child_spans[2] * child_spans[3];
    strides[0] = child_spans[1] * child_spans[2] * child_spans[3];

    auto is_full = [&](int i) -> bool {
        return child_ivs[i].size() == 1
            && child_ivs[i][0].lo == 0
            && child_ivs[i][0].hi == child_spans[i];
    };

    std::function<void(int, u256, std::vector<RankInterval>&)> rec =
        [&](int dim, u256 base, std::vector<RankInterval>& out) {
        if (dim == 4) {
            out.push_back({base, base + 1});
            return;
        }
        bool inner_all_full = true;
        for (int j = dim + 1; j < 4; j++) {
            if (!is_full(j)) { inner_all_full = false; break; }
        }
        if (inner_all_full) {
            for (auto& iv : child_ivs[dim]) {
                u256 lo = base + iv.lo * strides[dim];
                u256 hi = base + iv.hi * strides[dim];
                out.push_back({lo, hi});
            }
            return;
        }
        for (auto& iv : child_ivs[dim]) {
            for (u256 i = iv.lo; i < iv.hi; i++) {
                rec(dim + 1, base + i * strides[dim], out);
            }
        }
    };

    std::vector<RankInterval> result;
    rec(0, 0, result);
    return merge_intervals(std::move(result));
}

static std::vector<RankInterval> match_sig(
    int depth, int k, u64 sig,
    int row, int col, int sz,
    const Mask& mask,
    const QNode* required,
    DirectIndexMemo& m,
    MatchMemo& memo)
{
    if (required) {
        if (QuadtreeIndex::node_sig_int(*required) != sig) return {};

        std::vector<std::vector<int>> rendered(sz, std::vector<int>(sz, 0));
        std::function<void(const QNode&, int, int, int)> fill =
            [&](const QNode& nd, int r, int c, int s) {
                if (nd.is_leaf()) {
                    for (int dr = 0; dr < s; dr++)
                        for (int dc = 0; dc < s; dc++)
                            rendered[r+dr][c+dc] = s;
                } else {
                    int half = s / 2;
                    fill(*nd.children[0], r,        c,        half);
                    fill(*nd.children[1], r,        c + half, half);
                    fill(*nd.children[2], r + half, c,        half);
                    fill(*nd.children[3], r + half, c + half, half);
                }
            };
        fill(*required, 0, 0, sz);
        for (int r = 0; r < sz; r++) {
            for (int c = 0; c < sz; c++) {
                int want = mask.grid[row + r][col + c];
                if (want && want != rendered[r][c]) return {};
            }
        }
        u256 lr = local_rank_of(depth, sig, *required, m);
        return { { lr, lr + 1 } };
    }

    // Memoize by (sig, row, col): same sig at the same grid position always
    // produces the same interval list for a given mask.
    MatchKey mkey{ sig, (uint32_t)((row << 16) | col), (uint32_t)depth };
    auto mit = memo.find(mkey);
    if (mit != memo.end()) return mit->second;

    RegionConstraint rc = inspect_region(mask, row, col, sz);
    if (rc.bad) return memo.emplace(mkey, std::vector<RankInterval>{}).first->second;

    if (!rc.any_constraint) {
        u256 t = signature_total(depth, sig, m);
        return memo.emplace(mkey, std::vector<RankInterval>{{0, t}}).first->second;
    }

    u64 leaf_sig = QuadtreeIndex::leaf_sig_int(depth);
    std::vector<RankInterval> out;

    if (sig == leaf_sig && !rc.need_split) {
        out.push_back({0, 1});
        if (rc.need_leaf) return memo.emplace(mkey, std::move(out)).first->second;
    }
    if (rc.need_leaf && sig != leaf_sig) {
        return memo.emplace(mkey, std::vector<RankInterval>{}).first->second;
    }
    if (rc.need_leaf) {
        return memo.emplace(mkey, std::move(out)).first->second;
    }
    if (depth >= k) {
        return memo.emplace(mkey, std::move(out)).first->second;
    }

    const auto& cl = get_child_combo_list(depth, sig, m);
    const auto& child_sigs = m.sigs[depth + 1];
    const auto& prev       = m.counts[depth + 1];
    int half = sz / 2;

    for (size_t i = 0; i < cl.combos.size(); i++) {
        const auto& combo = cl.combos[i];
        u64 nw_s = child_sigs[combo[0]];
        u64 ne_s = child_sigs[combo[1]];
        u64 sw_s = child_sigs[combo[2]];
        u64 se_s = child_sigs[combo[3]];

        const QNode* req_nw = nullptr;
        const QNode* req_ne = nullptr;
        const QNode* req_sw = nullptr;
        const QNode* req_se = nullptr;
        if (depth == 0) {
            if (mask.exact[0]) req_nw = mask.exact[0].get();
            if (mask.exact[1]) req_ne = mask.exact[1].get();
            if (mask.exact[2]) req_sw = mask.exact[2].get();
            if (mask.exact[3]) req_se = mask.exact[3].get();
        }

        std::array<std::vector<RankInterval>, 4> kid_ivs;
        std::array<u256, 4> kid_spans = {
            prev.at(nw_s), prev.at(ne_s), prev.at(sw_s), prev.at(se_s)
        };

        kid_ivs[0] = match_sig(depth+1, k, nw_s, row,        col,        half, mask, req_nw, m, memo);
        if (kid_ivs[0].empty()) continue;
        kid_ivs[1] = match_sig(depth+1, k, ne_s, row,        col + half, half, mask, req_ne, m, memo);
        if (kid_ivs[1].empty()) continue;
        kid_ivs[2] = match_sig(depth+1, k, sw_s, row + half, col,        half, mask, req_sw, m, memo);
        if (kid_ivs[2].empty()) continue;
        kid_ivs[3] = match_sig(depth+1, k, se_s, row + half, col + half, half, mask, req_se, m, memo);
        if (kid_ivs[3].empty()) continue;

        auto combo_ivs = cross_intervals(kid_ivs, kid_spans);
        offset_into(out, combo_ivs, cl.starts[i]);
    }

    out = merge_intervals(std::move(out));
    return memo.emplace(mkey, std::move(out)).first->second;
}

// ------------------------------------------------------------------ shared enumeration core

// Returns true if [lo, lo+span) overlaps any interval in sorted list r.
static bool range_overlaps_intervals(u256 lo, u256 span,
                                     const std::vector<RankInterval>& r)
{
    u256 hi = lo + span;
    // Binary search for the last interval with .lo < hi
    auto it = std::lower_bound(r.begin(), r.end(), hi,
        [](const RankInterval& iv, const u256& v) { return iv.lo < v; });
    if (it == r.begin()) return false;
    --it;
    // it->lo < hi; overlaps iff it->hi > lo
    return it->hi > lo;
}

static u256 enumerate_matching_impl(
    int k,
    DirectIndexMemo& m,
    const Mask& mask,
    std::function<void(RankInterval)> cb,
    std::function<void(size_t done, size_t total)> progress_cb,
    const std::vector<RankInterval>* restrict_to = nullptr)
{
    int sz = 1 << k;

    if ((int)mask.grid.size() != sz)
        throw std::invalid_argument("mask grid wrong row count");
    for (auto& row : mask.grid)
        if ((int)row.size() != sz)
            throw std::invalid_argument("mask grid wrong column count");

    MatchMemo memo;

    u256 base = 0;
    u256 grand_total = 0;
    std::vector<RankInterval> emit_buf;

    size_t total_sigs = m.sigs[0].size();
    size_t done_sigs  = 0;
    for (u64 sig : m.sigs[0]) {
        u256 span = m.counts[0].at(sig);
        // Skip this entire top-level sig if its global rank range [base, base+span)
        // has no overlap with restrict_to. This avoids tree traversal for sigs
        // that can never contribute after the subsequent intersection.
        if (!restrict_to || range_overlaps_intervals(base, span, *restrict_to)) {
            auto ivs = match_sig(0, k, sig, 0, 0, sz, mask, nullptr, m, memo);
            for (auto& iv : ivs) {
                emit_buf.push_back({base + iv.lo, base + iv.hi});
                grand_total += (iv.hi - iv.lo);
            }
            memo.clear();
            for (auto& cache : m.combo_cache) cache.clear();
        }
        base += span;
        ++done_sigs;
        if (progress_cb) progress_cb(done_sigs, total_sigs);
    }

    auto merged = merge_intervals(std::move(emit_buf));
    for (auto& iv : merged) cb(iv);
    return grand_total;
}

// ------------------------------------------------------------------ public constrained API

u256 QuadtreeIndex::count_matching(const Mask& mask) const {
    u256 total_cnt = 0;
    enumerate_matching(mask, [&](RankInterval iv) {
        total_cnt += (iv.hi - iv.lo);
    });
    return total_cnt;
}

u256 QuadtreeIndex::enumerate_matching(const Mask& mask,
                                       std::function<void(RankInterval)> cb,
                                       std::function<void(size_t done, size_t total)> progress_cb) const
{
    DirectIndexMemo m;
    build_direct_memo(k_, m);
    return enumerate_matching_impl(k_, m, mask, std::move(cb), std::move(progress_cb));
}

u256 QuadtreeIndex::enumerate_matching_with_memo(DirectIndexMemo& m,
                                                  const Mask& mask,
                                                  std::function<void(RankInterval)> cb,
                                                  std::function<void(size_t done, size_t total)> progress_cb,
                                                  const std::vector<RankInterval>* restrict_to) const
{
    return enumerate_matching_impl(k_, m, mask, std::move(cb), std::move(progress_cb), restrict_to);
}
