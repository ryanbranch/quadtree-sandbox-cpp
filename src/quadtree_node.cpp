#include "quadtree_internal.h"

#include <algorithm>
#include <iostream>
#include <unordered_map>

using namespace quadtree_internal;

// ------------------------------------------------------------------ QNode helpers

std::shared_ptr<QNode> make_leaf(int depth) {
    auto n = std::make_shared<QNode>();
    n->depth = depth;
    n->min_top = n->max_top = depth;
    n->min_bottom = n->max_bottom = depth;
    n->min_left = n->max_left = depth;
    n->min_right = n->max_right = depth;
    return n;
}

std::shared_ptr<QNode> make_internal(int depth,
    std::shared_ptr<QNode> nw, std::shared_ptr<QNode> ne,
    std::shared_ptr<QNode> sw, std::shared_ptr<QNode> se)
{
    auto n = std::make_shared<QNode>();
    n->depth      = depth;
    n->min_top    = std::min(nw->min_top, ne->min_top);
    n->max_top    = std::max(nw->max_top, ne->max_top);
    n->min_bottom = std::min(sw->min_bottom, se->min_bottom);
    n->max_bottom = std::max(sw->max_bottom, se->max_bottom);
    n->min_left   = std::min(nw->min_left, sw->min_left);
    n->max_left   = std::max(nw->max_left, sw->max_left);
    n->min_right  = std::min(ne->min_right, se->min_right);
    n->max_right  = std::max(ne->max_right, se->max_right);
    n->children   = {nw, ne, sw, se};
    return n;
}

// ------------------------------------------------------------------ rendering

static void fill_grid(const QNode& node, int r, int c, int sz,
                      std::vector<std::vector<int>>& grid) {
    if (node.is_leaf()) {
        for (int dr = 0; dr < sz; dr++)
            for (int dc = 0; dc < sz; dc++)
                grid[r+dr][c+dc] = sz;
    } else {
        int half = sz / 2;
        fill_grid(*node.children[0], r,        c,        half, grid);
        fill_grid(*node.children[1], r,        c + half, half, grid);
        fill_grid(*node.children[2], r + half, c,        half, grid);
        fill_grid(*node.children[3], r + half, c + half, half, grid);
    }
}

std::vector<std::vector<int>> render_grid(const QNode& tree, int k) {
    int size = 1 << k;
    std::vector<std::vector<int>> grid(size, std::vector<int>(size, 0));
    fill_grid(tree, 0, 0, size, grid);
    return grid;
}

void print_grid(const std::vector<std::vector<int>>& grid) {
    if (grid.empty()) { std::cout << "[]\n"; return; }
    int maxv = 0;
    for (auto& row : grid) for (int v : row) if (v > maxv) maxv = v;
    int width = std::to_string(maxv).size();
    for (auto& row : grid) {
        for (size_t i = 0; i < row.size(); i++) {
            if (i) std::cout << ' ';
            std::cout.width(width);
            std::cout << row[i];
        }
        std::cout << '\n';
    }
}

// ------------------------------------------------------------------ static helpers

bool QuadtreeIndex::compatible(int a_max, int a_min, int b_max, int b_min) {
    return (a_max - b_min <= 1) && (b_max - a_min <= 1);
}

u64 QuadtreeIndex::leaf_sig_int(int depth) {
    u64 d = (u64)(depth & 0xFF);
    return d | (d<<8) | (d<<16) | (d<<24) | (d<<32) | (d<<40) | (d<<48) | (d<<56);
}

u64 QuadtreeIndex::node_sig_int(const QNode& n) {
    return (u64)(uint8_t)n.min_top
        | ((u64)(uint8_t)n.max_top    <<  8)
        | ((u64)(uint8_t)n.min_bottom << 16)
        | ((u64)(uint8_t)n.max_bottom << 24)
        | ((u64)(uint8_t)n.min_left   << 32)
        | ((u64)(uint8_t)n.max_left   << 40)
        | ((u64)(uint8_t)n.min_right  << 48)
        | ((u64)(uint8_t)n.max_right  << 56);
}

// ------------------------------------------------------------------ count_balanced_quadtrees

u256 count_balanced_quadtrees(int k) {
    std::unordered_map<u64, u256> counts;
    counts[QuadtreeIndex::leaf_sig_int(k)] = 1;

    for (int depth = k - 1; depth >= 0; depth--) {
        std::unordered_map<u64, u256> next_counts;
        next_counts[QuadtreeIndex::leaf_sig_int(depth)] = 1;

        std::vector<u64> sigs;
        sigs.reserve(counts.size());
        for (auto& kv : counts) sigs.push_back(kv.first);

        for (u64 nw : sigs) {
            u256 nw_c = counts[nw];
            for (u64 ne : sigs) {
                if (!QuadtreeIndex::compatible(sig_byte(nw,7), sig_byte(nw,6), sig_byte(ne,5), sig_byte(ne,4))) continue;
                u256 ne_c = counts[ne];
                for (u64 sw : sigs) {
                    if (!QuadtreeIndex::compatible(sig_byte(nw,3), sig_byte(nw,2), sig_byte(sw,1), sig_byte(sw,0))) continue;
                    u256 sw_c = counts[sw];
                    for (u64 se : sigs) {
                        if (!QuadtreeIndex::compatible(sig_byte(ne,3), sig_byte(ne,2), sig_byte(se,1), sig_byte(se,0))) continue;
                        if (!QuadtreeIndex::compatible(sig_byte(sw,7), sig_byte(sw,6), sig_byte(se,5), sig_byte(se,4))) continue;
                        u64 p = parent_sig(nw, ne, sw, se);
                        next_counts[p] += nw_c * ne_c * sw_c * counts[se];
                    }
                }
            }
        }
        counts = std::move(next_counts);
    }

    u256 total = 0;
    for (auto& kv : counts) total += kv.second;
    return total;
}

// ------------------------------------------------------------------ enumeration (small k only)

std::vector<std::shared_ptr<QNode>> all_balanced_quadtrees(int k) {
    std::vector<std::shared_ptr<QNode>> trees;
    trees.push_back(make_leaf(k));

    for (int depth = k - 1; depth >= 0; depth--) {
        std::vector<std::shared_ptr<QNode>> next_trees;
        next_trees.push_back(make_leaf(depth));
        auto& children = trees;

        for (auto& nw : children) {
            for (auto& ne : children) {
                if (!QuadtreeIndex::compatible(nw->max_right, nw->min_right,
                                                ne->max_left, ne->min_left)) continue;
                for (auto& sw : children) {
                    if (!QuadtreeIndex::compatible(nw->max_bottom, nw->min_bottom,
                                                    sw->max_top, sw->min_top)) continue;
                    for (auto& se : children) {
                        if (!QuadtreeIndex::compatible(ne->max_bottom, ne->min_bottom,
                                                        se->max_top, se->min_top)) continue;
                        if (!QuadtreeIndex::compatible(sw->max_right, sw->min_right,
                                                        se->max_left, se->min_left)) continue;
                        next_trees.push_back(make_internal(depth, nw, ne, sw, se));
                    }
                }
            }
        }
        trees = std::move(next_trees);
    }
    return trees;
}

std::vector<RankInterval> intersect_intervals(const std::vector<RankInterval>& a,
                                              const std::vector<RankInterval>& b) {
    std::vector<RankInterval> out;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        u256 lo = std::max(a[i].lo, b[j].lo);
        u256 hi = std::min(a[i].hi, b[j].hi);
        if (lo < hi) out.push_back({lo, hi});
        if (a[i].hi < b[j].hi) ++i;
        else                    ++j;
    }
    return out;
}

std::vector<RankInterval> merge_intervals(std::vector<RankInterval> v) {
    if (v.empty()) return v;
    std::sort(v.begin(), v.end(), [](const RankInterval& a, const RankInterval& b) {
        return a.lo < b.lo;
    });
    std::vector<RankInterval> out;
    out.reserve(v.size());
    out.push_back(v[0]);
    for (size_t i = 1; i < v.size(); i++) {
        if (v[i].lo <= out.back().hi) {
            if (v[i].hi > out.back().hi) out.back().hi = v[i].hi;
        } else {
            out.push_back(v[i]);
        }
    }
    return out;
}
