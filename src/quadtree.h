#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using u64  = uint64_t;
using u128 = unsigned __int128;

namespace quadtree_internal { struct DirectIndexMemo; }

// ------------------------------------------------------------------ u256
//
// 256-bit unsigned integer. Counts of balanced quadtrees at k=5 reach ~10^76,
// which exceeds u128 (max ~3.4e38). Stored as four 64-bit little-endian limbs:
// limbs[0] is least significant. POD, stack-allocated, trivially serializable.
struct u256 {
    uint64_t limbs[4];

    constexpr u256() : limbs{0, 0, 0, 0} {}
    constexpr u256(int v) : limbs{(uint64_t)(int64_t)v, 0, 0, 0} {}
    constexpr u256(unsigned v) : limbs{(uint64_t)v, 0, 0, 0} {}
    constexpr u256(uint64_t v) : limbs{v, 0, 0, 0} {}
    constexpr u256(u128 v) : limbs{(uint64_t)v, (uint64_t)(v >> 64), 0, 0} {}

    bool is_zero() const { return !(limbs[0] | limbs[1] | limbs[2] | limbs[3]); }
    bool fits_u128() const { return !(limbs[2] | limbs[3]); }
    u128 to_u128() const { return ((u128)limbs[1] << 64) | limbs[0]; }
};

bool operator==(const u256& a, const u256& b);
bool operator!=(const u256& a, const u256& b);
bool operator< (const u256& a, const u256& b);
bool operator<=(const u256& a, const u256& b);
bool operator> (const u256& a, const u256& b);
bool operator>=(const u256& a, const u256& b);

u256 operator+(const u256& a, const u256& b);
u256 operator-(const u256& a, const u256& b);
u256 operator*(const u256& a, const u256& b);  // wraps mod 2^256
u256 operator/(const u256& a, const u256& b);
u256 operator%(const u256& a, const u256& b);

u256& operator+=(u256& a, const u256& b);
u256& operator-=(u256& a, const u256& b);
u256& operator*=(u256& a, const u256& b);
u256& operator/=(u256& a, const u256& b);
u256& operator%=(u256& a, const u256& b);

u256& operator++(u256& a);          // prefix
u256  operator++(u256& a, int);     // postfix

// Print decimal representation to stdout (no newline).
void u256_print(const u256& v);
// Convert to decimal string.
std::string u256_to_string(const u256& v);

// ------------------------------------------------------------------ QNode
//
// A node in a balanced quadtree tiling.
//
// `depth` is the tree level: 0 = root (covers full 2^k grid), k = max leaf depth.
// The six edge fields record the range of leaf depths touching each edge. For a
// leaf all four min/max pairs equal its own depth. For an internal node they
// propagate from children (see make_internal). These ranges are the basis of the
// balance compatibility check (compatible()) and are packed into a u64 signature.

struct QNode {
    int depth;
    int min_top, max_top;
    int min_bottom, max_bottom;
    int min_left, max_left;
    int min_right, max_right;
    // empty = leaf; size-4 = internal (NW, NE, SW, SE)
    std::vector<std::shared_ptr<QNode>> children;

    bool is_leaf() const { return children.empty(); }
};

// ------------------------------------------------------------------ Mask
//
// A cell-size mask for a 2^k × 2^k grid.
//
// grid[row][col] == 0   → unconstrained
// grid[row][col] == s>0 → that cell must belong to a tile of size s×s
//
// Additionally, exact[q] (q in {NW=0,NE=1,SW=2,SE=3}) may point to a required
// exact QNode subtree for that root quadrant (nullptr = unconstrained). Exact
// constraints take priority and are checked before the grid.
//
// RankInterval represents a contiguous half-open interval [lo, hi) of global
// tree indices that all satisfy the mask. enumerate_matching() emits these in
// ascending sorted order; using intervals is critical because millions of
// matching trees may collapse to just a handful of intervals.

struct Mask {
    std::vector<std::vector<int>> grid;          // grid[row][col], 0 = free
    std::array<std::shared_ptr<QNode>, 4> exact; // per-quadrant exact subtree (nullptr = free)

    // Construct an all-free mask for a 2^k × 2^k grid.
    static Mask free_mask(int k) {
        int sz = 1 << k;
        Mask m;
        m.grid.assign(sz, std::vector<int>(sz, 0));
        m.exact.fill(nullptr);
        return m;
    }
};

struct RankInterval {
    u256 lo; // inclusive
    u256 hi; // exclusive
};

// ------------------------------------------------------------------ Multi-quadtree tilings
//
// A composition of multiple square quadtree roots over an arbitrary rectangular
// cell grid. width/height are measured in leaf cells; each root side is 2^k.

struct RootSpec {
    int x;
    int y;
    int k;
};

struct PlacedQuadtreeRoot {
    RootSpec spec;
    std::shared_ptr<QNode> tree;
};

struct MultiQuadtreeTiling {
    int width;
    int height;
    std::vector<PlacedQuadtreeRoot> roots;
};

struct RenderTile {
    int x;
    int y;
    int size;
    int root_index;
};

// Merge adjacent/overlapping intervals and return sorted result.
std::vector<RankInterval> merge_intervals(std::vector<RankInterval> v);

// Intersect two sorted, non-overlapping interval lists (as produced by merge_intervals).
std::vector<RankInterval> intersect_intervals(const std::vector<RankInterval>& a,
                                              const std::vector<RankInterval>& b);

std::shared_ptr<QNode> make_leaf(int depth);
std::shared_ptr<QNode> make_internal(int depth,
    std::shared_ptr<QNode> nw, std::shared_ptr<QNode> ne,
    std::shared_ptr<QNode> sw, std::shared_ptr<QNode> se);

// ------------------------------------------------------------------ helpers

// Render the tree to a 2-D depth grid (size = 2^k x 2^k).
std::vector<std::vector<int>> render_grid(const QNode& tree, int k);

// Render a multi-root tiling to a rectangular cell grid of tile sizes.
std::vector<std::vector<int>> render_grid(const MultiQuadtreeTiling& tiling);

// Return exact tile rectangles for a multi-root tiling.
std::vector<RenderTile> render_tiles(const MultiQuadtreeTiling& tiling);

// Print the grid to stdout.
void print_grid(const std::vector<std::vector<int>>& grid);

// Enumerate all balanced quadtrees for a 2^k x 2^k grid (small k only).
std::vector<std::shared_ptr<QNode>> all_balanced_quadtrees(int k);

// Fast count without enumeration (uses signature maps).
u256 count_balanced_quadtrees(int k);

// Generate a random exact cover of width x height with power-of-two squares.
// If seed_root is non-null, that root is pre-placed before the rest is filled.
// If outer_edge_1x1 is true, roots touching the outer boundary are capped at k=0.
std::vector<RootSpec> random_root_cover(int width, int height, uint64_t seed,
                                        const RootSpec* seed_root = nullptr,
                                        bool outer_edge_1x1 = false);

// Generate the deterministic row-major largest-fitting-square cover.
// Produces the minimum number of power-of-two square tiles for the given grid.
// If outer_edge_1x1 is true, roots touching the outer boundary are capped at k=0.
std::vector<RootSpec> greedy_root_cover(int width, int height,
                                        bool outer_edge_1x1 = false);

// Validate only the root cover: positive dimensions, in bounds, no overlaps,
// and every cell covered exactly once.
bool validate_root_cover(int width, int height,
                         const std::vector<RootSpec>& roots,
                         std::vector<std::string>* errors = nullptr);

// Validate the full multi-quadtree tiling including inter-root border rules.
bool validate_multi_quadtree(const MultiQuadtreeTiling& tiling,
                             std::vector<std::string>* errors = nullptr);

// Validate that every cell on the outer rectangle boundary belongs to a 1x1 tile.
bool validate_outer_edges_1x1(const MultiQuadtreeTiling& tiling,
                              std::vector<std::string>* errors = nullptr);

// Precompute/load cached rank intervals for quadtrees whose selected outer
// edges are all 1x1 tiles. edge_mask bits: N=1, E=2, S=4, W=8.
//
// jobs caps how many of the 4 cardinal masks are enumerated concurrently.
// Each concurrent worker holds its own clone of the index memo, so peak RAM
// scales with jobs (k=5 measured at ~37 GB with jobs=4). Pass jobs=1 for a
// fully serial run with a much lower peak. Values are clamped to [1, 4].
void precompute_edge_1x1_cache(int k, int jobs = 4);
bool edge_1x1_cache_complete(int k);
bool edge_1x1_caches_available_for_grid(int width, int height);

// Generate a valid multi-root tiling. If specs is empty, each cover attempt
// creates a new random cover (or the greedy cover if greedy_cover=true).
// Otherwise specs is used as the fixed cover.
MultiQuadtreeTiling generate_multi_quadtree(
    int width, int height,
    const std::vector<RootSpec>& specs,
    uint64_t seed,
    int max_attempts,
    std::string* error = nullptr,
    const u256* first_root_rank = nullptr,
    bool outer_edge_1x1 = false,
    bool skip_1x1_precomputation = false,
    bool greedy_cover = false,
    std::function<void(int)> attempt_callback = nullptr,
    const RootSpec* seed_root = nullptr);

// ------------------------------------------------------------------ QuadtreeIndex

class QuadtreeIndex {
public:
    explicit QuadtreeIndex(int k);
    ~QuadtreeIndex();

    u256 total() const;
    std::shared_ptr<QNode> unrank(u256 n) const;
    u256 rank(const QNode& tree) const;

    // Precomputation-free rank/unrank: recomputes sig-counts on the fly
    // with a local memoization table. Zero startup cost, no disk I/O.
    // Slower per call (re-runs O(n_d^4) per depth level each time).
    u256 total_direct() const;
    std::shared_ptr<QNode> unrank_direct(u256 n) const;
    u256 rank_direct(const QNode& tree) const;

    // Constrained enumeration: count and enumerate global indices of all
    // balanced quadtrees (for a 2^k × 2^k grid) that satisfy the given mask.
    //
    // count_matching returns the number of matching trees.
    //
    // enumerate_matching calls cb(interval) for each contiguous rank interval
    // [lo, hi) of matching trees.  Intervals are emitted in ascending order.
    // Returns the total count of matching trees.
    u256 count_matching(const Mask& mask) const;
    u256 enumerate_matching(const Mask& mask,
                            std::function<void(RankInterval)> cb,
                            std::function<void(size_t done, size_t total)> progress_cb = nullptr) const;
    // Variant that uses a caller-supplied DirectIndexMemo (already built via
    // build_direct_memo + cloned per thread) instead of building one internally.
    // The memo must have been cloned for this thread via clone_direct_memo_for_thread.
    //
    // If restrict_to is non-null, top-level sigs whose global rank range has no
    // overlap with restrict_to are skipped entirely, avoiding tree traversal for
    // ranks that can never contribute to the final intersection.
    u256 enumerate_matching_with_memo(quadtree_internal::DirectIndexMemo& m,
                                      const Mask& mask,
                                      std::function<void(RankInterval)> cb,
                                      std::function<void(size_t done, size_t total)> progress_cb = nullptr,
                                      const std::vector<RankInterval>* restrict_to = nullptr) const;

    static u64  leaf_sig_int(int depth);
    static u64  node_sig_int(const QNode& node);
    static bool compatible(int a_max, int a_min, int b_max, int b_min);

private:
    int k_;

    // Per depth: sig_table[d] = sorted vector of packed sig u64 values.
    // Sorted by relative sig (depth subtracted) for k-independence — see architecture.md.
    mutable std::vector<std::vector<u64>>                     sig_table_;
    // sig -> position in sig_table (inverse of sig_table_)
    mutable std::vector<std::unordered_map<u64, int>>         sig_to_idx_;
    // sig -> total count of subtrees with this signature at this depth
    mutable std::vector<std::unordered_map<u64, u256>>        sig_counts_;

    // children_ and child_starts_ are populated lazily, one parent sig at a time,
    // via fetch_children_for_sig(). Keys are packed (nw|ne<<16|sw<<32|se<<48) child
    // index quadruples, sorted. starts[i] is the prefix-sum rank offset for keys[i].
    mutable std::vector<std::unordered_map<u64, std::vector<u64>>>  children_;
    mutable std::vector<std::unordered_map<u64, std::vector<u256>>> child_starts_;

    // Lightweight directory loaded from the lb*.bin file header (stays in RAM).
    // child_parents_[d]: sorted absolute parent sigs for depth d.
    // child_ptrs_[d]: pairs [2*i]=start, [2*i+1]=end into the file's keys section.
    mutable std::vector<std::vector<u64>>      child_parents_;
    mutable std::vector<std::vector<uint64_t>> child_ptrs_;
    mutable std::vector<uint64_t>              child_keys_off_;   // byte offset of keys section
    mutable std::vector<uint64_t>              child_starts_off_; // byte offset of starts section
    mutable std::vector<FILE*>                 children_fh_;      // keep open for on-demand seeks

    // True if depth d's sig data is loaded in RAM.
    mutable std::vector<bool> loaded_;

    // Shared memo for rank_direct / unrank_direct / total_direct — built once and reused.
    mutable std::unique_ptr<quadtree_internal::DirectIndexMemo> direct_memo_;

    void build();
    void build_depth(int depth);

    std::filesystem::path depth_sigs_file(int depth) const;
    std::filesystem::path depth_children_file(int depth) const;
    bool depth_files_present(int depth) const;
    void save_depth_sigs(int depth) const;
    void load_depth(int depth) const;
    void unload_depth(int depth) const;
    void ensure_loaded(int depth) const;
    void fetch_children_for_sig(int depth, u64 sig) const;

    std::shared_ptr<QNode> unrank_sig(int depth, u64 sig, u256 n) const;
    u256 rank_sig(int depth, u64 sig, const QNode& tree) const;

};
