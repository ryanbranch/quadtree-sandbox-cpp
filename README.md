# quadtree-sandbox

A JavaScript library and CLI for counting, ranking, and unranking **balanced quadtree tilings** — a foundation for generative art and combinatorial exploration.

## What it does

A balanced quadtree tiling is a recursive subdivision of a 2ᵏ × 2ᵏ square into tiles, where no two adjacent tiles differ in size by more than a factor of 2. The project can:

- **Count** the total number of distinct balanced tilings for any given k
- **Rank** a specific tiling to a unique integer index
- **Unrank** an integer back to the corresponding tiling (enabling random sampling)
- **Enumerate** only the tilings that match a structural constraint (a *mask*)

Supported k values are 0 through 3. The counts are 1 at k=0, 2 at k=1, 17 at k=2, and 66,642 at k=3 — all well within JavaScript's safe integer range. All computation runs in memory with no disk I/O.

## Quick start

```bash
# Count all balanced tilings for an 8x8 grid
node quadtree.js 3
# Found 66642 balanced tilings for a 8x8 grid (k=3)

# Retrieve tiling #12 for a 4x4 grid and print it
node quadtree.js 2 --unrank 12
# Tiling #12 of 17 (k=2):
# 1 1 2 2
# 1 1 2 2
# 1 1 1 1
# 1 1 1 1
```

See [docs/api.md](docs/api.md) for all CLI options and the full JavaScript API.

## Documentation

| Document | Contents |
|----------|----------|
| [docs/concepts.md](docs/concepts.md) | What balanced quadtree tilings are, the balance constraint, why rank/unrank matters, and a table of exact counts by k |
| [docs/data-structures.md](docs/data-structures.md) | `QNode`, the signature, `Mask`, and `RankInterval` |
| [docs/architecture.md](docs/architecture.md) | The counting algorithm, in-memory index, and multi-quadtree tilings |
| [docs/api.md](docs/api.md) | Full JavaScript API reference and CLI usage with verified example outputs |
| [docs/gui.md](docs/gui.md) | GUI viewer (`quadtree_gui`): build instructions, CLI flags, keyboard controls, tile/path color modes, chirality + flip routing, and the noise field |
| [docs/cpp-implementation.md](docs/cpp-implementation.md) | C++-specific details: 256-bit integers, disk-cached index, k > 3 support, binary file formats |

Start with **[docs/concepts.md](docs/concepts.md)** if you're new to the project.

## License

See [LICENSE.md](LICENSE.md) for terms of use.
