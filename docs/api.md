# API Reference

## JavaScript API

### Construction

```js
const idx = new QuadtreeIndex(k);
```

Constructs an index for all balanced quadtree tilings of a 2ᵏ × 2ᵏ grid. Supports k values 0 through 3. Construction runs fully in memory — no disk I/O.

For all supported k values (≤ 3), construction is essentially instant.

---

### Counting

```js
idx.total()
```

Returns the total number of balanced quadtree tilings as a `Number`.

Also available as a standalone function (does not construct a full index):

```js
countBalancedQuadtrees(k)
```

---

### Unranking: index → tree

```js
idx.unrank(n)
```

Given a rank `n` in `[0, total())`, returns the corresponding balanced quadtree as a `QNode` tree. Throws if `n >= total()`.

The enumeration order is **deterministic and stable**: for a given `k`, rank 0 is always the same tree, rank 1 is always the same tree, and so on.

---

### Ranking: tree → index

```js
idx.rank(tree)
```

Given a `QNode` tree, returns its rank — the unique integer in `[0, total())` such that `unrank(rank(tree))` reconstructs the same tree.

The tree must be a valid balanced quadtree of depth consistent with `k`.

---

### Rendering

```js
renderGrid(tree, k)
printGrid(grid)
```

`renderGrid()` produces a 2ᵏ × 2ᵏ grid of numbers where each cell's value is the tile size (in grid cells) of the tile that covers it. For example, a 2×2 tile writes `2` into all four of its cells.

`printGrid()` prints the grid to the console, formatted for readability.

---

### Constrained enumeration

```js
idx.countMatching(mask)
idx.enumerateMatching(mask, callback)
```

`countMatching()` returns the number of balanced tilings that satisfy the mask.

`enumerateMatching()` calls `callback` once per contiguous interval of matching ranks, in ascending order. Returns the total count. Intervals are non-overlapping and in sorted order.

Example: print all matching trees for a mask:

```js
const mask = freeMask(k);
mask.grid[0][0] = 2;  // top-left cell must be in a 2×2 tile

idx.enumerateMatching(mask, ({ lo, hi }) => {
  for (let n = lo; n < hi; n++) {
    const tree = idx.unrank(n);
    printGrid(renderGrid(tree, k));
  }
});
```

See [data-structures.md](data-structures.md) for `Mask` and `RankInterval` details.

---

### Standalone enumeration (small k only)

```js
allBalancedQuadtrees(k)
```

Enumerates and returns all balanced tilings as an array of `QNode` trees. This is a brute-force approach that holds all trees in memory simultaneously. Feasible only for small k (k ≤ 3). Used primarily for testing and verification.

---

### Helpers

```js
leafSig(depth)
nodeSig(node)
compatible(aMax, aMin, bMax, bMin)
```

Low-level utilities. See [data-structures.md](data-structures.md) for details on signatures and the compatibility rule.

---

## CLI

### Basic usage

```
node quadtree.js <k> [options]
```

`k` is the grid depth parameter (0–3). The grid will be 2ᵏ × 2ᵏ.

**No options** — enumerate all balanced tilings and compare against the fast count:

```bash
node quadtree.js 3
# Found 66642 balanced tilings for a 8x8 grid (k=3)
# Fast count: 66642 [OK]
```

---

### Options

**`--count-only`** — print the count without enumerating all trees:

```bash
node quadtree.js 2 --count-only
# Counted 17 balanced tilings for a 4x4 grid (k=2)
```

**`--print-all`** — after enumeration, print every tiling's grid. Combine with small k only:

```bash
node quadtree.js 1 --print-all
```

**`--unrank N`** — retrieve and print tiling number N:

```bash
node quadtree.js 2 --unrank 12
# Tiling #12 of 17 (k=2):
# 1 1 2 2
# 1 1 2 2
# 1 1 1 1
# 1 1 1 1
```

**`--verify-index`** — brute-force verification: enumerate all trees, rank each one, unrank it back, check roundtrip correctness:

```bash
node quadtree.js 2 --verify-index
# [OK] 17 trees: 0 roundtrip errors, ranks cover [0, 17)
```

---

### Mask options

A **mask file** is a plain-text file with 2ᵏ rows and 2ᵏ columns of space-separated integers. `0` means unconstrained; a positive integer `s` means that cell must belong to a tile of size `s`.

Example mask file for k=2 (4×4 grid) requiring a 2×2 tile in the top-left quadrant:

```
2 2 0 0
2 2 0 0
0 0 0 0
0 0 0 0
```

**`--mask FILE`** — count matching tilings and print their rank intervals:

```bash
node quadtree.js 2 --mask my_mask.txt
# Matches: 8
# Intervals (7):
#   [1, 2)
#   [3, 4)
#   [5, 7)
#   [8, 9)
#   [10, 11)
#   [13, 14)
#   [15, 16)
```

**`--mask-print FILE`** — count matching tilings and print every one. Use only for small counts:

```bash
node quadtree.js 2 --mask-print my_mask.txt
```

**`--mask-verify FILE`** — verify the constrained enumeration against brute force:

```bash
node quadtree.js 2 --mask-verify my_mask.txt
# [OK] mask match: constrained=8, brute=8, intervals=7
```
