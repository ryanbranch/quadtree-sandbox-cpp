# Concepts: Balanced Quadtree Tilings

## What is a quadtree tiling?

A **quadtree tiling** of a 2ᵏ × 2ᵏ square is a recursive subdivision where each tile is either:

- A **leaf**: the tile is not subdivided further. It covers some 2ⁿ × 2ⁿ region as a single square tile.
- An **internal node**: the tile is split into four equal quadrants (NW, NE, SW, SE), each of which is itself a quadtree tiling.

The number `k` is the **depth parameter**: the root of the tree covers a 2ᵏ × 2ᵏ grid, and the maximum possible subdivision produces 1×1 tiles at depth k.

Visually, a quadtree tiling is a way of partitioning a square into non-overlapping square tiles of varying sizes, all powers of two, where each split divides a region into four equal quadrants.

```
Example: k=2 (4×4 grid), 3 different tilings

All leaves (one 4×4 tile):   Fully split (sixteen 1×1 tiles):   Mixed:
┌────────────┐                ┌──┬──┬──┬──┐                     ┌──┬──┬──┬──┐
│            │                │  │  │  │  │                     │  │  │     │
│            │                ├──┼──┼──┼──┤                     ├──┼──┤     │
│            │                │  │  │  │  │                     │  │  │     │
│            │                ├──┼──┼──┼──┤                     ├──┴──┼──┬──┤
│            │                │  │  │  │  │                     │     │  │  │
│            │                ├──┼──┼──┼──┤                     │     ├──┼──┤
│            │                │  │  │  │  │                     │     │  │  │
└────────────┘                └──┴──┴──┴──┘                     └─────┴──┴──┘
```

## The balance constraint

An **unbalanced** quadtree tiling can produce adjacent tiles of wildly different sizes — for example, a 4×4 tile sharing an edge with a 1×1 tile. For many art and layout applications, this is aesthetically or structurally undesirable.

A **balanced** quadtree tiling enforces that any two adjacent tiles differ in size by at most a factor of 2. Equivalently: for any edge shared between two tiles, the larger tile's size is at most twice the smaller tile's size.

The project implements this constraint using an **edge-depth compatibility rule**. Each node tracks, for each of its four edges, the range of depths at which leaves appear along that edge. Two adjacent nodes are **compatible** if their shared edge satisfies:

```
max_depth_left - min_depth_right ≤ 1
max_depth_right - min_depth_left ≤ 1
```

This ensures neither side has a leaf that is more than one depth level (factor of 2 in size) smaller than any leaf on the other side.

## Why enumerate and rank them?

The project is a foundation for **generative art**: to create random, aesthetically pleasing tiling patterns over a 2ᵏ × 2ᵏ canvas. To do this in a principled way — for example, to uniformly sample a random balanced tiling, or to reproduce a specific tiling by its index — you need:

1. **Counting**: how many balanced quadtree tilings exist for a given k?
2. **Ranking**: given a specific tiling, what is its index in the sorted enumeration?
3. **Unranking**: given an index, reconstruct the corresponding tiling.

This rank/unrank pair is a **combinatorial bijection** between the set of all balanced tilings and the integers `[0, total)`. It lets you:

- Pick a random tiling by generating a random number in `[0, total)` and unranking it.
- Save and reproduce a specific tiling by storing its rank.
- Enumerate all tilings in a canonical order.
- Enumerate only the tilings that satisfy additional structural constraints (the **constrained enumeration** feature).

## Scale

The number of balanced tilings grows very rapidly with k:

| k | Grid size | Exact count |
|---|-----------|-------------|
| 0 | 1×1       | 1 |
| 1 | 2×2       | 2 |
| 2 | 4×4       | 17 |
| 3 | 8×8       | 66,642 |

The JavaScript implementation supports k values from 0 through 3. All counts and rank values fit comfortably within JavaScript's `Number` type (IEEE 754 double-precision, safe up to 2⁵³ − 1 ≈ 9 × 10¹⁵), so no special big-integer handling is required.

## The constrained enumeration extension

Beyond raw rank/unrank, the project also supports **constrained enumeration**: given a **mask** specifying which cells of the grid must belong to tiles of a particular size, enumerate (or count) all balanced tilings that satisfy those constraints.

This is useful for generative art where certain regions of the canvas have fixed tile sizes — for example, a focal point requiring fine detail — while the rest of the tiling varies freely. See [data-structures.md](data-structures.md) for the `Mask` structure, and [api.md](api.md) for the `enumerate_matching` / `count_matching` API.
