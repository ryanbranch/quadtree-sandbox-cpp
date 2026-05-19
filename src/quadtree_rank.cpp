#include "quadtree_internal.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace quadtree_internal;

// ------------------------------------------------------------------ indexed rank/unrank API

u256 QuadtreeIndex::total() const {
    ensure_loaded(0);
    u256 t = 0;
    for (auto& kv : sig_counts_[0]) t += kv.second;
    return t;
}

std::shared_ptr<QNode> QuadtreeIndex::unrank(u256 n) const {
    if (n >= total()) throw std::out_of_range("index out of range");
    ensure_loaded(0);
    for (u64 sig : sig_table_[0]) {
        u256 c = sig_counts_[0].at(sig);
        if (n < c) return unrank_sig(0, sig, n);
        n -= c;
    }
    throw std::logic_error("unreachable");
}

std::shared_ptr<QNode> QuadtreeIndex::unrank_sig(int depth, u64 sig, u256 n) const {
    ensure_loaded(depth);
    if (depth + 1 <= k_) ensure_loaded(depth + 1);
    if (sig == leaf_sig_int(depth)) {
        if (n == 0) return make_leaf(depth);
        n -= 1;
    }

    fetch_children_for_sig(depth, sig);
    const auto& starts = child_starts_[depth].at(sig);
    const auto& keys   = children_[depth].at(sig);

    int lo = 0, hi = (int)starts.size();
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (starts[mid] <= n) lo = mid + 1;
        else hi = mid;
    }
    int i = lo - 1;
    u64 packed = keys[i];
    n -= starts[i];

    const auto& table = sig_table_[depth + 1];
    u64 nw_s = table[(packed)       & 0xFFFF];
    u64 ne_s = table[(packed >> 16) & 0xFFFF];
    u64 sw_s = table[(packed >> 32) & 0xFFFF];
    u64 se_s = table[(packed >> 48) & 0xFFFF];

    const auto& sc = sig_counts_[depth + 1];
    u256 ne_c = sc.at(ne_s), sw_c = sc.at(sw_s), se_c = sc.at(se_s);

    u256 i_se = n % se_c; n /= se_c;
    u256 i_sw = n % sw_c; n /= sw_c;
    u256 i_ne = n % ne_c; n /= ne_c;
    u256 i_nw = n;

    return make_internal(depth,
        unrank_sig(depth + 1, nw_s, i_nw),
        unrank_sig(depth + 1, ne_s, i_ne),
        unrank_sig(depth + 1, sw_s, i_sw),
        unrank_sig(depth + 1, se_s, i_se));
}

u256 QuadtreeIndex::rank(const QNode& tree) const {
    ensure_loaded(0);
    u64 sig = node_sig_int(tree);
    u256 n = 0;
    for (u64 s : sig_table_[0]) {
        if (s == sig) break;
        n += sig_counts_[0].at(s);
    }
    return n + rank_sig(0, sig, tree);
}

u256 QuadtreeIndex::rank_sig(int depth, u64 sig, const QNode& tree) const {
    ensure_loaded(depth);
    if (depth + 1 <= k_) ensure_loaded(depth + 1);
    if (tree.is_leaf()) return 0;

    const auto& nw = *tree.children[0];
    const auto& ne = *tree.children[1];
    const auto& sw = *tree.children[2];
    const auto& se = *tree.children[3];

    u64 nw_s = node_sig_int(nw);
    u64 ne_s = node_sig_int(ne);
    u64 sw_s = node_sig_int(sw);
    u64 se_s = node_sig_int(se);

    const auto& s2i = sig_to_idx_[depth + 1];
    u64 target = pack_key(s2i.at(nw_s), s2i.at(ne_s), s2i.at(sw_s), s2i.at(se_s));

    fetch_children_for_sig(depth, sig);
    const auto& keys   = children_[depth].at(sig);
    const auto& starts = child_starts_[depth].at(sig);
    auto it = std::lower_bound(keys.begin(), keys.end(), target);
    int j = (int)(it - keys.begin());
    u256 n = starts[j];
    if (sig == leaf_sig_int(depth)) n += 1;

    const auto& sc = sig_counts_[depth + 1];
    u256 ne_c = sc.at(ne_s), sw_c = sc.at(sw_s), se_c = sc.at(se_s);

    n += rank_sig(depth + 1, nw_s, nw) * (ne_c * sw_c * se_c);
    n += rank_sig(depth + 1, ne_s, ne) * (sw_c * se_c);
    n += rank_sig(depth + 1, sw_s, sw) * se_c;
    n += rank_sig(depth + 1, se_s, se);
    return n;
}

// ------------------------------------------------------------------ direct (precomputation-free)

// Open the QTCH children file for depth d and populate m.children_index[d].
// Also overwrites m.sigs[d+1] with the file's child-sig ordering (authoritative
// for packed-key indices) and m.sigs[d] with the file's parent-sig set (plus
// the leaf sig for depth d, which has no row in the file).
static void load_children_index_for_depth(int k, int d, DirectIndexMemo& m) {
    auto path = children_file_path(k, d);
    FILE* cf = std::fopen(path.c_str(), "rb");
    if (!cf) throw std::runtime_error("cannot open children file for read: "
                                      + path.string());

    char magic[4];
    read_exact_file(magic, 1, 4, cf);
    if (std::memcmp(magic, "QTCH", 4) != 0) {
        std::fclose(cf);
        throw std::runtime_error("bad children magic in " + path.string());
    }
    uint32_t ver, lb, n_child_sigs;
    read_exact_file(&ver, 4, 1, cf);
    read_exact_file(&lb,  4, 1, cf);
    (void)ver;
    if ((int)lb != k - d) {
        std::fclose(cf);
        throw std::runtime_error("levels_below mismatch in " + path.string());
    }

    read_exact_file(&n_child_sigs, 4, 1, cf);
    std::vector<u64> rel_child_sigs(n_child_sigs);
    read_exact_file(rel_child_sigs.data(), 8, n_child_sigs, cf);

    uint32_t n_parents;
    read_exact_file(&n_parents, 4, 1, cf);
    std::vector<u64>      rel_parents(n_parents);
    std::vector<uint64_t> ptrs(n_parents + 1);
    uint64_t total_keys;
    read_exact_file(rel_parents.data(), 8, n_parents, cf);
    read_exact_file(ptrs.data(), 8, n_parents + 1, cf);
    read_exact_file(&total_keys, 8, 1, cf);

    uint64_t header_bytes = 4 + 4 + 4 + 4
        + (uint64_t)n_child_sigs * 8
        + 4
        + (uint64_t)n_parents * 8
        + ((uint64_t)n_parents + 1) * 8
        + 8;

    // Authoritative depth-(d+1) child ordering: the packed keys in this file
    // index into rel_child_sigs[], so m.sigs[d+1] must match it exactly.
    std::vector<u64> abs_child_sigs(n_child_sigs);
    for (uint32_t i = 0; i < n_child_sigs; i++)
        abs_child_sigs[i] = relative_to_sig(rel_child_sigs[i], d + 1);
    m.sigs[d + 1] = std::move(abs_child_sigs);

    // Build the in-RAM directory, re-sorted by raw u64 so a plain lower_bound
    // resolves a parent sig to its key/start range.
    std::vector<std::tuple<u64, uint64_t, uint64_t>> par_ptr(n_parents);
    for (uint32_t i = 0; i < n_parents; i++)
        par_ptr[i] = { relative_to_sig(rel_parents[i], d), ptrs[i], ptrs[i + 1] };
    std::sort(par_ptr.begin(), par_ptr.end());

    ChildrenFileIndex& idx = m.children_index[d];
    idx.parents.resize(n_parents);
    idx.begins.resize(n_parents);
    idx.ends.resize(n_parents);
    for (uint32_t i = 0; i < n_parents; i++) {
        idx.parents[i] = std::get<0>(par_ptr[i]);
        idx.begins[i]  = std::get<1>(par_ptr[i]);
        idx.ends[i]    = std::get<2>(par_ptr[i]);
    }
    idx.keys_off   = header_bytes;
    idx.starts_off = header_bytes + total_keys * 8;
    idx.leaf_sig   = QuadtreeIndex::leaf_sig_int(d);
    idx.fh         = cf;

    // Depth-d sig set: every parent in the file plus the leaf sig (which the
    // file omits because the single-leaf tree has no child quads).
    std::vector<u64>& sv = m.sigs[d];
    sv.clear();
    sv.reserve(n_parents + 1);
    for (uint32_t i = 0; i < n_parents; i++) sv.push_back(idx.parents[i]);
    if (!std::binary_search(sv.begin(), sv.end(), idx.leaf_sig)) {
        sv.push_back(idx.leaf_sig);
        std::sort(sv.begin(), sv.end());
    }
}

void quadtree_internal::build_direct_memo(int k, DirectIndexMemo& m) {
    if (m.built) return;
    m.counts.resize(k + 1);
    m.sigs.resize(k + 1);
    m.children_index.resize(k + 1);
    m.combo_cache.resize(k + 1);

    u64 lk = QuadtreeIndex::leaf_sig_int(k);
    m.counts[k][lk] = 1;
    m.sigs[k] = {lk};

    for (int d = k - 1; d >= 0; d--) {
        // child_sigs is needed to (re)build the QTCH file if it is missing.
        // For d = k-1 this is just {leaf_k}; for d < k-1 it is the depth-(d+1)
        // ordering established by the previous iteration's file load.
        const std::vector<u64> child_sigs = m.sigs[d + 1];
        const auto& prev = m.counts[d + 1];

        // Ensure lb{k-d}_children.bin exists (idempotent), then open it. The
        // open call also overwrites m.sigs[d+1] with the file's child ordering
        // and sets m.sigs[d] to the file's parent set (+ leaf sig).
        build_children_file_for_depth(k, d, child_sigs, prev);
        load_children_index_for_depth(k, d, m);

        // Compute depth-d subtree counts by enumerating every valid child quad.
        // This is an in-RAM pass over the compatibility matrices; it never
        // touches the (potentially huge) on-disk key section.
        const std::vector<u64>& csigs = m.sigs[d + 1];  // file's ordering
        int n = (int)csigs.size();
        std::vector<uint8_t> right_mat(n * n, 0);
        std::vector<uint8_t> bottom_mat(n * n, 0);
        for (int i = 0; i < n; i++) {
            u64 si = csigs[i];
            int a7=sig_byte(si,7),a6=sig_byte(si,6),a3=sig_byte(si,3),a2=sig_byte(si,2);
            for (int j = 0; j < n; j++) {
                u64 sj = csigs[j];
                if ((a7-(int)sig_byte(sj,4)<=1)&&((int)sig_byte(sj,5)-a6<=1)) right_mat[i*n+j]=1;
                if ((a3-(int)sig_byte(sj,0)<=1)&&((int)sig_byte(sj,1)-a2<=1)) bottom_mat[i*n+j]=1;
            }
        }

        auto& cur = m.counts[d];
        cur[QuadtreeIndex::leaf_sig_int(d)] = 1;
        for (int nw = 0; nw < n; nw++) {
            const uint8_t* rc_nw = &right_mat[nw*n];
            const uint8_t* bc_nw = &bottom_mat[nw*n];
            u256 nw_c = prev.at(csigs[nw]);
            for (int ne = 0; ne < n; ne++) {
                if (!rc_nw[ne]) continue;
                const uint8_t* bc_ne = &bottom_mat[ne*n];
                u256 ne_c = prev.at(csigs[ne]);
                for (int sw = 0; sw < n; sw++) {
                    if (!bc_nw[sw]) continue;
                    const uint8_t* rc_sw = &right_mat[sw*n];
                    u256 sw_c = prev.at(csigs[sw]);
                    for (int se = 0; se < n; se++) {
                        if (!bc_ne[se]) continue;
                        if (!rc_sw[se]) continue;
                        u64 p = parent_sig(csigs[nw],csigs[ne],csigs[sw],csigs[se]);
                        cur[p] += nw_c * ne_c * sw_c * prev.at(csigs[se]);
                    }
                }
            }
        }
    }

    // Depth k leaf: seed an empty entry directly in the cache (no file needed).
    m.combo_cache[k][lk] = ChildComboList{};

    m.built = true;
}

DirectIndexMemo quadtree_internal::clone_direct_memo_for_thread(int k, const DirectIndexMemo& src)
{
    DirectIndexMemo dst;
    dst.counts = src.counts;
    dst.sigs   = src.sigs;
    dst.children_index.resize(k + 1);
    dst.combo_cache.resize(k + 1);

    // Depth k has no children file; seed the leaf entry as build_direct_memo does.
    u64 lk = QuadtreeIndex::leaf_sig_int(k);
    dst.combo_cache[k][lk] = ChildComboList{};

    for (int d = 0; d < k; d++) {
        const ChildrenFileIndex& s = src.children_index[d];
        ChildrenFileIndex& t = dst.children_index[d];

        // Copy the read-only header fields.
        t.parents    = s.parents;
        t.begins     = s.begins;
        t.ends       = s.ends;
        t.keys_off   = s.keys_off;
        t.starts_off = s.starts_off;
        t.leaf_sig   = s.leaf_sig;

        // Open a fresh FILE* so this thread has its own seek position.
        auto path = children_file_path(k, d);
        t.fh = std::fopen(path.c_str(), "rb");
        if (!t.fh)
            throw std::runtime_error("clone_direct_memo_for_thread: cannot open "
                                     + path.string());
    }

    dst.built = true;
    return dst;
}

// Return a reference to the ChildComboList for (depth, sig), fetching from disk if needed.
static const ChildComboList& get_combo_list(int depth, u64 sig, DirectIndexMemo& m)
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

static std::shared_ptr<QNode> unrank_sig_direct_impl(
    int depth, u64 sig, u256 n, DirectIndexMemo& m)
{
    if (sig == QuadtreeIndex::leaf_sig_int(depth)) {
        if (n == 0) return make_leaf(depth);
        n -= 1;
    }

    const auto& child_sigs = m.sigs[depth + 1];
    const auto& prev       = m.counts[depth + 1];

    const auto& cl = get_combo_list(depth, sig, m);

    // Binary search for the combo whose start <= n, then step into it.
    int lo = 0, hi = (int)cl.starts.size();
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cl.starts[mid] <= n) lo = mid + 1;
        else hi = mid;
    }
    int i = lo - 1;
    u64 packed = pack_key(cl.combos[i][0], cl.combos[i][1],
                          cl.combos[i][2], cl.combos[i][3]);
    n -= cl.starts[i];

    int i_nw = (int)( packed        & 0xFFFF);
    int i_ne = (int)((packed >> 16) & 0xFFFF);
    int i_sw = (int)((packed >> 32) & 0xFFFF);
    int i_se = (int)((packed >> 48) & 0xFFFF);

    u64 nw_s = child_sigs[i_nw];
    u64 ne_s = child_sigs[i_ne];
    u64 sw_s = child_sigs[i_sw];
    u64 se_s = child_sigs[i_se];

    u256 ne_c = prev.at(ne_s), sw_c = prev.at(sw_s), se_c = prev.at(se_s);

    u256 i_se_n = n % se_c; n /= se_c;
    u256 i_sw_n = n % sw_c; n /= sw_c;
    u256 i_ne_n = n % ne_c; n /= ne_c;
    u256 i_nw_n = n;

    return make_internal(depth,
        unrank_sig_direct_impl(depth+1, nw_s, i_nw_n, m),
        unrank_sig_direct_impl(depth+1, ne_s, i_ne_n, m),
        unrank_sig_direct_impl(depth+1, sw_s, i_sw_n, m),
        unrank_sig_direct_impl(depth+1, se_s, i_se_n, m));
}

static u256 rank_sig_direct_impl(
    int depth, u64 sig, const QNode& tree, DirectIndexMemo& m)
{
    if (tree.is_leaf()) return 0;

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
    u64 target = pack_key(t_nw, t_ne, t_sw, t_se);

    const auto& cl = get_combo_list(depth, sig, m);
    u256 n = (sig == QuadtreeIndex::leaf_sig_int(depth)) ? 1 : 0;
    for (size_t ci = 0; ci < cl.combos.size(); ci++) {
        u64 packed = pack_key(cl.combos[ci][0], cl.combos[ci][1],
                              cl.combos[ci][2], cl.combos[ci][3]);
        if (packed == target) {
            n += cl.starts[ci];
            break;
        }
    }

    u256 ne_c = prev.at(ne_s), sw_c = prev.at(sw_s), se_c = prev.at(se_s);
    n += rank_sig_direct_impl(depth+1, nw_s, *tree.children[0], m) * (ne_c * sw_c * se_c);
    n += rank_sig_direct_impl(depth+1, ne_s, *tree.children[1], m) * (sw_c * se_c);
    n += rank_sig_direct_impl(depth+1, sw_s, *tree.children[2], m) * se_c;
    n += rank_sig_direct_impl(depth+1, se_s, *tree.children[3], m);
    return n;
}

// ------------------------------------------------------------------ public direct API

static DirectIndexMemo& ensure_direct_memo(int k,
    std::unique_ptr<DirectIndexMemo>& ptr)
{
    if (!ptr) ptr = std::make_unique<DirectIndexMemo>();
    build_direct_memo(k, *ptr);
    return *ptr;
}

u256 QuadtreeIndex::total_direct() const {
    auto& m = ensure_direct_memo(k_, direct_memo_);
    u256 t = 0;
    for (auto& kv : m.counts[0]) t += kv.second;
    return t;
}

std::shared_ptr<QNode> QuadtreeIndex::unrank_direct(u256 n) const {
    auto& m = ensure_direct_memo(k_, direct_memo_);
    u256 t = 0;
    for (auto& kv : m.counts[0]) t += kv.second;
    if (n >= t) throw std::out_of_range("index out of range");

    for (u64 sig : m.sigs[0]) {
        u256 c = m.counts[0].at(sig);
        if (n < c) return unrank_sig_direct_impl(0, sig, n, m);
        n -= c;
    }
    throw std::logic_error("unreachable");
}

u256 QuadtreeIndex::rank_direct(const QNode& tree) const {
    auto& m = ensure_direct_memo(k_, direct_memo_);
    u64 sig = node_sig_int(tree);
    u256 n = 0;
    for (u64 s : m.sigs[0]) {
        if (s == sig) break;
        n += m.counts[0].at(s);
    }
    return n + rank_sig_direct_impl(0, sig, tree, m);
}
