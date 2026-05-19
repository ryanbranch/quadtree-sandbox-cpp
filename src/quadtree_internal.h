#pragma once

#include "quadtree.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace quadtree_internal {

inline constexpr uint32_t kSigsCacheVersion = 4;       // v4: u256 counts/starts are 32B
inline constexpr char kSigsCacheMagic[5] = "QTI5";     // 4 bytes + null

inline uint8_t sig_byte(u64 sig, int byte_index) {
    return (uint8_t)((sig >> (8 * byte_index)) & 0xFF);
}

// Subtract d from all 8 bytes of a sig to get a relative sig.
inline u64 sig_to_relative(u64 sig, int depth) {
    uint8_t delta = (uint8_t)depth;
    u64 relative_sig = 0;
    for (int i = 0; i < 8; i++)
        relative_sig |= (u64)(uint8_t)(sig_byte(sig, i) - delta) << (8 * i);
    return relative_sig;
}

// Add d to all 8 bytes of a relative sig to get an absolute sig.
inline u64 relative_to_sig(u64 relative_sig, int depth) {
    uint8_t delta = (uint8_t)depth;
    u64 sig = 0;
    for (int i = 0; i < 8; i++)
        sig |= (u64)(uint8_t)(sig_byte(relative_sig, i) + delta) << (8 * i);
    return sig;
}

// Compute the parent signature from four child signatures.
inline u64 parent_sig(u64 nw, u64 ne, u64 sw, u64 se) {
    uint8_t p0 = std::min(sig_byte(nw,0), sig_byte(ne,0));
    uint8_t p1 = std::max(sig_byte(nw,1), sig_byte(ne,1));
    uint8_t p2 = std::min(sig_byte(sw,2), sig_byte(se,2));
    uint8_t p3 = std::max(sig_byte(sw,3), sig_byte(se,3));
    uint8_t p4 = std::min(sig_byte(nw,4), sig_byte(sw,4));
    uint8_t p5 = std::max(sig_byte(nw,5), sig_byte(sw,5));
    uint8_t p6 = std::min(sig_byte(ne,6), sig_byte(se,6));
    uint8_t p7 = std::max(sig_byte(ne,7), sig_byte(se,7));
    return (u64)p0
        | ((u64)p1 <<  8) | ((u64)p2 << 16) | ((u64)p3 << 24)
        | ((u64)p4 << 32) | ((u64)p5 << 40)
        | ((u64)p6 << 48) | ((u64)p7 << 56);
}

// Pack four 16-bit child signature indices into one u64.
inline u64 pack_key(int nw, int ne, int sw, int se) {
    return (u64)(uint16_t)nw
        | ((u64)(uint16_t)ne << 16)
        | ((u64)(uint16_t)sw << 32)
        | ((u64)(uint16_t)se << 48);
}

template <typename T>
inline void write_pod(std::ostream& os, const T& v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
inline void read_pod(std::istream& is, T& v) {
    is.read(reinterpret_cast<char*>(&v), sizeof(T));
}

inline void write_u256_binary(std::ostream& os, const u256& v) {
    for (int i = 0; i < 4; i++) write_pod(os, v.limbs[i]);
}

inline u256 read_u256_binary(std::istream& is) {
    u256 r;
    for (int i = 0; i < 4; i++) read_pod(is, r.limbs[i]);
    return r;
}

inline void read_exact_file(void* ptr, size_t size, size_t count, FILE* stream) {
    if (std::fread(ptr, size, count, stream) != count)
        throw std::runtime_error("short read from cache file");
}

inline std::filesystem::path cache_directory() {
    return std::filesystem::path("quadtree_cache");
}

struct ChildComboList {
    std::vector<std::array<int,4>> combos;
    std::vector<u256> starts;
};

// Lightweight in-RAM directory for one depth's QTCH children file.
// Keeps the file open; per-parent combo blocks are read on demand via
// fetch_combo_list_from_children().
//
// parents[] is sorted by raw u64 value (re-sorted at load time, independent of
// the on-disk order) so a plain binary search resolves a parent sig to its row.
// ptrs[] holds, per row, the [begin, end) range into the keys/starts sections.
// The leaf sig at this depth has no row in the file; fetch returns an empty
// list for it.
struct ChildrenFileIndex {
    FILE* fh = nullptr;
    std::vector<u64>      parents;    // absolute parent sigs, sorted by raw u64
    std::vector<uint64_t> begins;     // begins[i], ends[i] = key/start range of parents[i]
    std::vector<uint64_t> ends;
    uint64_t keys_off   = 0;          // byte offset of the packed-key section
    uint64_t starts_off = 0;          // byte offset of the u256 starts section
    u64      leaf_sig   = 0;          // leaf sig at this depth (no file row)

    ~ChildrenFileIndex() { if (fh) { std::fclose(fh); fh = nullptr; } }
    ChildrenFileIndex() = default;
    ChildrenFileIndex(const ChildrenFileIndex&) = delete;
    ChildrenFileIndex& operator=(const ChildrenFileIndex&) = delete;
    ChildrenFileIndex(ChildrenFileIndex&& o) noexcept
        : fh(o.fh), parents(std::move(o.parents)), begins(std::move(o.begins)),
          ends(std::move(o.ends)), keys_off(o.keys_off),
          starts_off(o.starts_off), leaf_sig(o.leaf_sig)
    { o.fh = nullptr; }
    ChildrenFileIndex& operator=(ChildrenFileIndex&& o) noexcept {
        if (fh) std::fclose(fh);
        fh = o.fh; o.fh = nullptr;
        parents = std::move(o.parents);
        begins = std::move(o.begins);
        ends = std::move(o.ends);
        keys_off = o.keys_off;
        starts_off = o.starts_off;
        leaf_sig = o.leaf_sig;
        return *this;
    }
};

// Read one parent sig's ChildComboList from an open ChildrenFileIndex.
// Returns false only if the sig is genuinely absent (not a parent and not the
// leaf sig). The leaf sig yields an empty (but valid) ChildComboList.
//
// The QTCH file stores child-index quads as packed u64 keys
// (nw | ne<<16 | sw<<32 | se<<48); they are unpacked into combos[].
inline bool fetch_combo_list_from_children(
    const ChildrenFileIndex& idx, u64 sig, ChildComboList& out)
{
    auto it = std::lower_bound(idx.parents.begin(), idx.parents.end(), sig);
    if (it == idx.parents.end() || *it != sig) {
        if (sig == idx.leaf_sig) {  // leaf: valid, empty combo list
            out.combos.clear();
            out.starts.clear();
            return true;
        }
        return false;
    }
    size_t row = (size_t)(it - idx.parents.begin());

    uint64_t lo = idx.begins[row], hi = idx.ends[row];
    uint32_t n = (uint32_t)(hi - lo);
    out.combos.resize(n);
    out.starts.resize(n);

    if (fseeko(idx.fh, (off_t)(idx.keys_off + lo * 8), SEEK_SET) != 0)
        throw std::runtime_error("fseeko failed reading children keys");
    for (uint32_t j = 0; j < n; j++) {
        u64 packed;
        read_exact_file(&packed, 8, 1, idx.fh);
        out.combos[j][0] = (int)( packed        & 0xFFFF);
        out.combos[j][1] = (int)((packed >> 16) & 0xFFFF);
        out.combos[j][2] = (int)((packed >> 32) & 0xFFFF);
        out.combos[j][3] = (int)((packed >> 48) & 0xFFFF);
    }

    if (fseeko(idx.fh, (off_t)(idx.starts_off + lo * 32), SEEK_SET) != 0)
        throw std::runtime_error("fseeko failed reading children starts");
    for (uint32_t j = 0; j < n; j++) {
        for (int x = 0; x < 4; x++) {
            uint64_t limb;
            read_exact_file(&limb, 8, 1, idx.fh);
            out.starts[j].limbs[x] = limb;
        }
    }
    return true;
}

struct DirectIndexMemo {
    std::vector<std::unordered_map<u64, u256>> counts;
    std::vector<std::vector<u64>> sigs;
    // Per-depth QTCH children-file directory (header in RAM, file kept open).
    // Combo blocks are read on demand via fetch_combo_list_from_children().
    std::vector<ChildrenFileIndex> children_index;
    // Small per-depth cache of recently fetched combo lists (evicted between top-level sigs).
    std::vector<std::unordered_map<u64, ChildComboList>> combo_cache;
    bool built = false;
};

void build_direct_memo(int k, DirectIndexMemo& m);

// Create a per-thread copy of a fully-built DirectIndexMemo.
// The read-only fields (counts, sigs, parents/begins/ends/offsets/leaf_sig) are
// copied by value; each depth gets a freshly-opened FILE* so concurrent seeks
// don't collide; combo_cache starts empty.
DirectIndexMemo clone_direct_memo_for_thread(int k, const DirectIndexMemo& src);

// Path of the per-"levels-below" children cache file (QTCH format). The file
// content depends only on levels_below = k - depth, not on k or depth
// individually, so it is shared across every (k, depth) pair with the same
// levels_below value.
inline std::filesystem::path children_file_path(int k, int depth) {
    return cache_directory()
        / ("lb" + std::to_string(k - depth) + "_children.bin");
}

// Build the lb{k-depth}_children.bin file (QTCH) for one depth level, given the
// child signature table and the per-child-sig subtree counts. Idempotent: if the
// file already exists it is left untouched. Returns the sorted parent-sig vector
// (relative-sig sort order) so callers can reuse it for their own bookkeeping.
std::vector<u64> build_children_file_for_depth(
    int k, int depth,
    const std::vector<u64>& child_sigs,
    const std::unordered_map<u64, u256>& child_counts);

}  // namespace quadtree_internal
