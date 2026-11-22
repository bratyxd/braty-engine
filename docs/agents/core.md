# core - memory, logging, containers, strings, files, packs, save data

Header: `engine/include/braty/br_core.h`. Source: `engine/src/core/` (the storage layer:
`br_file.c`, `br_pack.c`). Tests: `tests/test_core.c`, `tests/test_pack.c` (packs, hostile packs,
async reads from a pack, the log file).

Plain C over SDL. No window or GPU needed. Arenas (linear, stack, pool, scratch, per-frame):
`docs/agents/arena.md`. Background jobs on worker threads (`br_job`): `docs/agents/job.md`.
Containers own their memory and are freed by their
`*_free` function.

## Functions you actually need

| Function | What for |
|---|---|
| `br_alloc(size)` / `br_free(p)` | Zeroed allocation / free (SDL allocator, counted). |
| `br_memory_get_stats(&s)` | Heap counters (live blocks/bytes, total allocations, peak). |
| `br_array/pool/map_init_with_allocator(..., alloc)` | Container on a `br_allocator` (an arena); NULL = heap. Arenas: `docs/agents/arena.md`. |
| `br_log_info/warn/error(fmt, ...)` | Logging. Call `br_log_error` before returning failure. |
| `br_array_init/push/at/remove_swap/free` | Growable array of fixed-size elements. |
| `br_pool_alloc/get/release` | Slots addressed by (index, generation); stale handles return NULL. |
| `br_map_init/set/get/remove/next` | String key -> fixed-size value. |
| `br_file_read_all(e, path, &bytes, &size)` | Whole file, relative to the asset root (loose or from a mounted pack), NUL-terminated. |
| `br_file_mount_pack(e, path)` | Mount a `.brpack` (`tools/pack_assets.py`; game.json `"packs"`, `--pack`). Validated whole; `br_file_unmount_packs`, `br_file_pack_count`. |
| `br_file_set_loose_first(e, on)` | Loose files over packs (debug default) or packs first (release default). |
| `br_file_resolve` / `_resolve_exe` / `_resolve_cwd` | A path against the asset root / the executable folder / the working directory. |
| `br_file_normalize(path, out, n)` | Asset path rules: `/` separators, `a/../b` -> `b`, nothing above the root. |
| `br_save_set_log_file(e, "log.txt")` | Mirror the log into user storage (written at shutdown and on a failed start; on in release builds for interactive runs). |
| `br_save_read/write(e, path, ...)` | Save data in per-user storage (SDL_Storage). Writes are atomic (temp file + rename). |
| `br_engine_set_save_name(e, org, app)` | Save folder identity (default `"braty"` + window title, sanitised); also `br_app_desc.save_org/save_app`, game.json `"save"`. |
| `br_u32_mul_ok(a, b, &out)`, `br_size_mul_ok` | `count * stride` from data files without wrapping (containers reject wrapping counts themselves). |
| `br_path_join`, `br_path_extension`, `br_str_*` | Small string helpers. |

## Snippet (from tests/test_core.c)

```c
br_array a;
br_array_init(&a, sizeof(int));
for (int i = 0; i < 100; i++) {
    br_array_push(&a, &i);
}
int *x = br_array_at(&a, 42);            /* NULL if out of range */
br_array_free(&a);

br_pool p;
br_pool_init(&p, sizeof(float));
uint32_t index, generation;
float *f = br_pool_alloc(&p, &index, &generation);
br_pool_release(&p, index, generation);
/* br_pool_get(&p, index, generation) is now NULL: stale handles are detected */
br_pool_free(&p);

void *bytes; size_t size;
if (br_file_read_all(e, "data/level1.json", &bytes, &size)) {
    /* bytes is NUL-terminated: parse text in place */
    br_free(bytes);
}
```

## Gotchas

- Pointers into a `br_array`, `br_pool` or `br_map` are invalidated by anything that grows it.
  Keep indices or handles, not pointers, across frames.
- `br_pool` generation 0 is never handed out, so a zeroed handle is always invalid.
- `br_map` copies keys; values are raw bytes of `value_size`.
- Path bases (the table at the `files` section of `br_core.h`): the asset root for every
  `br_file_*` / asset / scene path (loose files, then mounted packs), the executable folder for
  game.json and what it names, the working directory for command-line paths, user storage for
  saves and the log file. Absolute paths pass through and never look in packs. No engine code
  opens a file any other way (`grep SDL_IOFromFile engine/src` finds only `br_file.c` and
  `br_pack.c`). Never use `fopen`: files go through SDL so consoles work.
- Asset paths are normalised (`\` -> `/`, `.` and empty parts dropped, `a/../b` -> `b`); one
  that climbs above the asset root fails, logged. Pack lookups are case-sensitive; loose files
  follow the OS, so a wrong case can work loose on Windows and fail from a pack.
- Packs: the first mounted pack holding a path wins; loose files win in debug builds, packs in
  release builds (`NDEBUG`). Reads are safe from any thread (the asset worker reads packs: the
  table is looked up under a mutex and each read opens its own stream); mount and unmount on
  the main thread. Mounting checks the whole table (truncation, magic, version, sizes, offsets
  without overflow, paths, order / duplicates) and each read checks the entry's FNV-1a 64 hash;
  failures log the pack and entry and fall back to nothing worse than "file not found".
- Hot reload watches loose files only (`br_file_get_modify_time`); packed scenes never reload.
- `br_file_write_all` writes loose files under the asset root (development tools); a shipped
  game writes to user storage (`br_save_*`).
- `br_save_*` blocks until the storage is ready (instant on desktop; see the `ponytail:` note).
- `br_save_write` writes `<path>.tmp` and renames it over the save, so a crash mid-write keeps the
  old save; a stray `.tmp` after a crash is overwritten by the next write.
- Do not `br_map_remove` while walking a map with `br_map_next` (entries shift back past the
  cursor and are skipped): collect the keys first, or restart from slot 0 after each removal.
- `br_array` counts are uint32: `push_n` / `insert` past UINT32_MAX and reserves whose bytes do
  not fit return NULL / false (logged) instead of wrapping.
- `BR_ARRAY_COUNT(a)` is the only helper macro.
- `br_alloc` blocks carry a size header (for the stats): pair `br_alloc`/`br_realloc`/`br_strdup`/
  `br_file_read_all`/`br_save_read` with `br_free` only, and SDL/third-party memory with its own free.
