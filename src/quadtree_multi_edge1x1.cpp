#include "quadtree_multi_internal.h"

#include <array>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>

using namespace quadtree_internal;
using namespace quadtree_multi_internal;

namespace {

constexpr uint32_t kEdge1x1CacheVersion = 1;
constexpr char kEdge1x1CacheMagic[5] = "QE1X";

Mask edge_1x1_mask_for_k(int k, int edge_mask) {
    Mask mask = Mask::free_mask(k);
    int side = 1 << k;
    if (edge_mask & EDGE_N) {
        for (int x = 0; x < side; x++) mask.grid[0][x] = 1;
    }
    if (edge_mask & EDGE_E) {
        for (int y = 0; y < side; y++) mask.grid[y][side - 1] = 1;
    }
    if (edge_mask & EDGE_S) {
        for (int x = 0; x < side; x++) mask.grid[side - 1][x] = 1;
    }
    if (edge_mask & EDGE_W) {
        for (int y = 0; y < side; y++) mask.grid[y][0] = 1;
    }
    return mask;
}

void write_edge_1x1_cache_file(int k, int edge_mask,
                               const std::vector<RankInterval>& intervals) {
    std::filesystem::create_directories(cache_directory());
    std::ofstream os(edge_1x1_cache_file(k, edge_mask), std::ios::binary);
    if (!os) throw std::runtime_error("cannot open edge 1x1 cache for write");
    os.write(kEdge1x1CacheMagic, 4);
    write_pod(os, kEdge1x1CacheVersion);
    uint32_t kk = (uint32_t)k;
    uint32_t mm = (uint32_t)edge_mask;
    uint64_t count = (uint64_t)intervals.size();
    write_pod(os, kk);
    write_pod(os, mm);
    write_pod(os, count);
    for (const RankInterval& iv : intervals) {
        write_u256_binary(os, iv.lo);
        write_u256_binary(os, iv.hi);
    }
}

bool edge_1x1_cache_file_present(int k, int edge_mask) {
    auto path = edge_1x1_cache_file(k, edge_mask);
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    return !ec && sz > 0;
}

static std::vector<RankInterval> compute_edge_1x1_intervals(
    const QuadtreeIndex& idx, int k, int edge_mask,
    quadtree_internal::DirectIndexMemo& thread_memo,
    std::mutex& print_mtx)
{
    std::vector<RankInterval> intervals;
    Mask mask = edge_1x1_mask_for_k(k, edge_mask);
    size_t last_pct = 0;
    idx.enumerate_matching_with_memo(thread_memo, mask,
        [&](RankInterval iv) { intervals.push_back(iv); },
        [&](size_t done, size_t total) {
            size_t pct = done * 100 / total;
            if (pct > last_pct) {
                last_pct = pct;
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cout << "  edge1x1 k=" << k << " mask=" << edge_mask
                          << " " << last_pct << "%\n" << std::flush;
            }
        });
    std::lock_guard<std::mutex> lk(print_mtx);
    std::cout << "  edge1x1 k=" << k << " mask=" << edge_mask
              << " done, intervals=" << intervals.size() << "\n" << std::flush;
    return intervals;
}

}  // namespace

namespace quadtree_multi_internal {

std::vector<RankInterval> read_edge_1x1_cache_file(int k, int edge_mask) {
    std::ifstream is(edge_1x1_cache_file(k, edge_mask), std::ios::binary);
    if (!is) throw std::runtime_error("cannot open edge 1x1 cache for read");

    char magic[4];
    is.read(magic, 4);
    if (std::memcmp(magic, kEdge1x1CacheMagic, 4) != 0)
        throw std::runtime_error("bad edge 1x1 cache magic");
    uint32_t version, kk, mm;
    uint64_t count;
    read_pod(is, version);
    read_pod(is, kk);
    read_pod(is, mm);
    read_pod(is, count);
    if (version != kEdge1x1CacheVersion)
        throw std::runtime_error("bad edge 1x1 cache version");
    if ((int)kk != k || (int)mm != edge_mask)
        throw std::runtime_error("edge 1x1 cache key mismatch");

    std::vector<RankInterval> intervals;
    intervals.reserve((size_t)count);
    for (uint64_t i = 0; i < count; i++) {
        RankInterval iv;
        iv.lo = read_u256_binary(is);
        iv.hi = read_u256_binary(is);
        intervals.push_back(iv);
    }
    return intervals;
}

}  // namespace quadtree_multi_internal

void precompute_edge_1x1_cache(int k, int jobs) {
    if (k <= 0) return;
    if (edge_1x1_cache_complete(k)) {
        std::cout << "edge1x1 cache for k=" << k << " already complete, skipping.\n";
        return;
    }
    QuadtreeIndex idx(k);

    // Build the shared (read-only) index memo once, then clone it per thread so
    // each thread gets its own FILE* seek position and combo_cache.
    std::cout << "Building index memo for k=" << k << "...\n" << std::flush;
    quadtree_internal::DirectIndexMemo shared_memo;
    quadtree_internal::build_direct_memo(k, shared_memo);
    std::cout << "Index memo ready.\n" << std::flush;

    // Compute the 4 cardinal masks via full enumeration, up to `jobs` at a time.
    // All 11 composite masks are intersections of cardinals and are derived cheaply.
    //
    // RAM WARNING: each in-flight worker clones the index memo (counts/sigs + a
    // private combo_cache), so peak RSS scales with `jobs`. With jobs=4, peak RSS
    // for k=5 was measured at ~37 GB (transient, ~2.5 min) -- the machine needs at
    // least that much free RAM. Pass --no-parallel (jobs=1) or --precompute-jobs N
    // to lower the peak; a fully serial run is estimated at ~10-15 GB for k=5.
    // See the "Parallelization" section of docs/architecture.md for details.
    // Clamp jobs to [1, 4]: there are only 4 cardinal masks, and jobs<1 is meaningless.
    if (jobs < 1) jobs = 1;
    if (jobs > 4) jobs = 4;
    constexpr int cardinals[] = { EDGE_N, EDGE_E, EDGE_S, EDGE_W };
    std::map<int, std::vector<RankInterval>> cardinal_ivs;
    std::mutex print_mtx;
    if (jobs >= 4) {
        std::cout << "Computing 4 cardinal masks in parallel...\n" << std::flush;
    } else if (jobs == 1) {
        std::cout << "Computing 4 cardinal masks serially (jobs=1)...\n" << std::flush;
    } else {
        std::cout << "Computing 4 cardinal masks, " << jobs
                  << " at a time...\n" << std::flush;
    }
    {
        auto run_one = [&idx, k, &shared_memo, &print_mtx](int c) {
            auto thread_memo = quadtree_internal::clone_direct_memo_for_thread(k, shared_memo);
            return compute_edge_1x1_intervals(idx, k, c, thread_memo, print_mtx);
        };
        // Sliding window: keep at most `jobs` futures in flight at a time.
        int next = 0;
        std::array<std::future<std::vector<RankInterval>>, 4> futures;
        for (; next < jobs && next < 4; next++) {
            futures[next] = std::async(std::launch::async, run_one, cardinals[next]);
        }
        for (int i = 0; i < 4; i++) {
            cardinal_ivs[cardinals[i]] = futures[i].get();
            if (next < 4) {
                futures[next] = std::async(std::launch::async, run_one, cardinals[next]);
                next++;
            }
        }
    }
    for (int c : cardinals) {
        write_edge_1x1_cache_file(k, c, cardinal_ivs[c]);
    }

    // Derive composite masks by intersecting the relevant cardinals.
    std::cout << "Deriving 11 composite masks via intersection...\n";
    for (int edge_mask = 1; edge_mask <= 15; edge_mask++) {
        if (edge_mask == EDGE_N || edge_mask == EDGE_E ||
            edge_mask == EDGE_S || edge_mask == EDGE_W) continue;

        std::vector<RankInterval> result;
        bool first = true;
        for (int c : cardinals) {
            if (!(edge_mask & c)) continue;
            if (first) { result = cardinal_ivs[c]; first = false; }
            else        result = intersect_intervals(result, cardinal_ivs[c]);
        }
        u256 count = 0;
        for (auto& iv : result) count += iv.hi - iv.lo;
        std::cout << "  edge1x1 k=" << k << " mask=" << edge_mask
                  << " intervals=" << result.size()
                  << " matches=" << u256_to_string(count) << "\n";
        write_edge_1x1_cache_file(k, edge_mask, result);
    }
}

bool edge_1x1_cache_complete(int k) {
    if (k <= 0) return true;
    for (int edge_mask = 1; edge_mask <= 15; edge_mask++)
        if (!edge_1x1_cache_file_present(k, edge_mask)) return false;
    return true;
}

bool edge_1x1_caches_available_for_grid(int width, int height) {
    int max_k = max_applicable_k(width, height);
    for (int k = 1; k <= max_k; k++)
        if (!edge_1x1_cache_complete(k)) return false;
    return true;
}
