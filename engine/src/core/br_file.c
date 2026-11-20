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

/* A copy of name usable as a folder name on every desktop OS: characters Windows refuses
 * (<>:"/\|?* and control characters) become '_', trailing dots and spaces go, empty -> "braty".
 * Titles that were valid folder names come out unchanged, so their saves stay where they were. */
char *br_save_name_sanitize(const char *name)
{
    char out[128];
    size_t len = 0;
    for (const char *c = name != NULL ? name : ""; *c != 0 && len + 1 < sizeof(out); c++) {
        bool bad = (unsigned char)*c < 32 || SDL_strchr("<>:\"/\\|?*", *c) != NULL;
        out[len++] = bad ? '_' : *c;
    }
    while (len > 0 && (out[len - 1] == '.' || out[len - 1] == ' ')) {
        len--;
    }
    out[len] = 0;
    return br_strdup(len > 0 ? out : "braty");
}

void br_engine_set_save_name(br_engine *e, const char *org, const char *app)
{
    if (org != NULL && org[0] != 0) {
        br_free(e->save_org);
        e->save_org = br_save_name_sanitize(org);
    }
    if (app != NULL && app[0] != 0) {
        br_free(e->save_app);
        e->save_app = br_save_name_sanitize(app);
    }
    if (e->save_storage != NULL) {
        SDL_CloseStorage(e->save_storage); /* reopened under the new name on the next access */
        e->save_storage = NULL;
    }
}

static SDL_Storage *br_save_storage(br_engine *e)
{
    if (e->save_storage == NULL) {
        if (e->save_org == NULL || e->save_app == NULL) {
            return NULL; /* out of memory at create */
        }
        e->save_storage = SDL_OpenUserStorage(e->save_org, e->save_app, 0);
        if (e->save_storage == NULL) {
            br_log_error("cannot open save storage '%s/%s': %s", e->save_org, e->save_app, SDL_GetError());
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
    /* write a temp file, then rename it over the save: a crash mid-write leaves the old save */
    char tmp[1024];
    if (SDL_snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        br_log_error("save path too long: '%s'", path);
        return false;
    }
    if (!SDL_WriteStorageFile(storage, tmp, bytes, size)) {
        br_log_error("cannot write save '%s': %s", tmp, SDL_GetError());
        return false;
    }
    if (!SDL_RenameStoragePath(storage, tmp, path)) {
        br_log_error("cannot replace save '%s': %s", path, SDL_GetError());
        SDL_RemoveStoragePath(storage, tmp);
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
