 - Benchmark the execution speed and RAM usage of both the cache-based and non-cache-based ranking and unranking operations for each K value 0 through 4?

 - Test the ability to define the rank index of ONE quadtree in multi-quadtree tilings, when defining explicit cover files.

 - "Rank-walk animation": animate the GUI by stepping through rank indices sequentially (or by large strides) and recording to MP4 via the ffmpeg pipe. A 60 FPS video walking through all 66,642 k=3 tilings would run ~18.5 minutes. Key ideas: comma/period step by 1, </>/bracket keys step by large stride, speed multiplier controls ranks-per-second, Space toggles recording. This directly visualizes what "rank" means and is unique to this project.

 - "Multi-quadtree universe" with seamless chunks: build a 2D infinite-canvas where adjacent chunks are independently-generated multi-quadtree tilings that share boundary structure (cells on chunk edges agree on tile size with their neighbor). Use the constrained-rank / Mask API to generate each chunk under boundary constraints, then adopt the chunk-streaming architecture from art-project-src (per-chunk VBOs, viewport-driven load/evict). Prerequisite: finish the multi-quadtree tiling features first.





