#include "quadtree.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Parse a non-negative decimal into u256.
static bool parse_u256(const std::string& s, u256& out) {
    if (s.empty()) return false;
    u256 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (unsigned)(c - '0');
    }
    out = v;
    return true;
}

// Compare QNode trees structurally (depth + recursive children).
static bool trees_equal(const QNode& a, const QNode& b) {
    if (a.depth != b.depth) return false;
    if (a.is_leaf() != b.is_leaf()) return false;
    if (a.is_leaf()) return true;
    for (int i = 0; i < 4; i++)
        if (!trees_equal(*a.children[i], *b.children[i])) return false;
    return true;
}

static bool load_mask_file(const std::string& path, int k, Mask& out) {
    std::ifstream is(path);
    if (!is) { std::cerr << "cannot open mask file: " << path << "\n"; return false; }
    int sz = 1 << k;
    out = Mask::free_mask(k);
    std::string line;
    int r = 0;
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        int c = 0, v;
        while (ls >> v) {
            if (r >= sz || c >= sz) {
                std::cerr << "mask too large for k=" << k << "\n"; return false;
            }
            out.grid[r][c] = v;
            c++;
        }
        if (c != sz) {
            std::cerr << "mask row " << r << " has " << c << " entries, expected " << sz << "\n";
            return false;
        }
        r++;
    }
    if (r != sz) {
        std::cerr << "mask has " << r << " rows, expected " << sz << "\n";
        return false;
    }
    return true;
}

static bool load_root_specs_file(const std::string& path, std::vector<RootSpec>& out,
                                 bool& has_first_rank, u256& first_rank) {
    std::ifstream is(path);
    if (!is) { std::cerr << "cannot open layout file: " << path << "\n"; return false; }
    out.clear();
    has_first_rank = false;
    std::string line;
    int line_no = 0;
    int root_no = 0;
    while (std::getline(is, line)) {
        line_no++;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        RootSpec spec;
        if (!(ls >> spec.x >> spec.y >> spec.k)) {
            std::cerr << "bad layout line " << line_no << ": expected x y k [rank on first root only]\n";
            return false;
        }
        std::string extra;
        if (ls >> extra) {
            if (root_no != 0) {
                std::cerr << "bad layout line " << line_no
                          << ": rank may only be specified on the first root\n";
                return false;
            }
            if (!parse_u256(extra, first_rank)) {
                std::cerr << "bad layout line " << line_no << ": invalid first-root rank\n";
                return false;
            }
            std::string trailing;
            if (ls >> trailing) {
                std::cerr << "bad layout line " << line_no << ": too many fields\n";
                return false;
            }
            has_first_rank = true;
        }
        out.push_back(spec);
        root_no++;
    }
    return true;
}

// Does `tree` (rendered at size 2^k) match `mask.grid`?
// 0 cells are unconstrained; positive cells must equal the rendered tile size.
static bool tree_matches_mask(const QNode& tree, int k, const Mask& mask) {
    auto grid = render_grid(tree, k);
    int sz = 1 << k;
    for (int r = 0; r < sz; r++) {
        for (int c = 0; c < sz; c++) {
            int w = mask.grid[r][c];
            if (w && w != grid[r][c]) return false;
        }
    }
    return true;
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <k> [--count-only] [--print-all] "
              << "[--unrank N] [--unrank-direct N] [--verify-index] [--verify-direct] "
              << "[--mask FILE] [--mask-verify FILE] [--mask-print FILE]\n"
              << "       " << prog << " --multi M N [--layout FILE] [--seed S] "
              << "[--attempts N] [--max-attempts N] [--no-limit] [--print-grid] [--validate-only] [--outer-edge-1x1] "
              << "[--skip-1x1-precomputation] [--greedy-cover] [--first-rank N [--first-k K]]\n"
              << "         --attempts N / --max-attempts N: stop after N failed attempts (default 128)\n"
              << "         --no-limit: keep searching indefinitely until success (Ctrl-C to abort)\n"
              << "         --first-rank N: fix the first root's quadtree to rank N\n"
              << "           requires --layout or --first-k K (placed at origin, rest filled randomly)\n"
              << "       " << prog << " --precompute-edge-1x1 K "
              << "[--precompute-jobs N | --no-parallel]\n"
              << "         --precompute-jobs N: run N of the 4 cardinal masks at once "
              << "(1-4, default 4);\n"
              << "         lower N uses less RAM (k=5 needs ~37 GB at N=4). "
              << "--no-parallel = N=1\n"
              << "       layout lines are: x y k; first line may be: x y k rank\n"
              << "       --greedy-cover: use deterministic row-major largest-fitting-square cover\n"
              << "         (fewest tiles; ignored if --layout is also given)\n"
              << "       " << prog << " --multi-self-test\n";
}

static int run_multi_self_test() {
    int failures = 0;
    auto expect = [&](const std::string& name, const MultiQuadtreeTiling& t, bool want) {
        std::vector<std::string> errors;
        bool got = validate_multi_quadtree(t, &errors);
        bool ok = (got == want);
        std::cout << "[" << (ok ? "OK" : "FAIL") << "] " << name << "\n";
        if (!ok) {
            std::cout << "  expected " << (want ? "valid" : "invalid")
                      << ", got " << (got ? "valid" : "invalid") << "\n";
            for (const auto& e : errors) std::cout << "  " << e << "\n";
            failures++;
        }
    };

    expect("aligned equal 1x1 roots",
        {2, 1, {
            {{0, 0, 0}, make_leaf(0)},
            {{1, 0, 0}, make_leaf(0)}
        }},
        true);

    expect("half-offset equal 2x2 roots",
        {4, 3, {
            {{0, 0, 1}, make_leaf(0)},
            {{2, 1, 1}, make_leaf(0)},
            {{2, 0, 0}, make_leaf(0)},
            {{3, 0, 0}, make_leaf(0)},
            {{0, 2, 0}, make_leaf(0)},
            {{1, 2, 0}, make_leaf(0)}
        }},
        true);

    expect("aligned 2:1 contact",
        {4, 2, {
            {{0, 0, 1}, make_leaf(0)},
            {{2, 0, 0}, make_leaf(0)},
            {{2, 1, 0}, make_leaf(0)},
            {{3, 0, 0}, make_leaf(0)},
            {{3, 1, 0}, make_leaf(0)}
        }},
        true);

    expect("centered 2:1 contact rejected",
        {6, 4, {
            {{0, 0, 2}, make_leaf(0)},
            {{4, 1, 1}, make_leaf(0)},
            {{4, 0, 0}, make_leaf(0)},
            {{5, 0, 0}, make_leaf(0)},
            {{4, 3, 0}, make_leaf(0)},
            {{5, 3, 0}, make_leaf(0)}
        }},
        false);

    expect("larger than 2:1 rejected",
        {5, 4, {
            {{0, 0, 2}, make_leaf(0)},
            {{4, 0, 0}, make_leaf(0)},
            {{4, 1, 0}, make_leaf(0)},
            {{4, 2, 0}, make_leaf(0)},
            {{4, 3, 0}, make_leaf(0)}
        }},
        false);

    return failures == 0 ? 0 : 1;
}

static int run_multi_mode(int argc, char** argv) {
    if (argc < 4) { usage(argv[0]); return 1; }

    int width = std::atoi(argv[2]);
    int height = std::atoi(argv[3]);
    if (width <= 0 || height <= 0) {
        std::cerr << "--multi dimensions must be positive\n";
        return 1;
    }

    std::string layout_file;
    uint64_t seed = (uint64_t)std::chrono::high_resolution_clock::now()
        .time_since_epoch().count();
    int attempts = 128;
    bool print_grid_flag = false;
    bool validate_only = false;
    bool has_first_rank = false;
    bool has_first_k = false;
    bool outer_edge_1x1 = false;
    bool skip_1x1_precomputation = false;
    bool greedy_cover = false;
    u256 first_rank = 0;
    int first_k = -1;

    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--layout") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            layout_file = argv[++i];
        } else if (a == "--seed") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            seed = (uint64_t)std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--attempts" || a == "--max-attempts") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            attempts = std::atoi(argv[++i]);
        } else if (a == "--no-limit") {
            attempts = -1;
        } else if (a == "--print-grid") {
            print_grid_flag = true;
        } else if (a == "--validate-only") {
            validate_only = true;
        } else if (a == "--outer-edge-1x1") {
            outer_edge_1x1 = true;
        } else if (a == "--skip-1x1-precomputation") {
            skip_1x1_precomputation = true;
        } else if (a == "--greedy-cover") {
            greedy_cover = true;
        } else if (a == "--first-rank") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            if (!parse_u256(argv[++i], first_rank)) {
                std::cerr << "bad --first-rank N\n"; return 1;
            }
            has_first_rank = true;
        } else if (a == "--first-k") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            first_k = std::atoi(argv[++i]);
            if (first_k < 0) { std::cerr << "bad --first-k K\n"; return 1; }
            has_first_k = true;
        } else {
            usage(argv[0]); return 1;
        }
    }

    if (has_first_rank && layout_file.empty() && !has_first_k) {
        std::cerr << "--first-rank requires --layout or --first-k K\n";
        return 1;
    }
    if (has_first_k && layout_file.empty() && !has_first_rank) {
        std::cerr << "--first-k requires --first-rank N\n";
        return 1;
    }
    if (has_first_k && !layout_file.empty()) {
        std::cerr << "--first-k cannot be combined with --layout\n";
        return 1;
    }
    if (has_first_k) {
        int side = 1 << first_k;
        if (side > width || side > height) {
            std::cerr << "--first-k " << first_k << " gives size " << side
                      << " which exceeds grid " << width << "x" << height << "\n";
            return 1;
        }
    }

    std::vector<RootSpec> specs;
    if (!layout_file.empty()) {
        bool file_has_first_rank = false;
        u256 file_first_rank = 0;
        if (!load_root_specs_file(layout_file, specs, file_has_first_rank, file_first_rank)) return 1;
        if (file_has_first_rank && has_first_rank) {
            std::cerr << "--first-rank conflicts with rank specified in layout file\n";
            return 1;
        }
        if (file_has_first_rank) {
            has_first_rank = true;
            first_rank = file_first_rank;
        }
    }

    if (validate_only) {
        if (specs.empty()) specs = random_root_cover(width, height, seed);
        std::vector<std::string> errors;
        bool ok = validate_root_cover(width, height, specs, &errors);
        std::cout << "[" << (ok ? "OK" : "FAIL") << "] root cover: "
                  << specs.size() << " roots for " << width << "x" << height << "\n";
        for (const auto& e : errors) std::cout << "  " << e << "\n";
        return ok ? 0 : 1;
    }

    try {
        std::string err;
        bool unlimited = (attempts < 0);
        auto progress = [unlimited](int attempt) {
            std::cerr << "\r    attempt " << attempt
                      << (unlimited ? " (no limit, Ctrl-C to abort)..." : "...")
                      << "    " << std::flush;
        };
        RootSpec seed_root_spec{0, 0, first_k};
        auto tiling = generate_multi_quadtree(width, height, specs, seed, attempts, &err,
            has_first_rank ? &first_rank : nullptr, outer_edge_1x1,
            skip_1x1_precomputation, greedy_cover, progress,
            has_first_k ? &seed_root_spec : nullptr);
        std::cerr << "\r" << std::string(60, ' ') << "\r" << std::flush;
        std::vector<std::string> errors;
        bool ok = validate_multi_quadtree(tiling, &errors);
        if (outer_edge_1x1)
            ok = validate_outer_edges_1x1(tiling, &errors) && ok;
        std::cout << "[" << (ok ? "OK" : "FAIL") << "] multi-quadtree "
                  << width << "x" << height << ", roots=" << tiling.roots.size()
                  << ", seed=" << seed << "\n";
        for (size_t i = 0; i < tiling.roots.size(); i++) {
            const auto& s = tiling.roots[i].spec;
            std::cout << "  root " << i << ": x=" << s.x
                      << " y=" << s.y << " k=" << s.k
                      << " size=" << (1 << s.k) << "\n";
        }
        for (const auto& e : errors) std::cout << "  " << e << "\n";
        if (print_grid_flag) print_grid(render_grid(tiling));
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (std::strcmp(argv[1], "--precompute-edge-1x1") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        int pk = std::atoi(argv[2]);
        if (pk <= 0) {
            std::cerr << "--precompute-edge-1x1 K requires K >= 1\n";
            return 1;
        }
        // Optional concurrency control. jobs caps how many of the 4 cardinal
        // masks run at once; peak RAM scales with it (k=5 jobs=4 needs ~37 GB).
        int jobs = 4;
        for (int i = 3; i < argc; i++) {
            if (std::strcmp(argv[i], "--no-parallel") == 0) {
                jobs = 1;
            } else if (std::strcmp(argv[i], "--precompute-jobs") == 0) {
                if (i + 1 >= argc) {
                    std::cerr << "--precompute-jobs requires a value (1-4)\n";
                    return 1;
                }
                jobs = std::atoi(argv[++i]);
                if (jobs < 1 || jobs > 4) {
                    std::cerr << "--precompute-jobs must be between 1 and 4\n";
                    return 1;
                }
            } else {
                std::cerr << "unknown option for --precompute-edge-1x1: "
                          << argv[i] << "\n";
                usage(argv[0]);
                return 1;
            }
        }
        precompute_edge_1x1_cache(pk, jobs);
        return 0;
    }

    if (std::strcmp(argv[1], "--multi-self-test") == 0)
        return run_multi_self_test();

    if (std::strcmp(argv[1], "--multi") == 0)
        return run_multi_mode(argc, argv);

    int k = std::atoi(argv[1]);
    if (k < 0) { usage(argv[0]); return 1; }

    bool count_only      = false;
    bool print_all       = false;
    bool verify_index    = false;
    bool verify_direct   = false;
    bool do_unrank       = false;
    bool do_unrank_direct= false;
    u256 unrank_n        = 0;
    std::string mask_file;
    std::string mask_verify_file;
    std::string mask_print_file;

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--count-only")           count_only = true;
        else if (a == "--print-all")       print_all = true;
        else if (a == "--verify-index")    verify_index = true;
        else if (a == "--verify-direct")   verify_direct = true;
        else if (a == "--unrank") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            if (!parse_u256(argv[++i], unrank_n)) {
                std::cerr << "bad --unrank N\n"; return 1;
            }
            do_unrank = true;
        } else if (a == "--unrank-direct") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            if (!parse_u256(argv[++i], unrank_n)) {
                std::cerr << "bad --unrank-direct N\n"; return 1;
            }
            do_unrank_direct = true;
        } else if (a == "--mask") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            mask_file = argv[++i];
        } else if (a == "--mask-verify") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            mask_verify_file = argv[++i];
        } else if (a == "--mask-print") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            mask_print_file = argv[++i];
        } else {
            usage(argv[0]); return 1;
        }
    }

    int grid_size = 1 << k;

    if (!mask_file.empty()) {
        Mask mask;
        if (!load_mask_file(mask_file, k, mask)) return 1;
        QuadtreeIndex idx(k);
        std::vector<RankInterval> ivs;
        u256 cnt = idx.enumerate_matching(mask, [&](RankInterval iv) { ivs.push_back(iv); });
        std::cout << "Matches: " << u256_to_string(cnt) << "\n";
        std::cout << "Intervals (" << ivs.size() << "):\n";
        for (auto& iv : ivs) {
            std::cout << "  [" << u256_to_string(iv.lo) << ", " << u256_to_string(iv.hi) << ")\n";
        }
        return 0;
    }

    if (!mask_print_file.empty()) {
        Mask mask;
        if (!load_mask_file(mask_print_file, k, mask)) return 1;
        QuadtreeIndex idx(k);
        std::vector<RankInterval> ivs;
        idx.enumerate_matching(mask, [&](RankInterval iv) { ivs.push_back(iv); });
        for (auto& iv : ivs) {
            for (u256 n = iv.lo; n < iv.hi; n++) {
                auto tree = idx.unrank(n);
                std::cout << "\n--- Match #" << u256_to_string(n) << " ---\n";
                print_grid(render_grid(*tree, k));
            }
        }
        return 0;
    }

    if (!mask_verify_file.empty()) {
        Mask mask;
        if (!load_mask_file(mask_verify_file, k, mask)) return 1;
        QuadtreeIndex idx(k);
        std::vector<RankInterval> ivs;
        u256 cnt = idx.enumerate_matching(mask, [&](RankInterval iv) { ivs.push_back(iv); });

        // First check: every emitted index, when unranked, must match the mask.
        int bad = 0;
        u256 first_bad = 0;
        for (auto& iv : ivs) {
            for (u256 n = iv.lo; n < iv.hi; n++) {
                auto t = idx.unrank(n);
                if (!tree_matches_mask(*t, k, mask)) {
                    if (bad == 0) first_bad = n;
                    bad++;
                }
            }
        }
        if (bad) {
            std::cout << "[FAIL] " << bad << " emitted indices do NOT match mask; first bad = "
                      << u256_to_string(first_bad) << "\n";
            auto t = idx.unrank(first_bad);
            print_grid(render_grid(*t, k));
            return 1;
        }

        // Brute-force: enumerate all trees, filter by mask, rank them.
        auto trees = all_balanced_quadtrees(k);
        std::vector<u256> brute;
        for (auto& t : trees) {
            if (tree_matches_mask(*t, k, mask))
                brute.push_back(idx.rank(*t));
        }
        std::sort(brute.begin(), brute.end());

        // Expand intervals.
        std::vector<u256> from_ivs;
        for (auto& iv : ivs)
            for (u256 n = iv.lo; n < iv.hi; n++) from_ivs.push_back(n);

        bool ok = (brute.size() == from_ivs.size());
        if (ok) {
            for (size_t i = 0; i < brute.size(); i++) {
                if (brute[i] != from_ivs[i]) { ok = false; break; }
            }
        }
        std::cout << "[" << (ok ? "OK" : "FAIL") << "] mask match: "
                  << "constrained=" << u256_to_string(cnt)
                  << ", brute=" << brute.size()
                  << ", intervals=" << ivs.size() << "\n";
        if (!ok) {
            std::cout << "constrained list: ";
            for (auto n : from_ivs) std::cout << u256_to_string(n) << " ";
            std::cout << "\nbrute list:       ";
            for (auto n : brute)    std::cout << u256_to_string(n) << " ";
            std::cout << "\n";
        }
        return ok ? 0 : 1;
    }

    if (do_unrank_direct) {
        QuadtreeIndex idx(k);
        auto tree = idx.unrank_direct(unrank_n);
        std::cout << "Tiling #" << u256_to_string(unrank_n)
                  << " of " << u256_to_string(idx.total_direct())
                  << " (k=" << k << ", direct):\n";
        print_grid(render_grid(*tree, k));
        return 0;
    }

    if (do_unrank) {
        QuadtreeIndex idx(k);
        auto tree = idx.unrank(unrank_n);
        std::cout << "Tiling #" << u256_to_string(unrank_n)
                  << " of " << u256_to_string(idx.total())
                  << " (k=" << k << "):\n";
        print_grid(render_grid(*tree, k));
        return 0;
    }

    if (verify_index) {
        auto trees = all_balanced_quadtrees(k);
        QuadtreeIndex idx(k);
        std::vector<u256> ranks;
        ranks.reserve(trees.size());
        int errors = 0;
        for (auto& t : trees) {
            u256 r = idx.rank(*t);
            ranks.push_back(r);
            auto u = idx.unrank(r);
            if (!trees_equal(*u, *t)) errors++;
        }
        std::vector<u256> sorted_ranks = ranks;
        std::sort(sorted_ranks.begin(), sorted_ranks.end());
        bool coverage_ok = true;
        for (size_t i = 0; i < sorted_ranks.size(); i++) {
            if (sorted_ranks[i] != (u256)i) { coverage_ok = false; break; }
        }
        const char* status = (errors == 0 && coverage_ok) ? "OK" : "FAIL";
        std::cout << "[" << status << "] " << trees.size() << " trees: "
                  << errors << " roundtrip errors, ranks "
                  << (coverage_ok ? "cover" : "do NOT cover")
                  << " [0, " << trees.size() << ")\n";
        return (errors == 0 && coverage_ok) ? 0 : 1;
    }

    if (verify_direct) {
        auto trees = all_balanced_quadtrees(k);
        QuadtreeIndex idx(k);
        std::vector<u256> ranks;
        ranks.reserve(trees.size());
        int errors = 0;
        for (auto& t : trees) {
            u256 r = idx.rank_direct(*t);
            ranks.push_back(r);
            auto u = idx.unrank_direct(r);
            if (!trees_equal(*u, *t)) errors++;
        }
        std::vector<u256> sorted_ranks = ranks;
        std::sort(sorted_ranks.begin(), sorted_ranks.end());
        bool coverage_ok = true;
        for (size_t i = 0; i < sorted_ranks.size(); i++) {
            if (sorted_ranks[i] != (u256)i) { coverage_ok = false; break; }
        }
        const char* status = (errors == 0 && coverage_ok) ? "OK" : "FAIL";
        std::cout << "[" << status << "] " << trees.size() << " trees (direct): "
                  << errors << " roundtrip errors, ranks "
                  << (coverage_ok ? "cover" : "do NOT cover")
                  << " [0, " << trees.size() << ")\n";
        return (errors == 0 && coverage_ok) ? 0 : 1;
    }

    if (count_only) {
        u256 c = count_balanced_quadtrees(k);
        std::cout << "Counted " << u256_to_string(c)
                  << " balanced tilings for a " << grid_size << "x" << grid_size
                  << " grid (k=" << k << ")\n";
        return 0;
    }

    // Default: enumerate AND fast-count, compare.
    auto trees = all_balanced_quadtrees(k);
    u256 fast = count_balanced_quadtrees(k);
    const char* match = ((u256)trees.size() == fast) ? "OK" : "MISMATCH";
    std::cout << "Found " << trees.size() << " balanced tilings for a "
              << grid_size << "x" << grid_size << " grid (k=" << k << ")\n";
    std::cout << "Fast count: " << u256_to_string(fast) << " [" << match << "]\n";

    if (print_all) {
        for (size_t i = 0; i < trees.size(); i++) {
            std::cout << "\n--- Tiling " << (i + 1) << " ---\n";
            print_grid(render_grid(*trees[i], k));
        }
    }
    return 0;
}
