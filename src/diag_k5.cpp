// Diagnostic: at k=5, sample several random indices, and print:
// - the actual u256 index n
// - the depth at which the SE chain first becomes a leaf
// - the depth at which the SE->SE chain first becomes a leaf
// - the root signature byte pattern picked
//
// Compile from project root:
//   make diag_k5
// Run from project root so cache files at quadtree_cache/ are found.

#include "quadtree.h"
#include <cstdio>
#include <random>

int main(int argc, char** argv) {
    int k = 5;
    int n_samples = 20;
    if (argc > 1) n_samples = std::atoi(argv[1]);

    QuadtreeIndex idx(k);
    u256 total = idx.total();
    printf("k=%d total = %s\n", k, u256_to_string(total).c_str());

    std::mt19937_64 rng(std::random_device{}());

    auto randIndex = [&]() {
        u256 r;
        r.limbs[0] = rng();
        r.limbs[1] = rng();
        r.limbs[2] = rng();
        r.limbs[3] = rng();
        return r % total;
    };

    int n_se8x8_leaf = 0;

    for (int s = 0; s < n_samples; s++) {
        u256 n = randIndex();
        auto tree = idx.unrank(n);

        printf("\nsample %d  n=%s\n", s, u256_to_string(n).c_str());

        const QNode* p = tree.get();
        printf("  root: %s", p->is_leaf() ? "LEAF" : "internal");
        if (!p->is_leaf()) {
            u64 sig = QuadtreeIndex::node_sig_int(*p);
            printf("  sig=0x%016lx", sig);
        }
        printf("\n");

        for (int d = 0; d < k && p && !p->is_leaf(); d++) {
            const QNode* se = p->children[3].get();
            printf("  d=%d SE = %s", d+1, se->is_leaf() ? "LEAF" : "internal");
            if (!se->is_leaf()) {
                u64 sig = QuadtreeIndex::node_sig_int(*se);
                printf("  sig=0x%016lx", sig);
            }
            printf("\n");
            p = se;
        }

        const QNode* q = tree.get();
        bool se_se_uniform = false;
        if (!q->is_leaf()) {
            const QNode* se1 = q->children[3].get();
            if (se1->is_leaf()) {
                se_se_uniform = true;
                printf("  -> SE depth-1 is a LEAF (covers entire SE 16x16)\n");
            } else {
                const QNode* se2 = se1->children[3].get();
                if (se2->is_leaf()) {
                    se_se_uniform = true;
                    printf("  -> SE->SE depth-2 is a LEAF (covers SE-SE 8x8)\n");
                } else {
                    printf("  -> SE->SE depth-2 is internal (not uniform 8x8)\n");
                }
            }
        }
        if (se_se_uniform) n_se8x8_leaf++;

        u256 r2 = idx.rank(*tree);
        if (r2 != n) {
            printf("  *** RANK MISMATCH: rank(unrank(n)) = %s != n = %s\n",
                u256_to_string(r2).c_str(), u256_to_string(n).c_str());
        }
    }

    printf("\n%d/%d samples had uniform SE->SE 8x8 (or larger) leaf block\n",
        n_se8x8_leaf, n_samples);
    return 0;
}
