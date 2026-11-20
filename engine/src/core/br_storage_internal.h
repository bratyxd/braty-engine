/* The storage layer's state (br_file.c, br_pack.c): mounted packs and the log file. */
#ifndef BR_STORAGE_INTERNAL_H
#define BR_STORAGE_INTERNAL_H

#include "app/br_engine_internal.h"

/* .brpack layout (little endian; tools/pack_assets.py writes it, docs/DATA_FORMATS.md):
 *   header  "BRPK", u32 version, u32 entry count, u32 table-of-contents size in bytes
 *   toc     per entry: u64 offset, u64 size, u64 FNV-1a 64 hash of the bytes, u16 path length,
 *           path bytes (normalised, no NUL), entries sorted by path (byte order), no duplicates
 *   data    raw entry bytes at their offsets (from the start of the file) */
#define BR_PACK_MAGIC "BRPK"
#define BR_PACK_VERSION 1u
#define BR_PACK_HEADER_SIZE 16u
#define BR_PACK_ENTRY_FIXED 26u    /* offset + size + hash + path length */
#define BR_PACK_PATH_MAX 255u      /* longest entry path in bytes */

typedef struct br_pack_entry {
    const char *path; /* into br_pack.names */
    uint64_t    offset;
    uint64_t    size;
    uint64_t    hash;
} br_pack_entry;

typedef struct br_pack {
    char          *file;    /* absolute path of the .brpack */
    char          *names;   /* every entry path, NUL-terminated, back to back */
    br_pack_entry *entries; /* sorted by path */
    uint32_t       count;
} br_pack;

typedef struct br_storage_state {
    /* Guards `packs` (mounts happen on the main thread, reads also on the asset worker) and the
     * log buffer (log lines come from any thread). Never held while logging. */
    SDL_Mutex *mutex;
    br_array   packs;       /* br_pack, in mount order: the first mounted pack wins */
    bool       loose_first; /* loose files before packs (default in debug builds) */

    /* log hook, installed at register (br_save_set_log_file and the start-up message box) */
    bool                  log_hooked;
    bool                  log_in_hook;   /* re-entry guard (an allocation failure logs) */
    SDL_LogOutputFunction log_prev;
    void                 *log_prev_user;
    char                 *log_path;      /* save path of the log file, NULL = no file */
    br_array              log_text;      /* bytes logged since the hook went in */
    bool                  log_truncated;
    char                  last_error[512];
} br_storage_state;

typedef enum br_pack_read_result {
    BR_PACK_NOT_FOUND,
    BR_PACK_READ_OK,
    BR_PACK_READ_FAILED, /* logged with the pack and entry */
} br_pack_read_result;

/* Reads `key` (a normalised relative path) from the first mounted pack holding it.
 * Safe from any thread. */
br_pack_read_result br_pack_read(br_engine *e, const char *key, void **bytes, size_t *size);
/* True if a mounted pack holds `key`. Safe from any thread. */
bool br_pack_has(br_engine *e, const char *key);
/* FNV-1a 64 of bytes (the pack entry hash). */
uint64_t br_pack_hash(const void *bytes, size_t size);

/* Allocates the storage state and hooks the log (the last error line for the start-up dialog,
 * the text for the log file); unhooks and frees at shutdown. */
void br_storage_register(br_engine *e);

/* The last error line logged since the hook went in ("" if none). */
const char *br_storage_last_error(br_engine *e);
/* Where the log file is on disk (the user storage folder on desktop). False if none is set. */
bool br_storage_log_location(br_engine *e, char *out, size_t out_size);
/* Writes the log text to the log file now (shutdown and a failed start call it). */
void br_storage_flush_log(br_engine *e);

#endif
