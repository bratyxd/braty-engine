/* br_assets_resolve: the resolver (aliases, base path, "{a,b}" patterns, resolution/format
 * preferences) and manifests + bundles (PixiJS manifest JSON). */
#include "assets/br_assets_internal.h"

#include "cjson/cJSON.h"

#define BR_RESOLVE_MAX 16        /* candidates per name after pattern expansion */
#define BR_RESOLVE_PATH 512

typedef struct br_candidates {
    char     items[BR_RESOLVE_MAX][BR_RESOLVE_PATH];
    uint32_t count;
} br_candidates;

/* ---- preferences */

void br_assets_set_base_path(br_engine *e, const char *base_path)
{
    br_assets_state *a = e->assets;
    br_free(a->base_path);
    a->base_path = base_path != NULL && base_path[0] != 0 ? br_strdup(base_path) : NULL;
}

void br_assets_set_preferences(br_engine *e, float resolution, const char *formats)
{
    br_assets_state *a = e->assets;
    a->pref_resolution = resolution > 0.0f ? resolution : 1.0f;
    br_free(a->pref_formats);
    a->pref_formats = formats != NULL && formats[0] != 0 ? br_strdup(formats) : NULL;
}

float br_assets_resolution_of(const char *path)
{
    const char *name = SDL_strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    const char *at = SDL_strrchr(name, '@');
    if (at == NULL) {
        return 1.0f;
    }
    char *end = NULL;
    double value = SDL_strtod(at + 1, &end);
    if (end == at + 1 || *end != 'x' || value <= 0.0) {
        return 1.0f;
    }
    if (end[1] != '.' && end[1] != 0) {
        return 1.0f;
    }
    return (float)value;
}

/* ---- pattern expansion: "a{1,2}b{x,y}" -> a1bx, a1by, a2bx, a2by */

/* Appends len bytes of s to dst (size BR_RESOLVE_PATH). False if it does not fit. */
static bool br_append(char *dst, size_t *used, const char *s, size_t len)
{
    if (*used + len >= BR_RESOLVE_PATH) {
        return false;
    }
    SDL_memcpy(dst + *used, s, len);
    *used += len;
    dst[*used] = 0;
    return true;
}

static void br_expand(const char *pattern, br_candidates *c)
{
    const char *open = SDL_strchr(pattern, '{');
    const char *close = open != NULL ? SDL_strchr(open, '}') : NULL;
    if (open == NULL || close == NULL) {
        if (c->count < BR_RESOLVE_MAX && br_str_copy(c->items[c->count], BR_RESOLVE_PATH, pattern)) {
            c->count++;
        }
        return;
    }
    const char *option = open + 1;
    for (;;) {
        const char *end = option;
        while (end < close && *end != ',') {
            end++;
        }
        char buf[BR_RESOLVE_PATH];
        size_t used = 0;
        buf[0] = 0;
        bool ok = br_append(buf, &used, pattern, (size_t)(open - pattern)) &&
                  br_append(buf, &used, option, (size_t)(end - option)) &&
                  br_append(buf, &used, close + 1, SDL_strlen(close + 1));
        if (ok) {
            br_expand(buf, c);
        }
        if (end >= close) {
            break;
        }
        option = end + 1;
    }
}

/* Position of ext in the comma-separated formats list; large if absent. */
static int br_format_rank(const char *formats, const char *ext)
{
    if (formats == NULL) {
        return 0;
    }
    size_t ext_len = SDL_strlen(ext);
    int rank = 0;
    const char *p = formats;
    while (*p != 0) {
        const char *end = SDL_strchr(p, ',');
        size_t len = end != NULL ? (size_t)(end - p) : SDL_strlen(p);
        if (len == ext_len && SDL_strncasecmp(p, ext, len) == 0) {
            return rank;
        }
        if (end == NULL) {
            break;
        }
        rank++;
        p = end + 1;
    }
    return 1000;
}

/* Expands every src, picks the preferred candidate, applies the base path. */
static bool br_assets_resolve_srcs(br_assets_state *a, const char *const *srcs, uint32_t count, char *out,
                                   size_t out_size)
{
    br_candidates *c = br_alloc(sizeof(br_candidates)); /* 8 KB: keep it off the stack */
    if (c == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        br_expand(srcs[i], c);
    }
    if (c->count == 0) {
        br_log_error("assets: cannot resolve '%s'", count > 0 ? srcs[0] : "");
        br_free(c);
        return false;
    }
    uint32_t best = 0;
    float best_distance = 0.0f;
    int best_rank = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        float distance = SDL_fabsf(br_assets_resolution_of(c->items[i]) - a->pref_resolution);
        int rank = br_format_rank(a->pref_formats, br_path_extension(c->items[i]));
        if (i == 0 || distance < best_distance || (distance == best_distance && rank < best_rank)) {
            best = i;
            best_distance = distance;
            best_rank = rank;
        }
    }
    bool ok;
    if (a->base_path != NULL && !br_path_is_absolute(c->items[best])) {
        ok = br_path_join(out, out_size, a->base_path, c->items[best]);
    } else {
        ok = br_str_copy(out, out_size, c->items[best]);
    }
    if (!ok) {
        br_log_error("assets: path too long: '%s'", c->items[best]);
    }
    br_free(c);
    return ok;
}

bool br_assets_resolve_internal(br_assets_state *a, const char *name, char *out, size_t out_size)
{
    char **target = br_map_get(&a->aliases, name);
    if (target != NULL) {
        name = *target; /* alias targets are used as given */
        if (!br_str_copy(out, out_size, name)) {
            br_log_error("assets: path too long: '%s'", name);
            return false;
        }
        return true;
    }
    if (br_map_get(&a->cache, name) != NULL) {
        return br_str_copy(out, out_size, name); /* already a resolved cache key */
    }
    return br_assets_resolve_srcs(a, &name, 1, out, out_size);
}

bool br_assets_resolve(br_engine *e, const char *name, char *out, size_t out_size)
{
    if (name == NULL || out == NULL || out_size == 0) {
        return false;
    }
    return br_assets_resolve_internal(e->assets, name, out, out_size);
}

/* ---- bundles */

static br_bundle *br_bundle_find(br_assets_state *a, const char *name)
{
    for (uint32_t i = 0; i < a->bundles.count; i++) {
        br_bundle *b = br_array_at(&a->bundles, i);
        if (br_str_equal(b->name, name)) {
            return b;
        }
    }
    return NULL;
}

static br_bundle *br_bundle_get_or_add(br_assets_state *a, const char *name)
{
    br_bundle *b = br_bundle_find(a, name);
    if (b != NULL) {
        return b;
    }
    br_bundle fresh;
    fresh.name = br_strdup(name);
    br_array_init(&fresh.assets, sizeof(br_bundle_asset));
    return br_array_push(&a->bundles, &fresh);
}

/* Collects a string or an array of strings into out (max `max`). Returns the count. */
static uint32_t br_json_strings(const cJSON *item, const char **out, uint32_t max)
{
    uint32_t n = 0;
    if (cJSON_IsString(item)) {
        out[0] = item->valuestring;
        return 1;
    }
    if (cJSON_IsArray(item)) {
        for (const cJSON *child = item->child; child != NULL && n < max; child = child->next) {
            if (cJSON_IsString(child)) {
                out[n] = child->valuestring;
                n++;
            }
        }
    }
    return n;
}

/* Adds one manifest asset: resolves its src, points its aliases at the result. */
static bool br_bundle_add_asset(br_engine *e, br_bundle *bundle, const cJSON *alias, const cJSON *src,
                                const char *type)
{
    br_assets_state *a = e->assets;
    const char *srcs[BR_RESOLVE_MAX];
    uint32_t src_count = br_json_strings(src, srcs, BR_RESOLVE_MAX);
    if (src_count == 0) {
        br_log_error("manifest bundle '%s': an asset has no \"src\"", bundle->name);
        return false;
    }
    char path[BR_RESOLVE_PATH];
    if (!br_assets_resolve_srcs(a, srcs, src_count, path, sizeof(path))) {
        return false;
    }
    const char *aliases[BR_RESOLVE_MAX];
    uint32_t alias_count = br_json_strings(alias, aliases, BR_RESOLVE_MAX);
    for (uint32_t i = 0; i < alias_count; i++) {
        br_assets_add_alias(e, aliases[i], path);
    }
    br_bundle_asset entry;
    entry.path = br_strdup(path);
    entry.type = type != NULL ? br_strdup(type) : NULL;
    br_array_push(&bundle->assets, &entry);
    return true;
}

bool br_assets_add_manifest(br_engine *e, const char *json, size_t size)
{
    br_assets_state *a = e->assets;
    if (json == NULL) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(json, size);
    if (root == NULL) {
        const char *where = cJSON_GetErrorPtr();
        br_log_error("manifest: bad JSON at byte %d", where != NULL ? (int)(where - json) : -1);
        return false;
    }
    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(root, "bundles");
    if (!cJSON_IsArray(bundles)) {
        br_log_error("manifest: missing \"bundles\" array");
        cJSON_Delete(root);
        return false;
    }
    bool ok = true;
    for (const cJSON *b = bundles->child; b != NULL; b = b->next) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(b, "name");
        const cJSON *assets = cJSON_GetObjectItemCaseSensitive(b, "assets");
        if (!cJSON_IsString(name)) {
            br_log_error("manifest: a bundle has no \"name\"");
            ok = false;
            continue;
        }
        br_bundle *bundle = br_bundle_get_or_add(a, name->valuestring);
        if (bundle == NULL) {
            ok = false;
            continue;
        }
        if (cJSON_IsArray(assets)) {
            for (const cJSON *item = assets->child; item != NULL; item = item->next) {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
                ok = br_bundle_add_asset(e, bundle, cJSON_GetObjectItemCaseSensitive(item, "alias"),
                                         cJSON_GetObjectItemCaseSensitive(item, "src"),
                                         cJSON_IsString(type) ? type->valuestring : NULL) && ok;
            }
        } else if (cJSON_IsObject(assets)) {
            /* { "alias": "src" } form; the key is the alias */
            for (const cJSON *item = assets->child; item != NULL; item = item->next) {
                cJSON *alias = cJSON_CreateString(item->string);
                ok = br_bundle_add_asset(e, bundle, alias, item, NULL) && ok;
                cJSON_Delete(alias);
            }
        } else {
            br_log_error("manifest bundle '%s': missing \"assets\"", bundle->name);
            ok = false;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool br_assets_load_manifest(br_engine *e, const char *path)
{
    void *bytes = NULL;
    size_t size = 0;
    if (path == NULL || !br_file_read_all(e, path, &bytes, &size)) {
        br_log_error("manifest '%s': cannot read the file", path != NULL ? path : "");
        return false;
    }
    bool ok = br_assets_add_manifest(e, bytes, size);
    if (!ok) {
        br_log_error("manifest '%s': not fully loaded", path);
    }
    br_free(bytes);
    return ok;
}

static void br_bundle_asset_done(br_engine *e, const char *path, void *asset, void *user)
{
    (void)path;
    (void)asset;
    br_bundle_load *load = user;
    load->done++;
    if (load->fn != NULL) {
        load->fn(e, load->name, (float)load->done / (float)load->total, load->user);
    }
    if (load->done == load->total) {
        br_assets_state *a = e->assets;
        for (uint32_t i = 0; i < a->bundle_loads.count; i++) {
            if (*(br_bundle_load **)br_array_at(&a->bundle_loads, i) == load) {
                br_array_remove_swap(&a->bundle_loads, i);
                break;
            }
        }
        br_free(load->name);
        br_free(load);
    }
}

bool br_assets_load_bundle(br_engine *e, const char *name, br_asset_progress_fn fn, void *user)
{
    br_bundle *bundle = name != NULL ? br_bundle_find(e->assets, name) : NULL;
    if (bundle == NULL) {
        br_log_error("br_assets_load_bundle: unknown bundle '%s'", name != NULL ? name : "");
        return false;
    }
    if (bundle->assets.count == 0) {
        if (fn != NULL) {
            fn(e, name, 1.0f, user);
        }
        return true;
    }
    br_bundle_load *load = br_alloc(sizeof(br_bundle_load));
    if (load == NULL) {
        return false;
    }
    load->name = br_strdup(name);
    load->total = bundle->assets.count;
    load->fn = fn;
    load->user = user;
    br_array_push(&e->assets->bundle_loads, &load);
    uint32_t count = bundle->assets.count;
    for (uint32_t i = 0; i < count; i++) {
        br_bundle_asset *asset = br_array_at(&bundle->assets, i);
        br_asset_options options;
        SDL_zero(options);
        options.type = asset->type;
        br_assets_load_async_resolved(e, asset->path, &options, br_bundle_asset_done, load);
    }
    return true;
}

void br_assets_unload_bundle(br_engine *e, const char *name)
{
    br_bundle *bundle = name != NULL ? br_bundle_find(e->assets, name) : NULL;
    if (bundle == NULL) {
        return;
    }
    for (uint32_t i = 0; i < bundle->assets.count; i++) {
        br_bundle_asset *asset = br_array_at(&bundle->assets, i);
        br_assets_unload_resolved(e, asset->path);
    }
}

uint32_t br_assets_bundle_size(br_engine *e, const char *name)
{
    br_bundle *bundle = name != NULL ? br_bundle_find(e->assets, name) : NULL;
    return bundle != NULL ? bundle->assets.count : 0;
}

const char *br_assets_bundle_path(br_engine *e, const char *name, uint32_t index)
{
    br_bundle *bundle = name != NULL ? br_bundle_find(e->assets, name) : NULL;
    br_bundle_asset *asset = bundle != NULL ? br_array_at(&bundle->assets, index) : NULL;
    return asset != NULL ? asset->path : NULL;
}

void br_assets_bundles_free(br_assets_state *a)
{
    for (uint32_t i = 0; i < a->bundles.count; i++) {
        br_bundle *b = br_array_at(&a->bundles, i);
        for (uint32_t j = 0; j < b->assets.count; j++) {
            br_bundle_asset *asset = br_array_at(&b->assets, j);
            br_free(asset->path);
            br_free(asset->type);
        }
        br_array_free(&b->assets);
        br_free(b->name);
    }
    br_array_free(&a->bundles);
    for (uint32_t i = 0; i < a->bundle_loads.count; i++) {
        br_bundle_load *load = *(br_bundle_load **)br_array_at(&a->bundle_loads, i);
        br_free(load->name);
        br_free(load);
    }
    br_array_free(&a->bundle_loads);
    br_free(a->base_path);
    br_free(a->pref_formats);
}
