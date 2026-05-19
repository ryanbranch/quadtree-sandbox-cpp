#pragma once

#include "quadtree_internal.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// Shared helpers used by the quadtree_multi_*.cpp translation units.
// Not part of the public API (quadtree.h).

namespace quadtree_multi_internal {

constexpr int EDGE_N = 1;
constexpr int EDGE_E = 2;
constexpr int EDGE_S = 4;
constexpr int EDGE_W = 8;

inline int root_side(const RootSpec& spec) {
    if (spec.k < 0 || spec.k >= 30) return -1;
    return 1 << spec.k;
}

inline void add_error(std::vector<std::string>* errors, const std::string& msg) {
    if (errors) errors->push_back(msg);
}

inline int max_applicable_k(int width, int height) {
    int m = std::min(width, height);
    int k = 0;
    while (k + 1 < 30 && (1 << (k + 1)) <= m) k++;
    return k;
}

inline std::filesystem::path edge_1x1_cache_file(int k, int edge_mask) {
    return quadtree_internal::cache_directory()
        / ("edge1x1_k" + std::to_string(k) + "_m" + std::to_string(edge_mask) + ".bin");
}

// Read edge-1x1 rank intervals from a cache file (built by precompute_edge_1x1_cache).
std::vector<RankInterval> read_edge_1x1_cache_file(int k, int edge_mask);

// Per-tiling tile maps built by build_tile_maps().
struct TileMaps {
    std::vector<RenderTile> tiles;
    std::vector<std::vector<int>> tile_id;
    std::vector<std::vector<int>> root_id;
};

// Populate TileMaps from a tiling. If require_full, every cell must be covered.
// Returns false and appends to errors on any problem.
bool build_tile_maps(const MultiQuadtreeTiling& tiling, bool require_full,
                     TileMaps& maps, std::vector<std::string>* errors);

// Quick structural validation used during incremental tree assignment.
bool validate_partial_multi(const MultiQuadtreeTiling& tiling,
                            std::vector<std::string>* errors = nullptr);

}  // namespace quadtree_multi_internal
