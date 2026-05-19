# C++ Implementation Notes

This document covers features and design decisions specific to the C++ implementation. The main documentation ([concepts.md](concepts.md), [data-structures.md](data-structures.md), [architecture.md](architecture.md), [api.md](api.md)) is written for the JavaScript implementation.

---

## Support for k > 3

The C++ implementation supports k values up to 5. At k=4 the total count is ~1.88×10¹⁹ and at k=5 it reaches ~2.35×10⁷⁵ — both exceeding JavaScript's safe integer range and the capacity of 64-bit integers. The C++ codebase uses `u256` (see below) throughout to accommodate these counts.

| k | Grid size | Exact count |
|---|-----------|-------------|
| 4 | 16×16     | 18,777,159,138,053,248,015 |
| 5 | 32×32     | 329,705,861,805,574,718,007,790,980,969,155,498,447 |

---

## 256-bit integers (`u256`)

`u256` is a custom 256-bit unsigned integer type defined in `src/u256.cpp`. It is used for all counts, ranks, and interval bounds in the C++ codebase.

### Why 256 bits?

At k=5, the total count is approximately 2.35×10⁷⁵ — far beyond the ~3.4×10³⁸ capacity of a 128-bit integer. Attempting to use 128 bits caused silent overflow that corrupted the prefix-sum table in the children cache, making random sampling collapse to a degenerate subset of trees. `u256` (max ~1.16×10⁷⁷) comfortably fits all counts through at least k=5.

### Representation

```cpp
struct u256 {
    uint64_t limbs[4];  // little-endian: limbs[0] is least significant
};
```

Four 64-bit limbs stored in little-endian order. The type is POD and stack-allocated — no heap allocation.

---

## Disk-cached index

For k=4 and k=5, building the index from scratch on every run would take minutes to hours. The C++ implementation caches the computed tables to `quadtree_cache/` and loads them on subsequent runs.

### Cache file types

| Pattern | Built by | Contents |
|---------|----------|----------|
| `k{k}_d{d}_sigs.bin` | `QuadtreeIndex(k)` | Sorted signature list and per-signature subtree counts for depth d |
| `lb{n}_children.bin` | `QuadtreeIndex(k)` | Child-combination lookup table for n levels below the current depth (k-independent; shared across k values) |
| `k{k}_d{d}_combos.bin` | First use of direct rank/unrank | Per-parent-sig combo lists for the direct rank/unrank and constrained enumeration paths |
| `edge1x1_k{k}_m{mask}.bin` | `--precompute-edge-1x1 K` | Precomputed rank intervals for all trees of depth k whose outer edges (bitmask) are all 1×1 tiles |

The cache directory is safe to delete; it will be rebuilt automatically.

### Signature packing for disk storage

Signatures in the C++ implementation are packed into a single `u64`: each of the eight edge min/max values occupies one byte (cast to `uint8_t`).

```
Byte layout (little-endian within the u64):
  byte 0: min_top
  byte 1: max_top
  byte 2: min_bottom
  byte 3: max_bottom
  byte 4: min_left
  byte 5: max_left
  byte 6: min_right
  byte 7: max_right
```

When stored to disk, signatures are written as **relative** values: each byte has the node's depth subtracted from it. This makes the signature values independent of `k`, so the `lbN_children.bin` files can be shared across different values of `k`.

The conversion functions are `sig_to_relative(sig, depth)` and `relative_to_sig(rel_sig, depth)`.

### Children file format (QTCH)

```
Header:
  [4 bytes]  magic "QTCH"
  [4 bytes]  version (uint32)
  [4 bytes]  levels_below (uint32)  = k - depth
  [4 bytes]  n_child_sigs (uint32)
  [8 * n_child_sigs bytes]  relative child signatures, sorted
  [4 bytes]  n_parents (uint32)
  [8 * n_parents bytes]  relative parent signatures, sorted
  [8 * (n_parents+1) bytes]  ptrs[]: cumulative key counts (CSR-style offsets)
  [8 bytes]  total_keys (uint64)

Keys section (at offset keys_off):
  [8 * total_keys bytes]  packed child-index quadruples (u64 each)
                          4 x uint16 packed as: nw | (ne<<16) | (sw<<32) | (se<<48)

Starts section (at offset starts_off = keys_off + total_keys*8):
  [32 * total_keys bytes]  prefix-sum start values (u256 each, stored as 4 x u64 limbs)
```

For a given parent signature at index `i`, its keys span `ptrs[i]..ptrs[i+1]` in the keys section, and the matching starts span the same range in the starts section. Keys are sorted, enabling binary search during unranking.

The children files are never fully loaded into memory — only the header (parent directory + ptrs) is loaded. The actual key and start data is seeked and read on demand via an open `FILE*` handle.

---

## Build phase speedups for k=5

Building the k=5 combo lists naively would require holding ~60 GB of data in RAM simultaneously. Three speedups make k=5 feasible on ordinary hardware:

**1. Factored pair enumeration (largest impact)**

Instead of iterating all (nw, ne, sw, se) in O(nc⁴), the algorithm first enumerates valid top-pairs (nw, ne) satisfying the NW–NE right-edge compatibility check, then for each top-pair iterates only the sw values that are bottom-compatible with nw, and only the se values that are bottom-compatible with ne *and* right-compatible with sw. For nc=131 child sigs at k=4 depth 1, the compatibility matrices are sparse enough that this reduces the search space by orders of magnitude.

**2. Bitmask pruning**

Compatibility sets are stored as packed `uint64_t` bitmask arrays. The inner `se` loop uses a bitwise AND of two bitmask words to find valid se entries in bulk, then uses `__builtin_ctzll` to iterate only the set bits.

**3. Multi-pass partitioning for RAM reduction**

Rather than accumulating all parent entries in RAM before sorting and writing, the build phase runs 33 passes over the full combo enumeration:
- A fast **pre-pass** collects the set of all unique parent signatures.
- **32 main passes** each process 1/32 of the parent signatures, accumulate only that slice's entries, sort them, and write immediately — then discard before the next pass.

Peak RAM drops from ~60 GB (original) to ~2–3 GB.

---

## 1×1 outer-edge precompute cache

For multi-quadtree tilings with `--outer-edge-1x1`, the C++ implementation precomputes and caches rank-interval lists for each `(k, edge_mask)` combination. For k=4 or k=5, a single `enumerate_matching` call can take minutes to hours, so caching is essential.

### Precomputing

```bash
./quadtree --precompute-edge-1x1 K
```

For a grid that uses roots of k=1 through k=3, run:

```bash
./quadtree --precompute-edge-1x1 1
./quadtree --precompute-edge-1x1 2
./quadtree --precompute-edge-1x1 3
```

Only the 4 cardinal-edge masks (N, E, S, W) are computed by full `enumerate_matching`; the remaining 11 composite masks are derived by intersecting the cardinal interval lists.

### RAM cost for k=5

By default all 4 cardinal masks run **in parallel**, each holding its own clone of the index memo. For **k=5** this drives a peak of **~37 GB of RSS** lasting ~2.5 minutes. Use `--precompute-jobs N` or `--no-parallel` to reduce concurrency if RAM is limited.

---

## C++ API

### Construction

```cpp
QuadtreeIndex idx(k);
```

On first construction for a given `k`, this builds and saves the index to `quadtree_cache/`. On subsequent constructions it loads from cache.

### Counting

```cpp
u256 idx.total() const;
u256 idx.total_direct() const;
u256 count_balanced_quadtrees(int k);
```

`total()` uses the cached index. `total_direct()` recomputes from scratch.

### Unranking / ranking

```cpp
std::shared_ptr<QNode> idx.unrank(u256 n) const;
std::shared_ptr<QNode> idx.unrank_direct(u256 n) const;
u256 idx.rank(const QNode& tree) const;
u256 idx.rank_direct(const QNode& tree) const;
```

The `_direct` variants recompute everything in memory on each call (no disk I/O).

### Constrained enumeration

```cpp
u256 idx.count_matching(const Mask& mask) const;
u256 idx.enumerate_matching(const Mask& mask, std::function<void(RankInterval)> cb) const;
```

### Static helpers

```cpp
static u64  QuadtreeIndex::leaf_sig_int(int depth);
static u64  QuadtreeIndex::node_sig_int(const QNode& node);
static bool QuadtreeIndex::compatible(int a_max, int a_min, int b_max, int b_min);
```

---

## C++ source file map

| File | Contents |
|------|----------|
| `src/quadtree.h` | Public API: `QNode`, `Mask`, `RankInterval`, `QuadtreeIndex`, multi-quadtree types |
| `src/quadtree_internal.h` | Shared implementation helpers: signatures, cache serialization, direct/constrained memo types, `ChildrenFileIndex` |
| `src/quadtree_multi_internal.h` | Shared helpers for multi-quadtree TUs: `EDGE_*` constants, `TileMaps`, edge validation |
| `src/quadtree_node.cpp` | Node construction, rendering helpers, simple counting/enumeration, interval merging |
| `src/quadtree_io.cpp` | Disk-cached index construction, cache file I/O, lazy depth loading |
| `src/quadtree_rank.cpp` | Indexed rank/unrank; direct rank/unrank; combo list build/save/load |
| `src/quadtree_constrained.cpp` | Mask-constrained counting and interval enumeration |
| `src/quadtree_multi_render.cpp` | `render_tiles`, `render_grid` for multi-quadtree tilings |
| `src/quadtree_multi_cover.cpp` | `random_root_cover`, `greedy_root_cover` |
| `src/quadtree_multi_validate.cpp` | Multi-quadtree validation and outer-edge geometry |
| `src/quadtree_multi_edge1x1.cpp` | Outer-edge-1x1 cache I/O and precompute driver |
| `src/quadtree_multi_generate.cpp` | `IndexCache`, sampling helpers, `generate_multi_quadtree` |
| `src/quadtree_main.cpp` | CLI driver, mask file loading, verification logic |
| `src/u256.cpp` | Custom 256-bit unsigned integer: arithmetic, parsing, string conversion |
