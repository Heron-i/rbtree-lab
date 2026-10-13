## Reflection on the Mutations
One lesson I learned throughout coding the Mutations was on the particular design of the slab for Mutation 2. When planning with Claude, I would ensure complete mutual understanding by establishing that for Mutation 2; the slab should be treated as storage, and the free-list as the policy. The purpose of the slab allocation was to change how red-black tree nodes could be obtained rather than individually allocating every node. From here, I built my new tests to withstand pooled allocation, failed allocations, and any white-box test cases I could think of (mirror cases, large allocations, fault-injection scenarios, etc). 

Another design lesson I learned was maintaining metadata within the object it-self's available space. This was implemented for pooled trees with pool_stats, which would indicate:
- **slabs = p->nslabs:** How many slabs the pool has allocated already

- **live = p->nlive:** How many objects are currently in use (handed out by pool_alloc but not yet passed to pool_free)

- **free_objs = p->free_list + p->uncarved:** How many current slots are available aka how many pool_alloc calls will succeed before a new slab will need to be created.

Every slot in every slab satisfies the invariant that states they must be in exactly one state; live, on the free list, or not carved. This way: live + free_objs == slabs * objs_per_slab. Additionally, as mentioned earlier, I placed the pool_stats within the in-band header itself so that the metadata describing a slab is located within the slab itself.

## Where the Agent was Most and Least Reliable
Claude is highly reliable at creating detailed plans if you give it specific and thorough instruction to build one, for this portion of the project, I especially relied on chunkier plans at each Mutation. This allowed me to create a specific reference guideline that each new Agent could follow alongside their Claude.md instruction-set. I learned a lot about project management while playing around with this functionality, and found that creating stored markdown files of my plans also assisted me as an alternative to the traditional DEVLOG file for note taking. From here, I could guide the agent through exactly what I wanted, without any room for debate on semantics or missing instructions.

Claude was the least reliable when it came to requesting small incremental diffs, as the Agent often try to implement entire test-files at a time before doing any implementation. While I encouraged test-driven development, I had asked multiple times for Claude to only implement specific functions + their test cases at a time. An example that comes to mind is during Mutation 2, when I asked Claude to implement the geometry portion of pool.c, and it proceeded to propose implementation that included pool_alloc, free-list management, stats, etc etc. I rejected that diff given that it was much larger than was reasonable to commit at a time, and because development is best done thoroughly and kept in small bursts at a time.

## A Bug I caught + Poisoning Rationale
One important bug I caught was related to the difference in understanding between the Agent and I in regards to test files. For the first half of this project, I broke out individual test files for functions that I deemed in need of more thorough testing. While this served me well, when it came to working on pool.c, I realized Claude was continuing to run the new pool.c logic onto the old regression suite's test despite them being built for individual node allocation logic. This revealed some errors / leaks in my code, that I fixed by differentiating individual-node allocation logic testing versus pooled nodes logic. 

As for my rationale poisoning, I used a plain link with pool_free filling the whole stride with 0xDD to signify poison, then I wrote the raw next pointer over the first 8 bytes (which is the node's key). This was as on point to the spec as I believe I could get as this decision prioritized that bytes remained meaningful, and established two helpers (freelist_push / pop) as the owners of the link bytes (dead slots)
