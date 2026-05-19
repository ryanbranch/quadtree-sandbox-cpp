# Architecture

## Overview

The project implements rank/unrank operations entirely in memory, with no disk I/O. All data structures are computed fresh on each `QuadtreeIndex` construction, which is fast for all supported k values (0–3).

---

## The counting algorithm

Working **bottom-up** from depth `k` to depth `0`:

1. At depth `k`, there is exactly one possible node: the leaf. Its signature is unique.
2. At each depth `d < k`, enumerate all valid combinations of four child signatures (NW, NE, SW, SE) that are mutually compatible. For each valid combination, compute the parent signature using `parentSig()`. Accumulate: the number of trees with a given parent signature equals the sum over all valid child combinations that produce that parent, of the product of the per-child subtree counts.
3. The total count at depth `0` is the sum of counts across all root signatures.

The key insight is that only **signatures** need to be tracked — not individual trees. Trees with the same signature are structurally interchangeable for counting purposes. The number of distinct signatures grows much more slowly than the number of trees, making the algorithm feasible.

---

## The index implementation

### When to use it

`QuadtreeIndex` is always-on — there is no separate "direct" path. The index is built in memory when the `QuadtreeIndex` constructor runs, then reused for every `unrank()` / `rank()` call on that instance.

### Build phase

When a `QuadtreeIndex` is constructed for a given `k`, it runs the bottom-up DP:

1. Computes signature tables and counts from depth `k` down to depth `0`.
2. For each depth, stores the sorted signature list and their subtree counts in memory.
3. For each depth `d < k`, builds a child-combination lookup table (child-sig quadruples grouped by parent signature, with prefix-sum start values).

All data lives in memory for the lifetime of the index object.

### Ordering guarantee

Signatures are sorted by their **relative** value (depth subtracted from each field) so that the ordering of signatures is consistent regardless of `k`. This ensures that `rank()` and `unrank()` produce identical results for the same tree across different index instances.

---

## The direct (precomputation-free) implementation

The JavaScript implementation builds a `DirectIndexMemo` struct on construction — a complete in-memory copy of the signature tables and counts — by running the full bottom-up computation from depth `k` to `0`. This structure is then reused for all `rank()`, `unrank()`, `countMatching()`, and `enumerateMatching()` calls.

For k ≤ 3, this computation completes in milliseconds.

---

## Constrained enumeration architecture

The constrained enumeration (`enumerateMatching`) reuses the in-memory index and adds a `matchSig()` recursive function that walks the tree enumeration in the same order as `rank`/`unrank`, pruning branches that conflict with the mask.

`matchSig()` returns a list of `RankInterval` values — contiguous blocks of matching trees within that node's local rank space. These are composed using `crossIntervals()`, which takes the matched intervals from each of the four child quadrants and computes their Cartesian product as intervals in the parent's rank space.

Per-signature combo lists are cached in the memo so they aren't recomputed across many recursive calls within a single `enumerateMatching` invocation.

---

## Multi-quadtree tilings

A **multi-quadtree tiling** covers an arbitrary `width × height` rectangle by packing several power-of-two-square quadtree roots side by side. Each root is a `RootSpec {x, y, k}` specifying its top-left corner and depth. The roots form an **exact cover** — every cell belongs to exactly one root, with no overlaps and no gaps.

### Why multi-quadtree?

A single quadtree root must be a 2ᵏ × 2ᵏ square. To tile non-square or non-power-of-two canvases (e.g., a 12 × 8 panel), you compose several roots of different sizes. The inter-root **balance constraint** still applies: two tiles from adjacent roots must satisfy the same 2:1 size rule as tiles within a single root.

### Cover generation

Two strategies are provided for choosing which roots cover the rectangle:

- **Random cover** (`randomRootCover`): at each uncovered cell (scanned in row-major order), picks a random valid power-of-two square size. Produces varied, irregular mixes of root sizes.
- **Greedy cover** (`greedyRootCover`, `--greedy-cover` flag): at each uncovered cell, places the *largest* power-of-two square that fits without overlap. Always deterministic; produces the minimum number of roots for the given grid dimensions.

You can also supply an explicit **layout file** (`--layout FILE`) to fix the exact cover rather than letting the program choose.

### Tree assignment

Once a cover is chosen, each root gets a randomly sampled balanced quadtree. Roots are processed largest-first (highest k first), then in row-major order within the same k. This ordering ensures that, when sampling a small root, its larger neighbors are already placed and their border tiles are known.

The sampling respects inter-root compatibility: the border tiles of already-placed neighbors impose cell-size constraints on the new root, which are evaluated as a mask and fed into `enumerateMatching` to draw only from valid trees.

---

## Source file map

| File | Contents |
|------|----------|
| `js/quadtree.js` | Public API: `QNode`, `Mask`, `RankInterval`, `QuadtreeIndex`, multi-quadtree types |
| `js/quadtree_node.js` | Node construction, rendering helpers, simple counting/enumeration, interval merging |
| `js/quadtree_rank.js` | In-memory rank/unrank; direct rank/unrank; combo list construction |
| `js/quadtree_constrained.js` | Mask-constrained counting and interval enumeration |
| `js/quadtree_multi.js` | Multi-quadtree cover generation, validation, and tree assignment |
| `js/quadtree_main.js` | CLI driver, mask file loading, verification logic |
