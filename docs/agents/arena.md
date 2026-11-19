# arena - linear, stack, pool, scratch and frame arenas

Header: `engine/include/braty/br_arena.h`. Source: `engine/src/core/br_arena.c`.
Test: `tests/test_arena.c`. Allocator struct and heap stats: `br_core.h` (`docs/agents/core.md`).

Arenas hand out memory from one big block instead of one heap call per object. Every arena is a
plain struct with `stats` (name, used, peak, capacity), zeroes what it hands out, and has a
`br_allocator` view so `br_array` / `br_pool` / `br_map` can live inside it.

| Arena | Use it for |
|---|---|
| `br_arena_linear` | Bump allocation, free all at once (`reset`). Fixed, or growable (chains blocks; `reset` merges them into one). |
| `br_arena_stack` | Linear with `push_marker` / `pop_to_marker` (LIFO). Fixed capacity. |
| `br_arena_pool` | Fixed-size blocks with a free list (`alloc` / `free` one block). |
| `br_arena_scratch` | `begin` / `end` pair over a linear arena: everything allocated in between is dropped. |
| frame arena | `br_frame_alloc(e, size)`: memory valid until the next frame starts. 1 MB, growable. |

## Functions you actually need

| Function | What for |
|---|---|
| `br_arena_linear_create(&a, name, bytes, growable)` / `_create_in(&a, name, buf, size)` | Heap-backed / caller memory (no heap use, fixed). Same `_create_in` for stack and pool. |
| `br_arena_linear_alloc(&a, size)`, `_alloc_aligned(&a, size, align)` | Zeroed bytes; NULL + log when a fixed arena is full or `align` is not a power of two. |
| `br_arena_linear_reset(&a)`, `br_arena_linear_destroy(&a)` | Forget everything / free the blocks. |
| `br_arena_stack_push_marker`, `br_arena_stack_pop_to_marker` | Nested temporary allocations. |
| `br_arena_pool_create(&p, name, block_size, align, count)`, `_alloc`, `_free` | Many same-sized objects. |
| `br_arena_scratch_begin(&a)` / `br_arena_scratch_end(s)` | Temporary scope on a linear arena (no thread-locals: pass the arena). |
| `br_frame_alloc(e, size)`, `br_frame_arena(e)` | Per-frame scratch memory, reset by the first INPUT system. |
| `br_arena_linear_allocator(&a)` + `br_array_init_with_allocator(...)` | Containers on an arena (also `br_pool_`/`br_map_init_with_allocator`). |
| `br_arena_track(e, &a.stats)` / `br_arena_untrack` | List your arena in the debug overlay (Stats -> Arenas). |
| `br_memory_get_stats(&s)` | Heap counters: live blocks/bytes, total allocations, peak bytes. |

## Snippet (from tests/test_arena.c)

```c
uint8_t buffer[16 * 1024];
br_arena_linear a;
br_arena_linear_create_in(&a, "containers", buffer, sizeof(buffer));

br_array arr;
br_array_init_with_allocator(&arr, sizeof(int), br_arena_linear_allocator(&a));
for (int i = 0; i < 200; i++) {
    br_array_push(&arr, &i);           /* storage lives in `buffer` */
}

br_arena_scratch scratch = br_arena_scratch_begin(&a);
br_arena_linear_alloc(&a, 64);
br_arena_scratch_end(scratch);         /* the 64 bytes are gone again */

uint8_t *p = br_frame_alloc(e, 4096);  /* valid until the next br_engine_step */
```

## Gotchas

- An arena's allocator view points at the arena struct: do not copy or move an arena after
  `create`, and keep it alive as long as any container using it.
- Bump arenas ignore `free`; a container on one leaks its old storage when it grows (the newest
  allocation grows in place). Reset or end the scratch scope to get it back.
- Stack markers must be popped newest first. Debug builds (`NDEBUG` not defined) reject an
  out-of-order pop: log, return false, `order_errors` += 1. Release builds pop the newer markers too.
- A pool's allocator view only serves requests up to `block_size`; containers that grow do not fit.
- Frame memory dies when the next frame starts (the reset is the first INPUT-phase system), so
  never keep a frame pointer in a component or across `br_engine_step`.
- `br_arena_untrack` before destroying a tracked arena, or the overlay reads freed memory.
- Arenas are not thread-safe. Heap stats are (spinlock), and process-wide, not per engine.
- `br_alloc` blocks carry a size header: never free them with `SDL_free`/`free`, and never
  `br_free` memory from SDL (`SDL_LoadFile`, ...) or third-party libraries.
