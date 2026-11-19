# core - memory, logging, containers, strings, files, save data

Header: `engine/include/braty/br_core.h`. Source: `engine/src/core/`. Test: `tests/test_core.c`.

Plain C over SDL. No window or GPU needed. Containers own their memory and are freed by their
`*_free` function.

## Functions you actually need

| Function | What for |
|---|---|
| `br_alloc(size)` / `br_free(p)` | Zeroed allocation / free (SDL allocator). |
| `br_log_info/warn/error(fmt, ...)` | Logging. Call `br_log_error` before returning failure. |
| `br_array_init/push/at/remove_swap/free` | Growable array of fixed-size elements. |
| `br_pool_alloc/get/release` | Slots addressed by (index, generation); stale handles return NULL. |
| `br_map_init/set/get/remove/next` | String key -> fixed-size value. |
| `br_file_read_all(e, path, &bytes, &size)` | Whole file, relative to the asset root, NUL-terminated. |
| `br_save_read/write(e, path, ...)` | Save data in per-user storage (SDL_Storage). |
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
- Relative paths resolve against the asset root (`br_engine_asset_root`); absolute paths pass
  through. Never use `fopen`: files go through SDL so consoles work.
- `br_save_*` blocks until the storage is ready (instant on desktop; see the `ponytail:` note).
- `BR_ARRAY_COUNT(a)` is the only helper macro.
