# Data Structures

## QNode

`QNode` is the in-memory representation of a balanced quadtree tiling (or subtree thereof).

```js
{
  depth,                        // 0 = root, k = maximum leaf depth
  minTop, maxTop,               // range of leaf depths along the top edge
  minBottom, maxBottom,         // range of leaf depths along the bottom edge
  minLeft, maxLeft,             // range of leaf depths along the left edge
  minRight, maxRight,           // range of leaf depths along the right edge
  children,                     // null = leaf; array of [NW, NE, SW, SE] = internal node
}
```

### Edge depth ranges

Each of the four edge fields (`minTop`/`maxTop`, etc.) records the minimum and maximum **depth** of leaf nodes whose tiles touch that edge. Depth here means the level in the tree where a node is a leaf — a leaf at depth `d` covers a tile of size 2^(k−d) × 2^(k−d) pixels.

For a leaf node, all four min/max values equal its own depth, since the leaf itself is the only tile touching all of its edges.

For an internal node, these propagate from children — each edge's min/max comes from the two children that share that edge:

```
minTop    = min(NW.minTop,    NE.minTop)     ← NW and NE share the top edge
maxTop    = max(NW.maxTop,    NE.maxTop)
minBottom = min(SW.minBottom, SE.minBottom)   ← SW and SE share the bottom edge
maxBottom = max(SW.maxBottom, SE.maxBottom)
minLeft   = min(NW.minLeft,   SW.minLeft)     ← NW and SW share the left edge
maxLeft   = max(NW.maxLeft,   SW.maxLeft)
minRight  = min(NE.minRight,  SE.minRight)    ← NE and SE share the right edge
maxRight  = max(NE.maxRight,  SE.maxRight)
```

These ranges serve a dual purpose: they enforce the balance constraint at construction time (via the `compatible()` check), and they compress a node's structural information into a compact "signature" used for counting and indexing. The same propagation rules are used when computing a parent's signature from four child signatures (see the Signature section below).

### Construction

Use `makeLeaf(depth)` and `makeInternal(depth, nw, ne, sw, se)` rather than constructing a node object directly. These functions set all fields correctly.

---

## Signature

A **signature** is a compact object that summarizes a node's eight edge depth values. It is the key abstraction that makes efficient counting and indexing possible.

```js
{
  minTop, maxTop,
  minBottom, maxBottom,
  minLeft, maxLeft,
  minRight, maxRight,
}
```

Two nodes with the **same signature** are structurally interchangeable for the purposes of compatibility checking and counting: whether a given parent signature can be formed by assembling four children depends only on their signatures, not on the internal structure of their subtrees.

This is the insight that makes the counting algorithm tractable: instead of tracking individual trees, the code tracks **equivalence classes of subtrees** by signature, and accumulates counts per class.

### Signature computation

```js
nodeSig(node)   // from a QNode
leafSig(depth)  // for a pure leaf at given depth
```

For a leaf at depth `d`, all eight values equal `d` — the leaf has a constant depth on all edges.

### Parent signature derivation

Given four child signatures, the parent signature is computed by `parentSig()` using the same propagation rules described in the [QNode edge depth ranges](#edge-depth-ranges) section above — min/max per edge, taken from the two children that share that edge.

### Compatibility check

Two adjacent nodes (sharing an edge) are compatible if `compatible()` returns true:

```js
compatible(aMax, aMin, bMax, bMin)
// Returns true iff (aMax - bMin <= 1) && (bMax - aMin <= 1)
```

Call it for each shared edge using the relevant edge's min/max values:

- NW–NE horizontal edge: `compatible(NW.maxRight, NW.minRight, NE.maxLeft, NE.minLeft)`
- NW–SW vertical edge: `compatible(NW.maxBottom, NW.minBottom, SW.maxTop, SW.minTop)`
- NE–SE vertical edge: `compatible(NE.maxBottom, NE.minBottom, SE.maxTop, SE.minTop)`
- SW–SE horizontal edge: `compatible(SW.maxRight, SW.minRight, SE.maxLeft, SE.minLeft)`

---

## Mask

A `Mask` constrains which balanced tilings are considered in constrained enumeration.

```js
{
  grid,   // 2D array [row][col]; 0 = unconstrained, s > 0 = cell must be in an s×s tile
  exact,  // array of 4 QNode subtrees (or null) for [NW, NE, SW, SE] root quadrants
}
```

### Cell-size grid

`grid[row][col]` specifies a constraint on the tile that covers cell `(row, col)`:

- `0`: no constraint — any tile size is allowed.
- `s > 0`: the cell must belong to a tile of size `s × s`.

The grid must be 2ᵏ × 2ᵏ to match the index's `k`. Tile sizes in the grid must be powers of two in the range `[1, 2ᵏ]`.

**Example**: for k=2 (a 4×4 grid), setting `grid[0][0] = 2` requires that the top-left cell belong to a 2×2 tile, meaning the NW quadrant of the root must be a leaf (not further subdivided).

A region in the mask is considered **invalid** (no trees match) if it contains a mix of tile size constraints that can't all be satisfied simultaneously — for example, if a 2×2 region has one cell requiring size 4 and another requiring size 2.

### Exact subtree constraints

`exact[q]` for `q` in `{NW=0, NE=1, SW=2, SE=3}` pins the entire root quadrant to a specific `QNode` subtree. These take priority over the grid and are checked first during enumeration. If both an exact constraint and a grid constraint apply to the same region, both must be satisfied (the tree must equal the exact subtree AND the grid must be consistent with its rendered layout).

### Construction

```js
freeMask(k)  // all cells unconstrained, all exact = null
```

Modify individual grid cells after constructing a free mask:

```js
const mask = freeMask(k);
mask.grid[0][0] = 2;   // cell (0,0) must be in a 2×2 tile
```

---

## RankInterval

A `RankInterval` is a half-open interval `[lo, hi)` of global tree indices (ranks):

```js
{ lo, hi }  // lo inclusive, hi exclusive; both are plain Numbers
```

`enumerateMatching()` produces a list of these intervals. Each interval represents a contiguous block of trees — by their rank — that all satisfy the given mask. The intervals are non-overlapping and sorted in ascending order.

Using intervals rather than listing individual ranks is essential for performance: a mask that matches millions of trees may produce only a handful of intervals.

The helper `mergeIntervals(intervals)` sorts and merges an arbitrary list of intervals into the canonical non-overlapping sorted form.

All rank values for k ≤ 3 (maximum count: 66,642) fit well within JavaScript's safe integer range.
