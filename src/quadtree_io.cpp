#include "quadtree_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>

using namespace quadtree_internal;

// ------------------------------------------------------------------ QuadtreeIndex ctor

QuadtreeIndex::~QuadtreeIndex() {
    for (FILE* fh : children_fh_)
        if (fh) fclose(fh);
}

QuadtreeIndex::QuadtreeIndex(int k) : k_(k) {
    sig_table_.resize(k + 1);
    sig_to_idx_.resize(k + 1);
    sig_counts_.resize(k + 1);
    children_.resize(k + 1);
    child_starts_.resize(k + 1);
    child_parents_.resize(k + 1);
    child_ptrs_.resize(k + 1);
    child_keys_off_.assign(k + 1, 0);
    child_starts_off_.assign(k + 1, 0);
    children_fh_.assign(k + 1, nullptr);
    loaded_.assign(k + 1, false);

    bool all_present = true;
    for (int d = 0; d <= k_; d++) {
        if (!depth_files_present(d)) { all_present = false; break; }
    }

    if (!all_present) {
        std::cout << "Building index for k=" << k << " (streaming to disk)...\n";
        try {
            std::filesystem::create_directories(cache_directory());
        } catch (const std::exception& e) {
            std::cerr << "[cache dir create failed: " << e.what() << "]\n";
        }
        build();
        std::cout << "Cached per-depth files in " << cache_directory() << "\n";
        for (int d = 0; d <= k_; d++) unload_depth(d);
    }

    ensure_loaded(0);
}

// ------------------------------------------------------------------ build

void QuadtreeIndex::build() {
    int k = k_;
    u64 leaf_k = leaf_sig_int(k);
    sig_table_[k]   = {leaf_k};
    sig_to_idx_[k]  = {{leaf_k, 0}};
    sig_counts_[k]  = {{leaf_k, (u256)1}};
    loaded_[k] = true;

    save_depth_sigs(k);

    for (int depth = k - 1; depth >= 0; depth--) {
        int step = k - depth;
        std::cout << "  building depth " << depth
                  << "  (" << step << "/" << k << ")...\n" << std::flush;
        build_depth(depth);
        loaded_[depth] = true;

        std::cout << "    -> " << sig_counts_[depth].size() << " sigs\n" << std::flush;
        save_depth_sigs(depth);
        unload_depth(depth + 1);
    }
}

static constexpr int N_PASSES = 32;

// Build lb{k-depth}_children.bin (QTCH) from the child sig table + per-child
// subtree counts. Idempotent. Returns the sorted parent-sig vector.
std::vector<u64> quadtree_internal::build_children_file_for_depth(
    int k, int depth,
    const std::vector<u64>& child_sigs,
    const std::unordered_map<u64, u256>& child_counts)
{
    // If the file already exists, read sorted parents from its header and return
    // immediately — avoids the expensive O(n^4) in-RAM pre-pass entirely.
    auto children_path = children_file_path(k, depth);
    if (std::filesystem::exists(children_path)) {
        FILE* cf = fopen(children_path.c_str(), "rb");
        if (!cf) throw std::runtime_error("cannot open children file for read");
        char magic[4];
        read_exact_file(magic, 1, 4, cf);
        if (std::memcmp(magic, "QTCH", 4) != 0) { fclose(cf); throw std::runtime_error("bad QTCH magic"); }
        uint32_t ver, lb, n_child_sigs_file;
        read_exact_file(&ver,             4, 1, cf);
        read_exact_file(&lb,              4, 1, cf);
        read_exact_file(&n_child_sigs_file, 4, 1, cf);
        fseeko(cf, (off_t)(n_child_sigs_file * 8), SEEK_CUR);
        uint32_t n_parents;
        read_exact_file(&n_parents, 4, 1, cf);
        std::vector<u64> rel_parents(n_parents);
        read_exact_file(rel_parents.data(), 8, n_parents, cf);
        fclose(cf);
        std::vector<u64> sorted_parents(n_parents);
        for (uint32_t i = 0; i < n_parents; i++)
            sorted_parents[i] = relative_to_sig(rel_parents[i], depth);
        std::cout << "    reusing lb" << (k - depth) << "_children.bin\n";
        return sorted_parents;
    }

    int n = (int)child_sigs.size();

    std::vector<u256> sc_arr(n);
    for (int i = 0; i < n; i++) sc_arr[i] = child_counts.at(child_sigs[i]);

    std::vector<uint8_t> right_mat(n * n, 0);
    std::vector<uint8_t> bottom_mat(n * n, 0);
    for (int i = 0; i < n; i++) {
        u64 si = child_sigs[i];
        int a7 = sig_byte(si,7), a6 = sig_byte(si,6), a3 = sig_byte(si,3), a2 = sig_byte(si,2);
        for (int j = 0; j < n; j++) {
            u64 sj = child_sigs[j];
            if ((a7 - (int)sig_byte(sj,4) <= 1) && ((int)sig_byte(sj,5) - a6 <= 1)) right_mat[i*n+j] = 1;
            if ((a3 - (int)sig_byte(sj,0) <= 1) && ((int)sig_byte(sj,1) - a2 <= 1)) bottom_mat[i*n+j] = 1;
        }
    }

    // Count how many quads each parent sig has.
    std::unordered_map<u64, uint64_t> key_counts;
    for (int nw = 0; nw < n; nw++) {
        const uint8_t* rc_nw = &right_mat[nw*n];
        const uint8_t* bc_nw = &bottom_mat[nw*n];
        for (int ne = 0; ne < n; ne++) {
            if (!rc_nw[ne]) continue;
            const uint8_t* bc_ne = &bottom_mat[ne*n];
            for (int sw = 0; sw < n; sw++) {
                if (!bc_nw[sw]) continue;
                const uint8_t* rc_sw = &right_mat[sw*n];
                for (int se = 0; se < n; se++) {
                    if (!bc_ne[se]) continue;
                    if (!rc_sw[se]) continue;
                    u64 p = parent_sig(child_sigs[nw], child_sigs[ne], child_sigs[sw], child_sigs[se]);
                    key_counts[p]++;
                }
            }
        }
    }

    std::vector<u64> sorted_parents;
    sorted_parents.reserve(key_counts.size());
    for (auto& kv : key_counts) sorted_parents.push_back(kv.first);
    std::sort(sorted_parents.begin(), sorted_parents.end(), [depth](u64 a, u64 b) {
        return sig_to_relative(a, depth) < sig_to_relative(b, depth);
    });
    uint32_t n_parents = (uint32_t)sorted_parents.size();

    std::vector<uint64_t> ptrs(n_parents + 1);
    uint64_t total_keys = 0;
    for (uint32_t i = 0; i < n_parents; i++) {
        ptrs[i] = total_keys;
        total_keys += key_counts[sorted_parents[i]];
    }
    ptrs[n_parents] = total_keys;

    std::unordered_map<u64, uint32_t> parent_idx;
    parent_idx.reserve(n_parents);
    for (uint32_t i = 0; i < n_parents; i++) parent_idx[sorted_parents[i]] = i;

    {
        uint32_t n_child_sigs = (uint32_t)child_sigs.size();
        std::vector<u64> rel_child_sigs(n_child_sigs);
        for (uint32_t i = 0; i < n_child_sigs; i++)
            rel_child_sigs[i] = sig_to_relative(child_sigs[i], depth + 1);

        std::vector<u64> rel_parents(n_parents);
        for (uint32_t i = 0; i < n_parents; i++)
            rel_parents[i] = sig_to_relative(sorted_parents[i], depth);

        auto children_path = children_file_path(k, depth);
        FILE* cf = fopen(children_path.c_str(), "wb");
        if (!cf) throw std::runtime_error("cannot open children file for write");

        fwrite("QTCH", 1, 4, cf);
        uint32_t ver = 4, lb = (uint32_t)(k - depth);
        fwrite(&ver, 4, 1, cf);
        fwrite(&lb,  4, 1, cf);
        fwrite(&n_child_sigs,  4, 1, cf);
        fwrite(rel_child_sigs.data(), 8, n_child_sigs, cf);
        fwrite(&n_parents,   4, 1, cf);
        fwrite(rel_parents.data(), 8, n_parents, cf);
        fwrite(ptrs.data(), 8, n_parents + 1, cf);
        fwrite(&total_keys, 8, 1, cf);

        uint64_t header_bytes = 4 + 4 + 4 + 4
            + (uint64_t)n_child_sigs * 8
            + 4
            + (uint64_t)n_parents * 8
            + ((uint64_t)n_parents + 1) * 8
            + 8;
        uint64_t keys_off    = header_bytes;
        uint64_t starts_off  = keys_off + total_keys * 8;
        uint64_t file_size   = starts_off + total_keys * 32;

        if (fseeko(cf, (off_t)(file_size - 1), SEEK_SET) != 0)
            throw std::runtime_error("fseeko failed on pre-extend");
        { uint8_t z = 0; fwrite(&z, 1, 1, cf); }

        for (int pass = 0; pass < N_PASSES; pass++) {
            std::unordered_map<uint32_t, std::vector<u64>> pass_keys;

            for (int nw = 0; nw < n; nw++) {
                const uint8_t* rc_nw = &right_mat[nw*n];
                const uint8_t* bc_nw = &bottom_mat[nw*n];
                for (int ne = 0; ne < n; ne++) {
                    if (!rc_nw[ne]) continue;
                    const uint8_t* bc_ne = &bottom_mat[ne*n];
                    for (int sw = 0; sw < n; sw++) {
                        if (!bc_nw[sw]) continue;
                        const uint8_t* rc_sw = &right_mat[sw*n];
                        for (int se = 0; se < n; se++) {
                            if (!bc_ne[se]) continue;
                            if (!rc_sw[se]) continue;
                            u64 p = parent_sig(child_sigs[nw], child_sigs[ne], child_sigs[sw], child_sigs[se]);
                            auto it = parent_idx.find(p);
                            if (it == parent_idx.end()) continue;
                            if ((int)(it->second % N_PASSES) != pass) continue;
                            pass_keys[it->second].push_back(pack_key(nw, ne, sw, se));
                        }
                    }
                }
            }

            for (auto& kv : pass_keys) {
                uint32_t pidx = kv.first;
                auto& keys = kv.second;
                std::sort(keys.begin(), keys.end());

                std::vector<u256> starts(keys.size());
                u256 running = 0;
                for (size_t i = 0; i < keys.size(); i++) {
                    starts[i] = running;
                    u64 key = keys[i];
                    running += sc_arr[key & 0xFFFF]
                             * sc_arr[(key >> 16) & 0xFFFF]
                             * sc_arr[(key >> 32) & 0xFFFF]
                             * sc_arr[(key >> 48) & 0xFFFF];
                }

                if (fseeko(cf, (off_t)(keys_off + ptrs[pidx] * 8), SEEK_SET) != 0)
                    throw std::runtime_error("fseeko failed writing keys");
                fwrite(keys.data(), 8, keys.size(), cf);

                if (fseeko(cf, (off_t)(starts_off + ptrs[pidx] * 32), SEEK_SET) != 0)
                    throw std::runtime_error("fseeko failed writing starts");
                for (const u256& sv : starts) {
                    fwrite(sv.limbs, 8, 4, cf);
                }
            }

            std::cout << "    pass " << (pass+1) << "/" << N_PASSES << "\n";
        }

        fclose(cf);
        std::cout << "    wrote lb" << (k - depth) << "_children.bin\n";
    }

    return sorted_parents;
}

void QuadtreeIndex::build_depth(int depth) {
    auto& sc_next  = sig_counts_[depth + 1];
    auto& s2i_next = sig_to_idx_[depth + 1];

    std::vector<u64> child_sigs;
    child_sigs.reserve(sc_next.size());
    for (auto& kv : sc_next) child_sigs.push_back(kv.first);
    int child_depth = depth + 1;
    std::sort(child_sigs.begin(), child_sigs.end(), [child_depth](u64 a, u64 b) {
        return sig_to_relative(a, child_depth) < sig_to_relative(b, child_depth);
    });
    int n = (int)child_sigs.size();

    s2i_next.clear();
    for (int i = 0; i < n; i++) s2i_next[child_sigs[i]] = i;
    sig_table_[depth + 1] = child_sigs;

    std::vector<u256> sc_arr(n);
    for (int i = 0; i < n; i++) sc_arr[i] = sc_next[child_sigs[i]];

    std::vector<uint8_t> right_mat(n * n, 0);
    std::vector<uint8_t> bottom_mat(n * n, 0);
    for (int i = 0; i < n; i++) {
        u64 si = child_sigs[i];
        int a7 = sig_byte(si,7), a6 = sig_byte(si,6), a3 = sig_byte(si,3), a2 = sig_byte(si,2);
        for (int j = 0; j < n; j++) {
            u64 sj = child_sigs[j];
            if ((a7 - (int)sig_byte(sj,4) <= 1) && ((int)sig_byte(sj,5) - a6 <= 1)) right_mat[i*n+j] = 1;
            if ((a3 - (int)sig_byte(sj,0) <= 1) && ((int)sig_byte(sj,1) - a2 <= 1)) bottom_mat[i*n+j] = 1;
        }
    }

    std::unordered_map<u64, u256> new_counts;
    new_counts[leaf_sig_int(depth)] = 1;

    for (int nw = 0; nw < n; nw++) {
        const uint8_t* rc_nw = &right_mat[nw*n];
        const uint8_t* bc_nw = &bottom_mat[nw*n];
        u256 nw_c = sc_arr[nw];
        for (int ne = 0; ne < n; ne++) {
            if (!rc_nw[ne]) continue;
            const uint8_t* bc_ne = &bottom_mat[ne*n];
            u256 ne_c = sc_arr[ne];
            for (int sw = 0; sw < n; sw++) {
                if (!bc_nw[sw]) continue;
                const uint8_t* rc_sw = &right_mat[sw*n];
                u256 sw_c = sc_arr[sw];
                for (int se = 0; se < n; se++) {
                    if (!bc_ne[se]) continue;
                    if (!rc_sw[se]) continue;
                    u64 p = parent_sig(child_sigs[nw], child_sigs[ne], child_sigs[sw], child_sigs[se]);
                    new_counts[p] += nw_c * ne_c * sw_c * sc_arr[se];
                }
            }
        }
    }

    sig_counts_[depth] = std::move(new_counts);
    std::vector<u64> sigs;
    sigs.reserve(sig_counts_[depth].size());
    for (auto& kv : sig_counts_[depth]) sigs.push_back(kv.first);
    std::sort(sigs.begin(), sigs.end(), [depth](u64 a, u64 b) {
        return sig_to_relative(a, depth) < sig_to_relative(b, depth);
    });
    sig_table_[depth] = sigs;
    sig_to_idx_[depth].clear();
    for (size_t i = 0; i < sigs.size(); i++) sig_to_idx_[depth][sigs[i]] = (int)i;

    // Write (or reuse) the QTCH children file for this depth level.
    build_children_file_for_depth(k_, depth, child_sigs, sig_counts_[depth + 1]);

    children_[depth].clear();
    child_starts_[depth].clear();
}

// ------------------------------------------------------------------ per-depth save / load

std::filesystem::path QuadtreeIndex::depth_sigs_file(int depth) const {
    return cache_directory() / ("k" + std::to_string(k_) + "_d" + std::to_string(depth) + "_sigs.bin");
}

std::filesystem::path QuadtreeIndex::depth_children_file(int depth) const {
    return cache_directory() / ("lb" + std::to_string(k_ - depth) + "_children.bin");
}

bool QuadtreeIndex::depth_files_present(int depth) const {
    if (!std::filesystem::exists(depth_sigs_file(depth))) return false;
    if (depth < k_ && !std::filesystem::exists(depth_children_file(depth))) return false;
    return true;
}

void QuadtreeIndex::save_depth_sigs(int d) const {
    auto path = depth_sigs_file(d);
    std::ofstream os(path, std::ios::binary);
    if (!os) throw std::runtime_error("cannot open sigs cache for write");
    os.write(kSigsCacheMagic, 4);
    write_pod(os, kSigsCacheVersion);
    uint32_t kk = (uint32_t)k_, dd = (uint32_t)d;
    write_pod(os, kk);
    write_pod(os, dd);

    const auto& table = sig_table_[d];
    uint32_t n_sigs = (uint32_t)table.size();
    write_pod(os, n_sigs);
    for (u64 s : table) write_pod(os, s);
    for (u64 s : table) write_u256_binary(os, sig_counts_[d].at(s));
}

void QuadtreeIndex::load_depth(int d) const {
    {
        auto path = depth_sigs_file(d);
        std::ifstream is(path, std::ios::binary);
        if (!is) throw std::runtime_error("cannot open sigs cache for read");
        char magic[4];
        is.read(magic, 4);
        if (std::memcmp(magic, kSigsCacheMagic, 4) != 0) throw std::runtime_error("bad sigs magic");
        uint32_t version, kk, dd;
        read_pod(is, version);
        read_pod(is, kk);
        read_pod(is, dd);
        if (version != kSigsCacheVersion) throw std::runtime_error("bad version");
        if ((int)kk != k_) throw std::runtime_error("k mismatch");
        if ((int)dd != d)  throw std::runtime_error("depth mismatch");

        uint32_t n_sigs;
        read_pod(is, n_sigs);
        std::vector<u64> table(n_sigs);
        for (uint32_t i = 0; i < n_sigs; i++) read_pod(is, table[i]);
        sig_table_[d] = table;
        sig_to_idx_[d].clear();
        sig_to_idx_[d].reserve(n_sigs);
        for (uint32_t i = 0; i < n_sigs; i++) sig_to_idx_[d][table[i]] = (int)i;
        sig_counts_[d].clear();
        sig_counts_[d].reserve(n_sigs);
        for (uint32_t i = 0; i < n_sigs; i++) sig_counts_[d][table[i]] = read_u256_binary(is);
    }

    if (d < k_) {
        auto path = depth_children_file(d);
        FILE* cf = fopen(path.c_str(), "rb");
        if (!cf) throw std::runtime_error("cannot open children file for read");

        char magic[4];
        read_exact_file(magic, 1, 4, cf);
        if (std::memcmp(magic, "QTCH", 4) != 0) { fclose(cf); throw std::runtime_error("bad children magic"); }
        uint32_t ver, lb, n_child_sigs, n_parents;
        read_exact_file(&ver,          4, 1, cf);
        read_exact_file(&lb,           4, 1, cf);
        (void)ver;
        if ((int)lb != k_ - d) { fclose(cf); throw std::runtime_error("levels_below mismatch in children file"); }

        read_exact_file(&n_child_sigs, 4, 1, cf);
        std::vector<u64> rel_child_sigs(n_child_sigs);
        read_exact_file(rel_child_sigs.data(), 8, n_child_sigs, cf);

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
        uint64_t keys_off   = header_bytes;
        uint64_t starts_off = keys_off + total_keys * 8;

        std::vector<u64> abs_child_sigs(n_child_sigs);
        for (uint32_t i = 0; i < n_child_sigs; i++)
            abs_child_sigs[i] = relative_to_sig(rel_child_sigs[i], d + 1);
        sig_table_[d + 1] = abs_child_sigs;
        sig_to_idx_[d + 1].clear();
        sig_to_idx_[d + 1].reserve(n_child_sigs);
        for (uint32_t i = 0; i < n_child_sigs; i++)
            sig_to_idx_[d + 1][abs_child_sigs[i]] = (int)i;

        std::vector<std::tuple<u64,uint64_t,uint64_t>> par_ptr(n_parents);
        for (uint32_t i = 0; i < n_parents; i++)
            par_ptr[i] = { relative_to_sig(rel_parents[i], d), ptrs[i], ptrs[i+1] };
        std::sort(par_ptr.begin(), par_ptr.end());
        std::vector<u64>      parents(n_parents);
        std::vector<uint64_t> sorted_ptrs(2 * n_parents);
        for (uint32_t i = 0; i < n_parents; i++) {
            parents[i]             = std::get<0>(par_ptr[i]);
            sorted_ptrs[2 * i]     = std::get<1>(par_ptr[i]);
            sorted_ptrs[2 * i + 1] = std::get<2>(par_ptr[i]);
        }

        child_parents_[d]    = std::move(parents);
        child_ptrs_[d]       = std::move(sorted_ptrs);
        child_keys_off_[d]   = keys_off;
        child_starts_off_[d] = starts_off;

        if (children_fh_[d]) fclose(children_fh_[d]);
        children_fh_[d] = cf;

        children_[d].clear();
        child_starts_[d].clear();
    }

    loaded_[d] = true;
}

void QuadtreeIndex::fetch_children_for_sig(int depth, u64 sig) const {
    if (children_[depth].count(sig)) return;

    FILE* cf = children_fh_[depth];
    if (!cf) throw std::runtime_error("children file not open for depth");

    const auto& parents = child_parents_[depth];
    auto it = std::lower_bound(parents.begin(), parents.end(), sig);
    if (it == parents.end() || *it != sig)
        throw std::out_of_range("sig not found in child_parents_");
    uint32_t i = (uint32_t)(it - parents.begin());

    uint64_t lo  = child_ptrs_[depth][2 * i];
    uint64_t cnt = child_ptrs_[depth][2 * i + 1] - lo;

    std::vector<u64> k_vec(cnt);
    fseeko(cf, (off_t)(child_keys_off_[depth] + lo * 8), SEEK_SET);
    read_exact_file(k_vec.data(), 8, cnt, cf);

    std::vector<u256> s_vec(cnt);
    fseeko(cf, (off_t)(child_starts_off_[depth] + lo * 32), SEEK_SET);
    for (uint64_t j = 0; j < cnt; j++) {
        read_exact_file(s_vec[j].limbs, 8, 4, cf);
    }

    children_[depth][sig]     = std::move(k_vec);
    child_starts_[depth][sig] = std::move(s_vec);
}

void QuadtreeIndex::unload_depth(int d) const {
    sig_table_[d].clear();      sig_table_[d].shrink_to_fit();
    sig_to_idx_[d].clear();
    sig_counts_[d].clear();
    children_[d].clear();
    child_starts_[d].clear();
    child_parents_[d].clear();  child_parents_[d].shrink_to_fit();
    child_ptrs_[d].clear();     child_ptrs_[d].shrink_to_fit();
    child_keys_off_[d]   = 0;
    child_starts_off_[d] = 0;
    if (children_fh_[d]) { fclose(children_fh_[d]); children_fh_[d] = nullptr; }
    loaded_[d] = false;
}

void QuadtreeIndex::ensure_loaded(int d) const {
    if (!loaded_[d]) load_depth(d);
}
