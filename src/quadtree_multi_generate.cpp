#include "quadtree_multi_internal.h"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using namespace quadtree_multi_internal;

namespace {

// --------------------------------------------------------------------------
// NeighborKey: identifies the constraint state seen by a single root.
//
// A root's neighbor-constraint mask is fully determined by the set of
// neighboring roots that have already been assigned trees.  We capture
// this as a sorted vector of (root_index, tree_ptr) pairs so that any
// change in a neighbor's assignment produces a distinct key.
// outer_edge_1x1 is folded in because it changes the mask independently.

struct NeighborKey {
    int root_index;
    bool outer_edge_1x1;
    std::vector<std::pair<int, const QNode*>> neighbors; // sorted by root_index

    bool operator==(const NeighborKey& o) const {
        return root_index == o.root_index
            && outer_edge_1x1 == o.outer_edge_1x1
            && neighbors == o.neighbors;
    }
};

struct NeighborKeyHash {
    size_t operator()(const NeighborKey& k) const {
        size_t h = std::hash<int>{}(k.root_index);
        h ^= std::hash<bool>{}(k.outer_edge_1x1) + 0x9e3779b9 + (h << 6) + (h >> 2);
        for (auto& [idx, ptr] : k.neighbors) {
            h ^= std::hash<int>{}(idx)              + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<const void*>{}(ptr)      + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

// Compute the NeighborKey for root at position root_idx in tiling.
// Scans the four border edges of the root's footprint and collects the
// unique (root_index, tree*) pairs for all assigned adjacent roots.
NeighborKey compute_neighbor_key(int root_idx,
                                 const std::vector<RootSpec>& specs,
                                 const MultiQuadtreeTiling& tiling,
                                 bool outer_edge_1x1)
{
    const RootSpec& spec = specs[root_idx];
    int side = root_side(spec);
    NeighborKey key;
    key.root_index = root_idx;
    key.outer_edge_1x1 = outer_edge_1x1;

    // Walk all assigned roots and collect those whose footprint abuts ours.
    for (int r = 0; r < (int)tiling.roots.size(); r++) {
        if (r == root_idx) continue;
        if (!tiling.roots[r].tree) continue;
        const RootSpec& rs = specs[r];
        int rs_side = root_side(rs);
        bool touches =
            (rs.x + rs_side == spec.x       && rs.y < spec.y + side && rs.y + rs_side > spec.y) ||
            (rs.x == spec.x + side           && rs.y < spec.y + side && rs.y + rs_side > spec.y) ||
            (rs.y + rs_side == spec.y        && rs.x < spec.x + side && rs.x + rs_side > spec.x) ||
            (rs.y == spec.y + side           && rs.x < spec.x + side && rs.x + rs_side > spec.x);
        if (touches)
            key.neighbors.push_back({r, tiling.roots[r].tree.get()});
    }
    std::sort(key.neighbors.begin(), key.neighbors.end());
    return key;
}

// --------------------------------------------------------------------------
// IndexCache: lazily constructs QuadtreeIndex, loads edge-1x1 intervals,
// builds DirectIndexMemos, and caches neighbor-constraint interval lists.

class IndexCache {
public:
    QuadtreeIndex& get(int k) {
        auto it = indexes_.find(k);
        if (it != indexes_.end()) return *it->second;
        auto idx = std::make_unique<QuadtreeIndex>(k);
        auto [inserted, _] = indexes_.emplace(k, std::move(idx));
        return *inserted->second;
    }

    const std::vector<RankInterval>& edge_intervals(int k, int edge_mask) {
        int key = (k << 4) | edge_mask;
        auto it = edge_intervals_.find(key);
        if (it != edge_intervals_.end()) return it->second;
        auto intervals = read_edge_1x1_cache_file(k, edge_mask);
        auto [inserted, _] = edge_intervals_.emplace(key, std::move(intervals));
        return inserted->second;
    }

    // Returns a memoized DirectIndexMemo for k, building it on first access.
    // Reusing this across all picks for the same k avoids rebuilding it
    // (opening files, loading sig tables) on every enumerate_matching call.
    quadtree_internal::DirectIndexMemo& memo_for(int k) {
        auto it = memos_.find(k);
        if (it != memos_.end()) return it->second;
        quadtree_internal::DirectIndexMemo m;
        quadtree_internal::build_direct_memo(k, m);
        auto [inserted, _] = memos_.emplace(k, std::move(m));
        return inserted->second;
    }

    // Returns the cached neighbor-constraint intervals for the given key,
    // or nullptr on a cache miss (caller must compute and then store).
    const std::vector<RankInterval>* lookup_neighbor_intervals(const NeighborKey& key) const {
        auto it = neighbor_cache_.find(key);
        if (it == neighbor_cache_.end()) return nullptr;
        return &it->second;
    }

    void store_neighbor_intervals(NeighborKey key, std::vector<RankInterval> ivs) {
        neighbor_cache_.emplace(std::move(key), std::move(ivs));
    }

private:
    std::map<int, std::unique_ptr<QuadtreeIndex>> indexes_;
    std::map<int, std::vector<RankInterval>> edge_intervals_;
    std::map<int, quadtree_internal::DirectIndexMemo> memos_;
    std::unordered_map<NeighborKey, std::vector<RankInterval>, NeighborKeyHash> neighbor_cache_;
};

// --------------------------------------------------------------------------
// Sampling helpers

u256 random_index(u256 total, std::mt19937_64& rng) {
    u256 r;
    r.limbs[0] = rng();
    r.limbs[1] = rng();
    r.limbs[2] = rng();
    r.limbs[3] = rng();
    return r % total;
}

std::shared_ptr<QNode> sample_from_intervals(
    int k,
    const std::vector<RankInterval>& intervals,
    IndexCache& indexes,
    std::mt19937_64& rng)
{
    u256 total = 0;
    for (const RankInterval& iv : intervals) total += (iv.hi - iv.lo);
    if (total.is_zero()) return nullptr;
    u256 pick = random_index(total, rng);
    for (const RankInterval& iv : intervals) {
        u256 span = iv.hi - iv.lo;
        if (pick < span) return indexes.get(k).unrank(iv.lo + pick);
        pick -= span;
    }
    return nullptr;
}

std::shared_ptr<QNode> sample_root_tree(int k, IndexCache& indexes, std::mt19937_64& rng) {
    QuadtreeIndex& idx = indexes.get(k);
    return idx.unrank(random_index(idx.total(), rng));
}

int root_outer_edge_mask(const RootSpec& spec, int width, int height) {
    int side = root_side(spec);
    int mask = 0;
    if (spec.y == 0)             mask |= EDGE_N;
    if (spec.x + side == width)  mask |= EDGE_E;
    if (spec.y + side == height) mask |= EDGE_S;
    if (spec.x == 0)             mask |= EDGE_W;
    return mask;
}

bool constraint_mask_for_root(const RootSpec& spec,
                              const MultiQuadtreeTiling& partial,
                              bool outer_edge_1x1,
                              Mask& mask) {
    int side = root_side(spec);
    if (side <= 0) return false;
    mask = Mask::free_mask(spec.k);

    TileMaps maps;
    if (!build_tile_maps(partial, false, maps, nullptr)) return false;

    bool any = false;
    auto set_cell = [&](int row, int col, int size) {
        if (size <= 0 || size > side) return;
        int& cell = mask.grid[row][col];
        if (cell == 0 || cell == size) {
            cell = size;
            any = true;
        } else {
            cell = 0;
        }
    };

    for (int dy = 0; dy < side; dy++) {
        int y = spec.y + dy;
        if (spec.x > 0 && y >= 0 && y < partial.height) {
            int tid = maps.tile_id[y][spec.x - 1];
            if (tid >= 0) set_cell(dy, 0, maps.tiles[tid].size);
        }
        int rx = spec.x + side;
        if (rx < partial.width && y >= 0 && y < partial.height) {
            int tid = maps.tile_id[y][rx];
            if (tid >= 0) set_cell(dy, side - 1, maps.tiles[tid].size);
        }
    }

    for (int dx = 0; dx < side; dx++) {
        int x = spec.x + dx;
        if (spec.y > 0 && x >= 0 && x < partial.width) {
            int tid = maps.tile_id[spec.y - 1][x];
            if (tid >= 0) set_cell(0, dx, maps.tiles[tid].size);
        }
        int by = spec.y + side;
        if (by < partial.height && x >= 0 && x < partial.width) {
            int tid = maps.tile_id[by][x];
            if (tid >= 0) set_cell(side - 1, dx, maps.tiles[tid].size);
        }
    }

    if (outer_edge_1x1) {
        if (spec.y == 0) {
            for (int x = 0; x < side; x++) { mask.grid[0][x] = 1; any = true; }
        }
        if (spec.y + side == partial.height) {
            for (int x = 0; x < side; x++) { mask.grid[side - 1][x] = 1; any = true; }
        }
        if (spec.x == 0) {
            for (int y = 0; y < side; y++) { mask.grid[y][0] = 1; any = true; }
        }
        if (spec.x + side == partial.width) {
            for (int y = 0; y < side; y++) { mask.grid[y][side - 1] = 1; any = true; }
        }
    }

    return any;
}

// restrict_to, if non-null, causes enumeration to skip top-level sigs whose
// global rank range has no overlap with the given interval list. This lets
// the neighbor search avoid traversing the entire tree when we already know
// (from the edge-cache) which global rank ranges are valid.
//
// Results are cached by NeighborKey so that repeated calls with the same
// neighbor assignment (common during backtracking retries) return instantly.
std::vector<RankInterval> get_neighbor_intervals(
    int root_idx,
    const std::vector<RootSpec>& specs,
    const MultiQuadtreeTiling& partial,
    bool outer_edge_1x1,
    IndexCache& indexes,
    const std::vector<RankInterval>* restrict_to = nullptr)
{
    NeighborKey nkey = compute_neighbor_key(root_idx, specs, partial, outer_edge_1x1);
    if (const auto* cached = indexes.lookup_neighbor_intervals(nkey))
        return *cached;

    const RootSpec& spec = specs[root_idx];
    Mask mask;
    if (!constraint_mask_for_root(spec, partial, outer_edge_1x1, mask)) {
        indexes.store_neighbor_intervals(nkey, {});
        return {};
    }
    QuadtreeIndex& idx = indexes.get(spec.k);
    auto& memo = indexes.memo_for(spec.k);
    std::vector<RankInterval> intervals;
    idx.enumerate_matching_with_memo(memo, mask,
        [&](RankInterval iv) { intervals.push_back(iv); },
        nullptr,
        restrict_to);
    indexes.store_neighbor_intervals(nkey, intervals);
    return intervals;
}

std::shared_ptr<QNode> sample_root_tree_from_neighbors(
    int root_idx,
    const std::vector<RootSpec>& specs,
    const MultiQuadtreeTiling& partial,
    bool outer_edge_1x1,
    IndexCache& indexes,
    std::mt19937_64& rng)
{
    auto intervals = get_neighbor_intervals(root_idx, specs, partial, outer_edge_1x1, indexes);
    return sample_from_intervals(specs[root_idx].k, intervals, indexes, rng);
}

std::shared_ptr<QNode> sample_root_tree_combined(
    int root_idx,
    const std::vector<RootSpec>& specs,
    int edge_mask,
    const MultiQuadtreeTiling& partial,
    IndexCache& indexes,
    std::mt19937_64& rng)
{
    const RootSpec& spec = specs[root_idx];
    const auto& edge_ivs = indexes.edge_intervals(spec.k, edge_mask);
    // Pass edge_ivs as restrict_to so enumerate_matching skips sigs outside
    // the edge-compatible rank ranges before doing any tree traversal.
    auto neighbor_ivs = get_neighbor_intervals(root_idx, specs, partial,
                                               /*outer_edge_1x1=*/false,
                                               indexes, &edge_ivs);

    if (neighbor_ivs.empty())
        return sample_from_intervals(spec.k, edge_ivs, indexes, rng);

    auto combined = intersect_intervals(edge_ivs, neighbor_ivs);
    return sample_from_intervals(spec.k, combined, indexes, rng);
}

// --------------------------------------------------------------------------
// Tree assignment

std::vector<int> assignment_order(const std::vector<RootSpec>& specs) {
    std::vector<int> order(specs.size());
    for (size_t i = 0; i < specs.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (specs[a].k != specs[b].k) return specs[a].k > specs[b].k;
        if (specs[a].y != specs[b].y) return specs[a].y < specs[b].y;
        return specs[a].x < specs[b].x;
    });
    return order;
}

bool assign_trees_rec(int pos,
                      const std::vector<int>& order,
                      const std::vector<RootSpec>& specs,
                      MultiQuadtreeTiling& tiling,
                      IndexCache& indexes,
                      std::mt19937_64& rng,
                      int per_root_attempts,
                      int& assignment_budget,
                      bool outer_edge_1x1,
                      bool skip_1x1_precomputation) {
    if (assignment_budget <= 0) return false;
    if (pos == (int)order.size())
        return validate_multi_quadtree(tiling, nullptr);

    int root_idx = order[pos];
    if (tiling.roots[root_idx].tree) {
        return validate_partial_multi(tiling, nullptr)
            && assign_trees_rec(pos + 1, order, specs, tiling, indexes, rng,
                                per_root_attempts, assignment_budget, outer_edge_1x1,
                                skip_1x1_precomputation);
    }

    int edge_mask = outer_edge_1x1
        ? root_outer_edge_mask(specs[root_idx], tiling.width, tiling.height)
        : 0;
    // k=0 roots are a single 1×1 leaf — no cache exists or is needed.
    bool use_edge_cache = edge_mask && specs[root_idx].k > 0 && !skip_1x1_precomputation;

    auto pick_tree = [&]() -> std::shared_ptr<QNode> {
        if (use_edge_cache)
            return sample_root_tree_combined(root_idx, specs, edge_mask, tiling, indexes, rng);
        if (outer_edge_1x1) {
            auto t = sample_root_tree_from_neighbors(root_idx, specs, tiling, true, indexes, rng);
            if (t) return t;
            // No neighbor constraints yet (unconstrained root): sample freely.
        }
        return sample_root_tree(specs[root_idx].k, indexes, rng);
    };

    auto constrained = pick_tree();
    if (constrained) {
        assignment_budget--;
        tiling.roots[root_idx].tree = constrained;
        if (validate_partial_multi(tiling, nullptr)
         && assign_trees_rec(pos + 1, order, specs, tiling, indexes, rng,
                             per_root_attempts, assignment_budget, outer_edge_1x1,
                             skip_1x1_precomputation))
            return true;
        tiling.roots[root_idx].tree.reset();
    }

    for (int attempt = 0; attempt < per_root_attempts && assignment_budget > 0; attempt++) {
        assignment_budget--;
        tiling.roots[root_idx].tree = pick_tree();
        if (!tiling.roots[root_idx].tree) continue;
        if (!validate_partial_multi(tiling, nullptr)) {
            tiling.roots[root_idx].tree.reset();
            continue;
        }
        if (assign_trees_rec(pos + 1, order, specs, tiling, indexes, rng,
                             per_root_attempts, assignment_budget, outer_edge_1x1,
                             skip_1x1_precomputation))
            return true;
        tiling.roots[root_idx].tree.reset();
    }
    return false;
}

}  // namespace

// --------------------------------------------------------------------------
// Public API

MultiQuadtreeTiling generate_multi_quadtree(
    int width, int height,
    const std::vector<RootSpec>& specs,
    uint64_t seed,
    int max_attempts,
    std::string* error,
    const u256* first_root_rank,
    bool outer_edge_1x1,
    bool skip_1x1_precomputation,
    bool greedy_cover,
    std::function<void(int)> attempt_callback,
    const RootSpec* seed_root) {
    bool unlimited = (max_attempts < 0);
    if (!unlimited && max_attempts == 0) max_attempts = 1;
    if (first_root_rank && specs.empty() && !seed_root)
        throw std::invalid_argument("first_root_rank requires an explicit root cover or seed_root");
    if (outer_edge_1x1 && !skip_1x1_precomputation
     && !edge_1x1_caches_available_for_grid(width, height)) {
        int max_k = max_applicable_k(width, height);
        std::ostringstream os;
        os << "missing edge-1x1 precompute cache for this "
           << width << "x" << height << " grid; run";
        for (int k = 1; k <= max_k; k++) {
            if (!edge_1x1_cache_complete(k))
                os << " `./quadtree --precompute-edge-1x1 " << k << "`";
        }
        os << " or pass --skip-1x1-precomputation to use slower on-the-fly constraints";
        if (error) *error = os.str();
        throw std::runtime_error(os.str());
    }

    std::mt19937_64 rng(seed);
    IndexCache indexes;

    // Greedy cover is deterministic; compute it once outside the attempt loop.
    std::vector<RootSpec> greedy_cover_specs;
    if (greedy_cover && specs.empty())
        greedy_cover_specs = greedy_root_cover(width, height, outer_edge_1x1);

    for (int attempt = 0; unlimited || attempt < max_attempts; attempt++) {
        if (attempt_callback) attempt_callback(attempt + 1);
        std::vector<RootSpec> cover;
        if (!specs.empty())
            cover = specs;
        else if (greedy_cover)
            cover = greedy_cover_specs;
        else
            cover = random_root_cover(width, height, rng(), seed_root, outer_edge_1x1);

        std::vector<std::string> cover_errors;
        if (!validate_root_cover(width, height, cover, &cover_errors)) {
            if (!specs.empty()) {
                if (error && !cover_errors.empty()) *error = cover_errors.front();
                throw std::runtime_error(error ? *error : "invalid root cover");
            }
            continue;
        }

        MultiQuadtreeTiling tiling;
        tiling.width = width;
        tiling.height = height;
        tiling.roots.reserve(cover.size());
        for (const RootSpec& spec : cover)
            tiling.roots.push_back({spec, nullptr});

        if (first_root_rank && !cover.empty()) {
            QuadtreeIndex& idx = indexes.get(cover[0].k);
            u256 total = idx.total();
            if (*first_root_rank >= total) {
                std::ostringstream os;
                os << "first root rank " << u256_to_string(*first_root_rank)
                   << " out of range for k=" << cover[0].k
                   << " (total=" << u256_to_string(total) << ")";
                if (error) *error = os.str();
                throw std::out_of_range(os.str());
            }
            tiling.roots[0].tree = idx.unrank(*first_root_rank);
        }

        auto order = assignment_order(cover);
        int effective_max = unlimited ? 128 : max_attempts;
        int per_root_attempts = std::max(16, std::min(256, effective_max));
        int assignment_budget = std::max(128,
            std::min(20000, (int)cover.size() * per_root_attempts * 4));
        if (assign_trees_rec(0, order, cover, tiling, indexes, rng,
                             per_root_attempts, assignment_budget, outer_edge_1x1,
                             skip_1x1_precomputation)) {
            if (!outer_edge_1x1 || validate_outer_edges_1x1(tiling, nullptr))
                return tiling;
        }
    }

    if (error) {
        std::ostringstream os;
        os << "failed to generate a valid multi-quadtree tiling after "
           << max_attempts << " attempts";
        *error = os.str();
    }
    // unlimited mode never reaches here (infinite loop exits only on success)
    throw std::runtime_error(error ? *error : "failed to generate multi-quadtree tiling");
}
