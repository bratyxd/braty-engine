/* br_assets: loader registry (by extension), cache keyed by path, aliases. Core of Task 2;
 * Task 5 extends this module (resolver, bundles, background loading, more loaders). */
#include "app/br_engine_internal.h"
#include "braty/br_assets.h"

typedef struct br_loader_entry {
    char *type_name;
    char *extensions;
    void *(*load)(br_engine *e, const char *path, const void *bytes, size_t size);
    void (*unload)(br_engine *e, void *asset);
} br_loader_entry;

typedef struct br_asset_entry {
    uint32_t loader;   /* index into loaders */
    void    *asset;
} br_asset_entry;

typedef struct br_assets_state {
    br_array loaders;  /* br_loader_entry; later entries win for the same extension */
    br_map   cache;    /* path -> br_asset_entry */
    br_map   aliases;  /* alias -> char * path (owned) */
} br_assets_state;

void br_assets_register_loader(br_engine *e, const br_asset_loader_desc *desc)
{
    if (desc == NULL || desc->type_name == NULL || desc->extensions == NULL || desc->load == NULL) {
        br_log_error("br_assets_register_loader: type_name, extensions and load are required");
        return;
    }
    br_loader_entry entry;
    entry.type_name = br_strdup(desc->type_name);
    entry.extensions = br_strdup(desc->extensions);
    entry.load = desc->load;
    entry.unload = desc->unload;
    br_array_push(&e->assets->loaders, &entry);
}

/* True if `ext` (any case) is one of the comma-separated lower-case `list`. */
static bool br_extension_in_list(const char *list, const char *ext)
{
    size_t ext_len = SDL_strlen(ext);
    const char *p = list;
    while (*p != 0) {
        const char *end = SDL_strchr(p, ',');
        size_t len = end != NULL ? (size_t)(end - p) : SDL_strlen(p);
        if (len == ext_len && SDL_strncasecmp(p, ext, len) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return false;
}

static const char *br_assets_resolve(br_assets_state *a, const char *path)
{
    char **target = br_map_get(&a->aliases, path);
    return target != NULL ? *target : path;
}

void *br_assets_load(br_engine *e, const char *path)
{
    br_assets_state *a = e->assets;
    if (path == NULL || path[0] == 0) {
        br_log_error("br_assets_load: empty path");
        return NULL;
    }
    path = br_assets_resolve(a, path);
    br_asset_entry *cached = br_map_get(&a->cache, path);
    if (cached != NULL) {
        return cached->asset;
    }
    const char *ext = br_path_extension(path);
    uint32_t loader_index = 0;
    br_loader_entry *loader = NULL;
    for (uint32_t i = a->loaders.count; i > 0; i--) {
        br_loader_entry *l = br_array_at(&a->loaders, i - 1);
        if (br_extension_in_list(l->extensions, ext)) {
            loader = l;
            loader_index = i - 1;
            break;
        }
    }
    if (loader == NULL) {
        br_log_error("asset '%s': no loader for extension '%s'", path, ext);
        return NULL;
    }
    void *bytes = NULL;
    size_t size = 0;
    if (!br_file_read_all(e, path, &bytes, &size)) {
        br_log_error("asset '%s': cannot read the file", path);
        return NULL;
    }
    // ponytail: synchronous load on the calling thread; Task 5 adds background loading.
    void *asset = loader->load(e, path, bytes, size);
    br_free(bytes);
    if (asset == NULL) {
        br_log_error("asset '%s': the %s loader failed", path, loader->type_name);
        return NULL;
    }
    br_asset_entry entry = { loader_index, asset };
    br_map_set(&a->cache, path, &entry);
    return asset;
}

void *br_assets_get(br_engine *e, const char *path)
{
    br_asset_entry *entry = br_map_get(&e->assets->cache, br_assets_resolve(e->assets, path));
    return entry != NULL ? entry->asset : NULL;
}

const char *br_assets_type(br_engine *e, const char *path)
{
    br_assets_state *a = e->assets;
    br_asset_entry *entry = br_map_get(&a->cache, br_assets_resolve(a, path));
    if (entry == NULL) {
        return NULL;
    }
    br_loader_entry *loader = br_array_at(&a->loaders, entry->loader);
    return loader->type_name;
}

void br_assets_unload(br_engine *e, const char *path)
{
    br_assets_state *a = e->assets;
    path = br_assets_resolve(a, path);
    br_asset_entry *found = br_map_get(&a->cache, path);
    if (found == NULL) {
        return;
    }
    br_asset_entry entry = *found;
    char key[1024];
    br_str_copy(key, sizeof(key), path); /* path may be the alias target we are about to keep */
    br_map_remove(&a->cache, key);
    br_loader_entry *loader = br_array_at(&a->loaders, entry.loader);
    if (loader->unload != NULL) {
        loader->unload(e, entry.asset);
    }
}

void br_assets_unload_all(br_engine *e)
{
    br_assets_state *a = e->assets;
    while (a->cache.count > 0) {
        uint32_t slot = 0;
        const char *key = NULL;
        void *value = NULL;
        if (!br_map_next(&a->cache, &slot, &key, &value)) {
            break;
        }
        char path[1024];
        br_str_copy(path, sizeof(path), key);
        br_assets_unload(e, path);
    }
}

void br_assets_add_alias(br_engine *e, const char *alias, const char *path)
{
    br_assets_state *a = e->assets;
    char **old = br_map_get(&a->aliases, alias);
    if (old != NULL) {
        br_free(*old);
    }
    char *copy = br_strdup(path);
    br_map_set(&a->aliases, alias, &copy);
}

uint32_t br_assets_count(br_engine *e)
{
    return e->assets->cache.count;
}

static void br_assets_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_assets_state *a = e->assets;
    br_assets_unload_all(e);
    for (uint32_t i = 0; i < a->loaders.count; i++) {
        br_loader_entry *l = br_array_at(&a->loaders, i);
        br_free(l->type_name);
        br_free(l->extensions);
    }
    uint32_t slot = 0;
    const char *key = NULL;
    void *value = NULL;
    while (br_map_next(&a->aliases, &slot, &key, &value)) {
        br_free(*(char **)value);
    }
    br_array_free(&a->loaders);
    br_map_free(&a->cache);
    br_map_free(&a->aliases);
    br_free(a);
    e->assets = NULL;
}

void br_assets_register(br_engine *e)
{
    br_assets_state *a = br_alloc(sizeof(br_assets_state));
    if (a == NULL) {
        return;
    }
    e->assets = a;
    br_array_init(&a->loaders, sizeof(br_loader_entry));
    br_map_init(&a->cache, sizeof(br_asset_entry));
    br_map_init(&a->aliases, sizeof(char *));
    br_engine_on_shutdown(e, br_assets_shutdown, NULL);
}
