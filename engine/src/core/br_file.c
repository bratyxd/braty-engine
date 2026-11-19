/* Files over SDL_IOStream helpers, save data over SDL_Storage. */
#include "app/br_engine_internal.h"

bool br_file_resolve(br_engine *e, const char *path, char *out, size_t out_size)
{
    bool ok;
    if (br_path_is_absolute(path) || e == NULL || e->asset_root == NULL) {
        ok = br_str_copy(out, out_size, path);
    } else {
        ok = br_path_join(out, out_size, e->asset_root, path);
    }
    if (!ok) {
        br_log_error("path too long: '%s'", path);
    }
    return ok;
}

bool br_file_read_all(br_engine *e, const char *path, void **bytes, size_t *size)
{
    char full[1024];
    *bytes = NULL;
    *size = 0;
    if (!br_file_resolve(e, path, full, sizeof(full))) {
        return false;
    }
    /* Read into a br_alloc block (callers br_free it); NUL-terminated so text parses in place. */
    SDL_IOStream *io = SDL_IOFromFile(full, "rb");
    Sint64 length = io != NULL ? SDL_GetIOSize(io) : -1;
    if (length < 0) {
        br_log_error("cannot read '%s': %s", full, SDL_GetError());
        if (io != NULL) {
            SDL_CloseIO(io);
        }
        return false;
    }
    uint8_t *data = br_alloc((size_t)length + 1);
    size_t got = data != NULL ? SDL_ReadIO(io, data, (size_t)length) : 0;
    SDL_CloseIO(io);
    if (data == NULL || got != (size_t)length) {
        br_log_error("cannot read '%s': %s", full, SDL_GetError());
        br_free(data);
        return false;
    }
    data[length] = 0;
    *bytes = data;
    *size = (size_t)length;
    return true;
}

bool br_file_write_all(br_engine *e, const char *path, const void *bytes, size_t size)
{
    char full[1024];
    if (!br_file_resolve(e, path, full, sizeof(full))) {
        return false;
    }
    if (!SDL_SaveFile(full, bytes, size)) {
        br_log_error("cannot write '%s': %s", full, SDL_GetError());
        return false;
    }
    return true;
}

bool br_file_exists(br_engine *e, const char *path)
{
    char full[1024];
    if (!br_file_resolve(e, path, full, sizeof(full))) {
        return false;
    }
    return SDL_GetPathInfo(full, NULL);
}

static SDL_Storage *br_save_storage(br_engine *e)
{
    if (e->save_storage == NULL) {
        e->save_storage = SDL_OpenUserStorage("braty", e->title, 0);
        if (e->save_storage == NULL) {
            br_log_error("cannot open save storage: %s", SDL_GetError());
            return NULL;
        }
    }
    // ponytail: busy-waits for storage readiness; desktop is ready at once, consoles may need an async callback.
    while (!SDL_StorageReady(e->save_storage)) {
        SDL_Delay(1);
    }
    return e->save_storage;
}

bool br_save_read(br_engine *e, const char *path, void **bytes, size_t *size)
{
    *bytes = NULL;
    *size = 0;
    SDL_Storage *storage = br_save_storage(e);
    if (storage == NULL) {
        return false;
    }
    Uint64 length = 0;
    if (!SDL_GetStorageFileSize(storage, path, &length)) {
        return false; /* missing save files are normal, not an error */
    }
    uint8_t *data = br_alloc((size_t)length + 1);
    if (data == NULL) {
        return false;
    }
    if (length > 0 && !SDL_ReadStorageFile(storage, path, data, length)) {
        br_log_error("cannot read save '%s': %s", path, SDL_GetError());
        br_free(data);
        return false;
    }
    data[length] = 0;
    *bytes = data;
    *size = (size_t)length;
    return true;
}

bool br_save_write(br_engine *e, const char *path, const void *bytes, size_t size)
{
    SDL_Storage *storage = br_save_storage(e);
    if (storage == NULL) {
        return false;
    }
    if (!SDL_WriteStorageFile(storage, path, bytes, size)) {
        br_log_error("cannot write save '%s': %s", path, SDL_GetError());
        return false;
    }
    return true;
}

bool br_save_remove(br_engine *e, const char *path)
{
    SDL_Storage *storage = br_save_storage(e);
    if (storage == NULL) {
        return false;
    }
    if (!SDL_RemoveStoragePath(storage, path)) {
        br_log_error("cannot remove save '%s': %s", path, SDL_GetError());
        return false;
    }
    return true;
}
