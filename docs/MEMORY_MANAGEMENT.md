# MiniOS memory management (Phase 6)

## What is REAL, what is NOT

| Feature | Status | Notes |
|---|---|---|
| Physical page-frame tracking (4 KiB frames) | **REAL** | `kernel/memory/pmm.c`, one state byte per frame |
| Reading the Multiboot memory map | **REAL** | falls back to `mem_lower`/`mem_upper` if no map is given |
| Kernel heap (`kmalloc`/`kfree`/`kcalloc`) | **REAL** | `kernel/memory/heap.c`, first-fit, split, coalesce |
| Heap integrity checking, double-free / bad-pointer detection | **REAL** | headers carry address-dependent magic numbers |
| Interrupt-safe allocation | **REAL** | `kmalloc` disables interrupts around heap changes |
| Guarded `peek` / `memdump` / `poke` | **REAL** | only RAM and the first MiB readable; `poke` only inside `alloc` blocks |
| Paging, virtual memory, address spaces | **NOT IMPLEMENTED** | `kernel/memory/paging.c` does not exist; all addresses are physical |
| Memory protection between processes | **NOT IMPLEMENTED** | flat model, everything runs in ring 0 |
| Per-process memory limits (`memlimit`) | **PLANNED** (Phase 7) | still a registered stub |
| RAM above 1 GiB | **NOT USED** | `PMM_MAX_FRAMES` limit (documented) |

Nothing here is simulated: `mem`, `free`, `pages`, `heap` and friends read the real allocator state.

## Boot-time layout

```
0x00000000 +-------------------------+
           | first MiB: BIOS, VGA,   |  RESERVED (never handed out)
0x00100000 +-------------------------+
           | kernel image (.text,    |  RESERVED  (kernel_start .. kernel_end,
           |  .data, .bss, PMM table,|             includes the boot stack)
           |  boot stack)            |
kernel_end +-------------------------+
           | free frames             |  FREE
           |  +-------------------+  |
           |  | kernel heap 4 MiB |  |  ALLOCATED (taken from the PMM by memory_init)
           |  +-------------------+  |
           | free frames             |  FREE
top of RAM +-------------------------+
```

`memory_init()` order: `pmm_init` -> add every *available* map region -> reserve first MiB, kernel image, Multiboot info and map -> take `min(4 MiB, free/2)` contiguous frames for the heap -> `heap_init`.

## Physical memory manager (`include/pmm.h`)

* `state[frame]` is `NONRAM`, `FREE`, `USED` or `RESERVED` (256 KiB table, up to 1 GiB).
* `pmm_alloc(count, &addr)`: first-fit run of `count` contiguous free frames; fails with `PMM_ERR_NO_MEMORY` even when enough *scattered* frames exist.
* `pmm_free(addr, count)`: all-or-nothing; only `USED` frames can be freed (not kernel/reserved/non-RAM frames, no double free).
* No 64-bit division anywhere (the kernel is not linked with libgcc); shifts only.

## Kernel heap (`include/heap.h`)

```
[ header 32 B | payload ][ header | payload ] ...   blocks in address order
header = { magic, requested, size, next, prev }
```

* Payloads are 16-byte aligned; sizes are rounded up to 16.
* **First fit**; a block is split when the remainder can hold a header plus 16 bytes.
* **free()** validates: inside heap -> aligned -> magic -> list membership, so an invalid pointer is refused instead of corrupting memory. It then poisons the payload with `0xDD`, marks the block free and merges with free neighbours.
* The magic number is `CONSTANT xor block address`, so a copied header at another address does not validate.
* `heap_check()` walks every block: magic, links, size continuity, last block reaches the end, and "no two adjacent free blocks".
* Invariant (tested): `total = payload_used + payload_free + header_overhead`.
* `heap_alloc(0)` returns `NULL` and is not counted as a failure.
* Allocation is O(blocks); fine for an educational 4 MiB heap.

## Error codes

| Module | Codes |
|---|---|
| heap | `HEAP_ERR_NULL`, `RANGE`, `BAD_POINTER`, `DOUBLE_FREE`, `CORRUPT`, `NOT_INIT`, `TOO_SMALL` (`heap_strerror`) |
| pmm | `PMM_ERR_ARG`, `NO_MEMORY`, `NOT_ALLOCATED`, `RANGE` (`pmm_strerror`) |
| memory | `MEM_ERR_NO_INFO`, `NO_RAM`, `NO_HEAP`, `HEAP_INIT` (`memory_strerror`) |

## Commands (all implemented in `commands/memory_commands.c`)

| Command | What it does |
|---|---|
| `mem`, `free`, `meminfo` | summary / table / detailed physical + heap statistics |
| `memmap` | Multiboot memory map as reported by the bootloader |
| `pages` | physical page-frame map plus runs (kernel, heap, free) |
| `heap`, `blocks` | heap layout with integrity check / list of allocated blocks |
| `alloc N`, `dealloc #`, `memfill # V` | allocate, free, fill (blocks are tracked by number) |
| `memstat`, `memfrag` | counters / fragmentation estimate |
| `memtest` | deterministic heap stress test with pattern verification |
| `memdump`, `peek`, `poke` | guarded raw memory access |
| `memlimit` | **stub** until the process manager exists (Phase 7) |

`pages` was registered as L3 (simulated) in Phase 5. It reads the real frame table, so it was
reclassified to **L2** (levels are now L1=51, L2=217, L3=32).

## Testing

`make test` (host-side, real kernel sources):

| File | Checks |
|---|---|
| `tests/test_memory.c` | numparse, PMM (fragmentation, double free, clamping, wrap-around), heap (exhaustion, coalescing in all orders, corruption, forged headers, 4000-op randomized stress with pattern checks), `memory_init` with fake Multiboot maps (tiny RAM, no map, malformed entry, too many regions, readable-range guard) |
| `tests/test_memory_commands.c` | every memory command through the real parser and registry on the real heap: lifecycle, invalid input, exhaustion, guarded peek/poke, memtest pass **and** fail, corrupted heap |

The tests were also checked by mutation: deliberately breaking the allocator (no merge, no double-free
check, wrong alignment, no poisoning, ...) makes them fail.

In-kernel self-tests: the heap self-test at boot and the `memtest` command.

## Limitations

* No virtual memory or protection; a buggy command can still overwrite kernel memory (except where guarded).
* Heap is one fixed 4 MiB region; it cannot grow.
* Linear-scan allocator; no size classes or free lists.
* Only the first 1 GiB of RAM is used.
* `poke` writes one byte at a time and only into `alloc` blocks.
