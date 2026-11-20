/* .brpack asset packs: mount (validate the whole table of contents once), look up, read.
 * Every error names the pack and, where there is one, the entry. Layout: br_storage_internal.h. */
#include "core/br_storage_internal.h"

uint64_t br_pack_hash(const void *bytes, size_t size)
{
    const uint8_t *p = bytes;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t br_pack_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint32_t br_pack_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Byte order (unsigned), the order tools/pack_assets.py sorts by. SDL_strcmp compares plain
 * char, which is signed on some compilers and would misorder UTF-8 names. */
static int br_pack_compare(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x != 0 && *x == *y) {
        x++;
        y++;
    }
    return (int)*x - (int)*y;
}

static void br_pack_free(br_pack *p)
{
    br_free(p->file);
    br_free(p->names);
    br_free(p->entries);
    SDL_zerop(p);
}

/* Reads exactly `size` bytes at `offset` of an open stream. */
static bool br_pack_read_at(SDL_IOStream *io, uint64_t offset, void *out, size_t size)
{
    if (SDL_SeekIO(io, (Sint64)offset, SDL_IO_SEEK_SET) < 0) {
        return false;
    }
    return size == 0 || SDL_ReadIO(io, out, size) == size;
}

/* Parses and checks the table of contents into `pack`. False (logged) on anything hostile. */
static bool br_pack_parse_toc(br_pack *pack, const uint8_t *toc, uint32_t toc_size, uint32_t count, uint64_t file_size)
{
    const char *name = pack->file;
    /* every entry takes at least BR_PACK_ENTRY_FIXED + 1 bytes: a bigger count cannot be real */
    if (count > toc_size / (BR_PACK_ENTRY_FIXED + 1)) {
        br_log_error("pack '%s': %u entries cannot fit in a %u-byte table of contents", name, count, toc_size);
        return false;
    }
    pack->entries = count > 0 ? br_alloc(sizeof(br_pack_entry) * count) : NULL;
    pack->names = br_alloc((size_t)toc_size + 1);
    if ((count > 0 && pack->entries == NULL) || pack->names == NULL) {
        br_log_error("pack '%s': out of memory for the table of contents", name);
        return false;
    }
    uint32_t at = 0;
    size_t names_used = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (toc_size - at < BR_PACK_ENTRY_FIXED) {
            br_log_error("pack '%s': entry %u: table of contents truncated", name, i);
            return false;
        }
        br_pack_entry *entry = &pack->entries[i];
        entry->offset = br_pack_u64(toc + at);
        entry->size = br_pack_u64(toc + at + 8);
        entry->hash = br_pack_u64(toc + at + 16);
        uint32_t len = (uint32_t)toc[at + 24] | ((uint32_t)toc[at + 25] << 8);
        at += BR_PACK_ENTRY_FIXED;
        if (len == 0 || len > BR_PACK_PATH_MAX || toc_size - at < len) {
            br_log_error("pack '%s': entry %u: bad path length %u (1..%u, inside the table of contents)", name, i, len,
                         BR_PACK_PATH_MAX);
            return false;
        }
        char *path = pack->names + names_used;
        SDL_memcpy(path, toc + at, len);
        path[len] = 0;
        at += len;
        names_used += (size_t)len + 1;
        entry->path = path;
        /* the stored path must already be normalised: no '\\', '..', '.', empty or absolute parts */
        char normal[BR_PACK_PATH_MAX + 1];
        if (SDL_strlen(path) != len || !br_file_normalize(path, normal, sizeof(normal)) ||
            SDL_strcmp(normal, path) != 0 || SDL_strchr(path, ':') != NULL) {
            br_log_error("pack '%s': entry %u '%s': bad path (forward slashes, relative, no '..' / '.' parts)", name, i,
                         path);
            return false;
        }
        if (entry->size > file_size || entry->offset > file_size - entry->size) {
            br_log_error("pack '%s': entry '%s': points past the end of the file (offset %llu, size %llu, file %llu bytes)",
                         name, path, (unsigned long long)entry->offset, (unsigned long long)entry->size,
                         (unsigned long long)file_size);
            return false;
        }
        if (i > 0 && br_pack_compare(pack->entries[i - 1].path, path) >= 0) {
            br_log_error("pack '%s': entry '%s': duplicate or out of order (entries are sorted by path)", name, path);
            return false;
        }
    }
    if (at != toc_size) {
        br_log_error("pack '%s': %u stray bytes after the last entry of the table of contents", name, toc_size - at);
        return false;
    }
    pack->count = count;
    return true;
}

static bool br_pack_open(br_pack *pack, const char *full)
{
    SDL_zerop(pack);
    pack->file = br_strdup(full);
    SDL_IOStream *io = SDL_IOFromFile(full, "rb");
    if (io == NULL || pack->file == NULL) {
        br_log_error("pack '%s': cannot open: %s", full, SDL_GetError());
        if (io != NULL) {
            SDL_CloseIO(io);
        }
        return false;
    }
    Sint64 length = SDL_GetIOSize(io);
    uint8_t header[BR_PACK_HEADER_SIZE];
    bool ok = false;
    uint8_t *toc = NULL;
    if (length < (Sint64)BR_PACK_HEADER_SIZE || !br_pack_read_at(io, 0, header, sizeof(header))) {
        br_log_error("pack '%s': truncated header (%lld bytes)", full, (long long)length);
    } else if (SDL_memcmp(header, BR_PACK_MAGIC, 4) != 0) {
        br_log_error("pack '%s': not a .brpack (wrong magic)", full);
    } else if (br_pack_u32(header + 4) != BR_PACK_VERSION) {
        br_log_error("pack '%s': version %u, this engine reads version %u", full, br_pack_u32(header + 4), BR_PACK_VERSION);
    } else {
        uint64_t file_size = (uint64_t)length;
        uint32_t count = br_pack_u32(header + 8);
        uint32_t toc_size = br_pack_u32(header + 12);
        if (toc_size > file_size - BR_PACK_HEADER_SIZE) {
            br_log_error("pack '%s': table of contents (%u bytes) larger than the file (%llu bytes)", full, toc_size,
                         (unsigned long long)file_size);
        } else {
            toc = br_alloc((size_t)toc_size + 1);
            if (toc == NULL || !br_pack_read_at(io, BR_PACK_HEADER_SIZE, toc, toc_size)) {
                br_log_error("pack '%s': cannot read the table of contents", full);
            } else {
                ok = br_pack_parse_toc(pack, toc, toc_size, count, file_size);
            }
        }
    }
    br_free(toc);
    SDL_CloseIO(io);
    return ok;
}

bool br_file_mount_pack(br_engine *e, const char *path)
{
    br_storage_state *s = e->storage;
    char full[1024];
    if (path == NULL || !br_file_resolve(e, path, full, sizeof(full))) {
        return false;
    }
    br_pack pack;
    if (!br_pack_open(&pack, full)) {
        br_pack_free(&pack);
        return false;
    }
    SDL_LockMutex(s->mutex);
    bool pushed = br_array_push(&s->packs, &pack) != NULL;
    SDL_UnlockMutex(s->mutex);
    if (!pushed) {
        br_pack_free(&pack);
        return false;
    }
    br_log_info("pack '%s': mounted, %u entries", full, pack.count);
    return true;
}

uint32_t br_file_pack_count(br_engine *e)
{
    SDL_LockMutex(e->storage->mutex);
    uint32_t count = e->storage->packs.count;
    SDL_UnlockMutex(e->storage->mutex);
    return count;
}

void br_file_unmount_packs(br_engine *e)
{
    br_storage_state *s = e->storage;
    SDL_LockMutex(s->mutex);
    for (uint32_t i = 0; i < s->packs.count; i++) {
        br_pack_free(br_array_at(&s->packs, i));
    }
    br_array_free(&s->packs);
    SDL_UnlockMutex(s->mutex);
}

static const br_pack_entry *br_pack_find(const br_pack *pack, const char *key)
{
    uint32_t lo = 0;
    uint32_t hi = pack->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = br_pack_compare(pack->entries[mid].path, key);
        if (c == 0) {
            return &pack->entries[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

/* Copies what a read needs out of the table under the lock: the pack may be unmounted after. */
static bool br_pack_lookup(br_engine *e, const char *key, char *file, size_t file_size, br_pack_entry *out)
{
    br_storage_state *s = e->storage;
    bool found = false;
    SDL_LockMutex(s->mutex);
    for (uint32_t i = 0; i < s->packs.count && !found; i++) {
        const br_pack *pack = br_array_at(&s->packs, i);
        const br_pack_entry *entry = br_pack_find(pack, key);
        if (entry != NULL) {
            *out = *entry;
            found = br_str_copy(file, file_size, pack->file);
        }
    }
    SDL_UnlockMutex(s->mutex);
    return found;
}

bool br_pack_has(br_engine *e, const char *key)
{
    char file[1024];
    br_pack_entry entry;
    return br_pack_lookup(e, key, file, sizeof(file), &entry);
}

br_pack_read_result br_pack_read(br_engine *e, const char *key, void **bytes, size_t *size)
{
    char file[1024];
    br_pack_entry entry;
    if (!br_pack_lookup(e, key, file, sizeof(file), &entry)) {
        return BR_PACK_NOT_FOUND;
    }
    if (entry.size >= SIZE_MAX) {
        br_log_error("pack '%s': entry '%s': too large for this platform", file, key);
        return BR_PACK_READ_FAILED;
    }
    /* each read opens its own stream: reads on several threads never share a file position */
    SDL_IOStream *io = SDL_IOFromFile(file, "rb");
    uint8_t *data = io != NULL ? br_alloc((size_t)entry.size + 1) : NULL;
    bool ok = data != NULL && br_pack_read_at(io, entry.offset, data, (size_t)entry.size);
    if (io != NULL) {
        SDL_CloseIO(io);
    }
    if (!ok) {
        br_log_error("pack '%s': entry '%s': cannot read %llu bytes at %llu (file changed or truncated?)", file, key,
                     (unsigned long long)entry.size, (unsigned long long)entry.offset);
        br_free(data);
        return BR_PACK_READ_FAILED;
    }
    if (br_pack_hash(data, (size_t)entry.size) != entry.hash) {
        br_log_error("pack '%s': entry '%s': hash mismatch (corrupt pack)", file, key);
        br_free(data);
        return BR_PACK_READ_FAILED;
    }
    data[entry.size] = 0;
    *bytes = data;
    *size = (size_t)entry.size;
    return BR_PACK_READ_OK;
}
