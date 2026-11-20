/* The storage layer: every file the engine opens goes through here (API_REVIEW 51).
 *
 * Path bases (each defined once; the table is in br_core.h):
 *   - asset root (title storage): loose files under e->asset_root, then mounted .brpack packs
 *     (br_pack.c); in debug builds loose files win, in release builds packs win
 *   - executable folder: game.json and what it names (br_file_resolve_exe)
 *   - working directory: command-line paths (br_file_resolve_cwd)
 *   - user storage: saves and the log file (SDL_OpenUserStorage, below)
 *
 * ponytail: loose files use SDL_IOFromFile, not SDL_OpenTitleStorage: title storage takes only
 * relative paths under one root (absolute paths and --assets anywhere are used here) and makes
 * no thread-safety promise for the asset worker. A console port routes br_file_read_loose
 * through a title SDL_Storage. */
#include "core/br_storage_internal.h"

/* ---- paths */

bool br_file_normalize(const char *path, char *out, size_t out_size)
{
    size_t len = 0;
    size_t starts[128]; /* where each kept segment begins, so ".." can drop the last one */
    uint32_t depth = 0;
    const char *p = path != NULL ? path : "";
    if (out_size == 0 || br_path_is_absolute(p)) {
        return false;
    }
    while (*p != 0) {
        const char *seg = p;
        while (*p != 0 && *p != '/' && *p != '\\') {
            if ((unsigned char)*p < 32) {
                return false; /* control characters are never part of an asset path */
            }
            p++;
        }
        size_t seg_len = (size_t)(p - seg);
        if (*p != 0) {
            p++;
        }
        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
            continue;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth == 0) {
                return false; /* escapes the root */
            }
            len = starts[--depth];
            continue;
        }
        if (depth == BR_ARRAY_COUNT(starts)) {
            return false;
        }
        size_t at = len > 0 ? len + 1 : 0; /* one '/' between segments */
        if (at + seg_len + 1 > out_size) {
            return false;
        }
        starts[depth++] = len;
        if (len > 0) {
            out[len] = '/';
        }
        SDL_memcpy(out + at, seg, seg_len);
        len = at + seg_len;
    }
    out[len] = 0;
    return len > 0;
}

bool br_file_resolve(br_engine *e, const char *path, char *out, size_t out_size)
{
    if (path == NULL) {
        br_log_error("file: NULL path");
        return false;
    }
    if (br_path_is_absolute(path) || e == NULL || e->asset_root == NULL) {
        if (!br_str_copy(out, out_size, path)) {
            br_log_error("path too long: '%s'", path);
            return false;
        }
        return true;
    }
    char key[1024];
    if (!br_file_normalize(path, key, sizeof(key))) {
        br_log_error("bad asset path '%s' (empty, too long or '..' above the asset root)", path);
        return false;
    }
    if (!br_path_join(out, out_size, e->asset_root, key)) {
        br_log_error("path too long: '%s'", path);
        return false;
    }
    return true;
}

static bool br_file_resolve_under(const char *base, const char *path, char *out, size_t out_size)
{
    bool ok = false;
    if (path != NULL) {
        ok = br_path_is_absolute(path) || base == NULL ? br_str_copy(out, out_size, path)
                                                      : br_path_join(out, out_size, base, path);
    }
    if (!ok) {
        br_log_error("path too long: '%s'", path != NULL ? path : "(null)");
    }
    return ok;
}

bool br_file_resolve_exe(const char *path, char *out, size_t out_size)
{
    return br_file_resolve_under(SDL_GetBasePath(), path, out, out_size); /* SDL owns the base path */
}

bool br_file_resolve_cwd(const char *path, char *out, size_t out_size)
{
    char *cwd = SDL_GetCurrentDirectory();
    bool ok = br_file_resolve_under(cwd, path, out, out_size);
    SDL_free(cwd);
    return ok;
}

/* ---- reads and writes */

static bool br_file_read_loose(const char *full, void **bytes, size_t *size)
{
    /* Read into a br_alloc block (callers br_free it); NUL-terminated so text parses in place. */
    SDL_IOStream *io = SDL_IOFromFile(full, "rb");
    Sint64 length = io != NULL ? SDL_GetIOSize(io) : -1;
    if (length < 0 || (uint64_t)length >= SIZE_MAX) {
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

/* The pack key of a resolved relative path: the normalised part after "<asset root>/". */
static const char *br_file_key(br_engine *e, const char *full)
{
    return full + SDL_strlen(e->asset_root) + 1;
}

bool br_file_read_all(br_engine *e, const char *path, void **bytes, size_t *size)
{
    char full[1024];
    *bytes = NULL;
    *size = 0;
    if (!br_file_resolve(e, path, full, sizeof(full))) {
        return false;
    }
    if (e != NULL && e->storage != NULL && !br_path_is_absolute(path)) {
        bool loose = e->storage->loose_first && SDL_GetPathInfo(full, NULL);
        if (!loose) {
            br_pack_read_result r = br_pack_read(e, br_file_key(e, full), bytes, size);
            if (r != BR_PACK_NOT_FOUND) {
                return r == BR_PACK_READ_OK;
            }
        }
    }
    return br_file_read_loose(full, bytes, size);
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
    if (SDL_GetPathInfo(full, NULL)) {
        return true;
    }
    return e != NULL && e->storage != NULL && !br_path_is_absolute(path) && br_pack_has(e, br_file_key(e, full));
}

bool br_file_get_modify_time(br_engine *e, const char *path, int64_t *out)
{
    char full[1024];
    SDL_PathInfo info;
    *out = 0;
    if (!br_file_resolve(e, path, full, sizeof(full)) || !SDL_GetPathInfo(full, &info)) {
        return false;
    }
    *out = (int64_t)info.modify_time;
    return true;
}

void br_file_set_loose_first(br_engine *e, bool on)
{
    e->storage->loose_first = on;
}

bool br_file_loose_first(br_engine *e)
{
    return e->storage->loose_first;
}

/* ---- the log hook: the last error for the start-up message box, the text for the log file */

static void SDLCALL br_storage_log_output(void *user, int category, SDL_LogPriority priority, const char *message)
{
    br_storage_state *s = user;
    static const char *const names[] = { "", "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL" };
    /* SDL calls this from one thread at a time; the mutex orders it against br_storage_flush_log.
     * in_hook: a failing br_array call logs, which would come straight back here. */
    SDL_LockMutex(s->mutex);
    if (!s->log_in_hook) {
        s->log_in_hook = true;
        if (priority >= SDL_LOG_PRIORITY_ERROR) {
            br_str_copy(s->last_error, sizeof(s->last_error), message);
        }
        if (s->log_path != NULL && !s->log_truncated) {
            const char *level = (uint32_t)priority < BR_ARRAY_COUNT(names) ? names[priority] : "";
            uint32_t level_len = (uint32_t)SDL_strlen(level);
            uint32_t message_len = (uint32_t)SDL_min(SDL_strlen(message), 65536u);
            uint32_t need = s->log_text.count + level_len + message_len + 3;
            // ponytail: the log file keeps the first 1 MB of a run; a ring of the last lines if long sessions need the end.
            if (need > 1024u * 1024u || !br_array_reserve(&s->log_text, need)) {
                s->log_truncated = true;
            } else {
                br_array_push_n(&s->log_text, level, level_len);
                br_array_push_n(&s->log_text, ": ", 2);
                br_array_push_n(&s->log_text, message, message_len);
                br_array_push_n(&s->log_text, "\n", 1);
            }
        }
        s->log_in_hook = false;
    }
    SDL_UnlockMutex(s->mutex);
    if (s->log_prev != NULL) {
        s->log_prev(s->log_prev_user, category, priority, message);
    }
}

static void br_storage_unhook_log(br_engine *e)
{
    br_storage_state *s = e->storage;
    SDL_LogOutputFunction current = NULL;
    void *user = NULL;
    SDL_GetLogOutputFunction(&current, &user);
    if (s->log_hooked && current == br_storage_log_output && user == s) {
        SDL_SetLogOutputFunction(s->log_prev, s->log_prev_user); /* else someone hooked after us: theirs stays */
    }
    s->log_hooked = false;
}

const char *br_storage_last_error(br_engine *e)
{
    return e->storage->last_error;
}

void br_save_set_log_file(br_engine *e, const char *path)
{
    br_storage_state *s = e->storage;
    char *copy = path != NULL && path[0] != 0 ? br_strdup(path) : NULL;
    SDL_LockMutex(s->mutex);
    br_free(s->log_path);
    s->log_path = copy;
    SDL_UnlockMutex(s->mutex);
}

bool br_storage_log_location(br_engine *e, char *out, size_t out_size)
{
    if (e->storage->log_path == NULL) {
        return false;
    }
    /* desktop SDL user storage is the pref path; elsewhere the dialog shows the file name only */
    char *folder = SDL_GetPrefPath(e->save_org, e->save_app);
    int n = SDL_snprintf(out, out_size, "%s%s", folder != NULL ? folder : "", e->storage->log_path);
    SDL_free(folder);
    return n > 0 && (size_t)n < out_size;
}

void br_storage_flush_log(br_engine *e)
{
    br_storage_state *s = e->storage;
    /* copy under the lock, write without it: br_save_write may log */
    SDL_LockMutex(s->mutex);
    char *path = br_strdup(s->log_path);
    uint32_t count = s->log_text.count;
    char *text = path != NULL ? br_alloc((size_t)count + 1) : NULL;
    if (text != NULL && count > 0) {
        SDL_memcpy(text, s->log_text.data, count);
    }
    bool truncated = s->log_truncated;
    SDL_UnlockMutex(s->mutex);
    if (text != NULL) {
        text[count] = 0;
        if (truncated) {
            br_log_warn("log file '%s': over 1 MB, the rest of the run is not in it", path);
        }
        br_save_write(e, path, text, count);
    }
    br_free(text);
    br_free(path);
}

/* ---- module state */

static void br_storage_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_storage_state *s = e->storage;
    br_storage_flush_log(e);
    br_storage_unhook_log(e);
    br_file_unmount_packs(e);
    br_array_free(&s->log_text);
    br_free(s->log_path);
    SDL_DestroyMutex(s->mutex);
    br_free(s);
    e->storage = NULL;
}

void br_storage_register(br_engine *e)
{
    br_storage_state *s = br_alloc(sizeof(br_storage_state));
    if (s == NULL) {
        return;
    }
    s->mutex = SDL_CreateMutex();
    br_array_init(&s->packs, sizeof(br_pack));
    br_array_init(&s->log_text, 1);
#ifdef NDEBUG
    s->loose_first = false; /* a shipped game reads what it shipped with */
#else
    s->loose_first = true; /* edit a file in the data folder and it wins over a stale pack */
#endif
    e->storage = s;
    /* hooked before the debug console and play-test hook theirs: shutdown unhooks in reverse */
    SDL_GetLogOutputFunction(&s->log_prev, &s->log_prev_user);
    SDL_SetLogOutputFunction(br_storage_log_output, s);
    s->log_hooked = true;
    br_engine_on_shutdown(e, br_storage_shutdown, NULL);
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
