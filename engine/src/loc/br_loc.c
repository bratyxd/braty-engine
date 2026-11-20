/* br_loc: string tables (the "strings" asset loader), the locale chain, placeholders, plural
 * forms and text nodes bound to keys. */
#include "app/br_engine_internal.h"
#include "braty/braty.h"
#include "braty/br_loc.h"

#include <SDL3/SDL_locale.h>

#define BR_LOC_DIR_SIZE 256
#define BR_LOC_PATH_SIZE 512
#define BR_LOC_MAX_CHAIN (BR_LOC_MAX_FALLBACKS + 3) /* locale, its language, fallbacks, default */

/* A loaded strings.<locale>.json: the document and its entries by key. */
typedef struct br_loc_table {
    cJSON *doc;
    br_map entries;   /* key -> const cJSON * (a string, or an object of plural forms) */
} br_loc_table;

/* The key bound to a text node, by node index. */
typedef struct br_loc_binding {
    uint32_t generation;
    char    *key;     /* NULL = unbound */
} br_loc_binding;

typedef struct br_loc_state {
    char          dir[BR_LOC_DIR_SIZE];
    char          default_locale[BR_LOC_NAME_SIZE];
    char          fallbacks[BR_LOC_MAX_FALLBACKS][BR_LOC_NAME_SIZE];
    uint32_t      fallback_count;
    char          locale[BR_LOC_NAME_SIZE];
    char          paths[BR_LOC_MAX_CHAIN][BR_LOC_PATH_SIZE]; /* the tables of the chain, in lookup order */
    br_loc_table *tables[BR_LOC_MAX_CHAIN];
    uint32_t      table_count;
    br_map        missing;    /* keys already logged as missing in this locale */
    br_array      bindings;   /* br_loc_binding, by node index */
    br_array      format;     /* char: the br_loc_format result */
} br_loc_state;

/* ---- the "strings" asset loader */

// ponytail: tables are not hot-reloaded (an edited file is read again once no locale holds it); watch the chain's files like scene files when translators need live edits.

static bool br_loc_is_table_path(const char *path)
{
    const char *slash = SDL_strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    if (SDL_strncmp(name, "strings.", 8) != 0) {
        return false;
    }
    const char *dot = SDL_strrchr(name + 8, '.'); /* strings.<locale>.json, not strings.json */
    return dot != NULL && dot != name + 8;
}

static bool br_loc_test(const char *path, const void *bytes, size_t size)
{
    (void)bytes;
    (void)size;
    return br_loc_is_table_path(path);
}

/* A plural entry: an object of "zero" / "one" / "other" strings, at least "other" or "one". */
static bool br_loc_plural_ok(const cJSON *v)
{
    bool any = false;
    const cJSON *form = NULL;
    cJSON_ArrayForEach(form, v)
    {
        bool known = SDL_strcmp(form->string, "zero") == 0 || SDL_strcmp(form->string, "one") == 0 ||
                     SDL_strcmp(form->string, "other") == 0;
        if (!known || !cJSON_IsString(form)) {
            return false;
        }
        any = any || SDL_strcmp(form->string, "zero") != 0;
    }
    return any;
}

static void br_loc_table_free(br_loc_table *t)
{
    br_map_free(&t->entries);
    cJSON_Delete(t->doc);
    br_free(t);
}

static void *br_loc_table_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    (void)e;
    cJSON *doc = cJSON_ParseWithLength(bytes, size);
    if (!cJSON_IsObject(doc)) {
        br_log_error("%s: a string table is a JSON object { \"key\": \"text\" }", path);
        cJSON_Delete(doc);
        return NULL;
    }
    br_loc_table *t = br_alloc(sizeof(br_loc_table));
    if (t == NULL) {
        cJSON_Delete(doc);
        return NULL;
    }
    t->doc = doc;
    br_map_init(&t->entries, sizeof(const cJSON *));
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, doc)
    {
        if (cJSON_IsString(item) || (cJSON_IsObject(item) && br_loc_plural_ok(item))) {
            br_map_set(&t->entries, item->string, &item);
        } else {
            br_log_error("%s: %s: expected a string or { \"one\", \"other\", \"zero\" } strings (entry skipped)", path,
                         item->string);
        }
    }
    return t;
}

static void br_loc_table_unload(br_engine *e, void *asset)
{
    (void)e;
    br_loc_table_free(asset);
}

/* ---- the locale chain */

static bool br_loc_name_ok(const char *name)
{
    size_t len = name != NULL ? SDL_strlen(name) : 0;
    if (len == 0 || len >= BR_LOC_NAME_SIZE) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false; /* the name becomes part of a file path */
        }
    }
    return true;
}

static void br_loc_table_path(br_loc_state *s, const char *locale, char *out, size_t out_size)
{
    char file[BR_LOC_NAME_SIZE + 16];
    SDL_snprintf(file, sizeof(file), "strings.%s.json", locale);
    if (s->dir[0] == '\0') {
        br_str_copy(out, out_size, file);
    } else {
        br_path_join(out, out_size, s->dir, file);
    }
}

/* True if locale has a table file (checked without loading, so a missing one is not an error). */
static bool br_loc_table_exists(br_engine *e, const char *locale)
{
    char path[BR_LOC_PATH_SIZE];
    char resolved[BR_LOC_PATH_SIZE];
    br_loc_table_path(e->loc, locale, path, sizeof(path));
    return br_assets_resolve(e, path, resolved, sizeof(resolved)) && br_file_exists(e, resolved);
}

/* "de-AT" -> "de" (false if the locale has no region part). */
static bool br_loc_language(const char *locale, char *out, size_t out_size)
{
    size_t len = 0;
    while (locale[len] != '\0' && locale[len] != '-' && locale[len] != '_') {
        len++;
    }
    if (locale[len] == '\0' || len == 0 || len >= out_size) {
        return false;
    }
    SDL_memcpy(out, locale, len);
    out[len] = '\0';
    return true;
}

/* Drops the references the current chain holds. */
static void br_loc_release_tables(br_engine *e)
{
    br_loc_state *s = e->loc;
    for (uint32_t i = 0; i < s->table_count; i++) {
        br_assets_unload(e, s->paths[i]);
    }
    s->table_count = 0;
}

/* Re-sets every bound text node (stale handles are skipped; their bindings die with the node). */
static void br_loc_refresh_bindings(br_engine *e)
{
    br_loc_state *s = e->loc;
    for (uint32_t i = 0; i < s->bindings.count; i++) {
        br_loc_binding *b = br_array_at(&s->bindings, i);
        br_node n = { i, b->generation };
        if (b->key != NULL && br_node_valid(e, n)) {
            br_text_set_text(e, n, br_loc_get(e, b->key));
        }
    }
}

bool br_loc_set_locale(br_engine *e, const char *locale)
{
    br_loc_state *s = e->loc;
    if (s == NULL) {
        return false;
    }
    if (!br_loc_name_ok(locale)) {
        br_log_error("br_loc_set_locale: '%s' is not a locale name (letters, digits, '-', '_')",
                     locale != NULL ? locale : "(null)");
        return false;
    }
    char language[BR_LOC_NAME_SIZE] = "";
    bool has_language = br_loc_language(locale, language, sizeof(language));
    /* candidates in lookup order, each once; the first `own` are the locale's own tables */
    const char *names[BR_LOC_MAX_CHAIN];
    uint32_t name_count = 0;
    names[name_count++] = locale;
    if (has_language) {
        names[name_count++] = language;
    }
    uint32_t own = name_count;
    bool own_loaded = false;
    for (uint32_t i = 0; i < s->fallback_count; i++) {
        names[name_count++] = s->fallbacks[i];
    }
    if (s->default_locale[0] != '\0') {
        names[name_count++] = s->default_locale;
    }
    char paths[BR_LOC_MAX_CHAIN][BR_LOC_PATH_SIZE];
    br_loc_table *tables[BR_LOC_MAX_CHAIN];
    uint32_t count = 0;
    for (uint32_t i = 0; i < name_count; i++) {
        bool seen = false;
        for (uint32_t k = 0; k < i; k++) {
            seen = seen || SDL_strcmp(names[k], names[i]) == 0;
        }
        if (seen || !br_loc_table_exists(e, names[i])) {
            continue;
        }
        br_loc_table_path(s, names[i], paths[count], sizeof(paths[count]));
        /* the new chain first: shared tables stay loaded. Forced to the strings loader (a broken
         * table must not come back as another loader's asset), and checked: a path already cached
         * by another loader keeps its type */
        br_asset_options options;
        SDL_zero(options);
        options.type = "strings";
        tables[count] = br_assets_load_with(e, paths[count], &options);
        const char *type = tables[count] != NULL ? br_assets_type(e, paths[count]) : NULL;
        if (tables[count] != NULL && (type == NULL || SDL_strcmp(type, "strings") != 0)) {
            br_log_error("br_loc_set_locale: '%s' is already loaded as '%s', not as a string table", paths[count],
                         type != NULL ? type : "?");
            br_assets_unload(e, paths[count]);
            tables[count] = NULL;
        }
        if (tables[count] != NULL) {
            own_loaded = own_loaded || i < own;
            count++;
        }
    }
    if (!own_loaded) {
        for (uint32_t i = 0; i < count; i++) {
            br_assets_unload(e, paths[i]);
        }
        br_log_error("br_loc_set_locale: no string table loads for '%s'%s%s (locale unchanged)", locale,
                     has_language ? " or " : "", language);
        return false;
    }
    br_loc_release_tables(e);
    SDL_memcpy(s->paths, paths, sizeof(paths));
    SDL_memcpy(s->tables, tables, sizeof(tables));
    s->table_count = count;
    br_str_copy(s->locale, sizeof(s->locale), locale);
    br_map_clear(&s->missing);
    br_loc_refresh_bindings(e);
    return true;
}

/* The first preferred system locale (region form, then language) with a table, false if none. */
static bool br_loc_system_choice(br_engine *e, char *out, size_t out_size)
{
    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    bool found = false;
    for (int i = 0; locales != NULL && i < count && !found; i++) {
        const SDL_Locale *l = locales[i];
        char name[BR_LOC_NAME_SIZE];
        if (l->country != NULL) {
            SDL_snprintf(name, sizeof(name), "%s-%s", l->language, l->country);
            found = br_loc_name_ok(name) && br_loc_table_exists(e, name);
        }
        if (!found) {
            br_str_copy(name, sizeof(name), l->language);
            found = br_loc_name_ok(name) && br_loc_table_exists(e, name);
        }
        if (found) {
            br_str_copy(out, out_size, name);
        }
    }
    SDL_free(locales);
    return found;
}

bool br_loc_setup(br_engine *e, const char *dir, const char *default_locale, const char *const *fallbacks,
                  uint32_t fallback_count)
{
    br_loc_state *s = e->loc;
    if (s == NULL) {
        return false;
    }
    if (!br_loc_name_ok(default_locale) || fallback_count > BR_LOC_MAX_FALLBACKS ||
        !br_str_copy(s->dir, sizeof(s->dir), dir != NULL ? dir : "")) {
        br_log_error("br_loc_setup: needs a default locale name, at most %d fallbacks and a directory below %d bytes",
                     BR_LOC_MAX_FALLBACKS, BR_LOC_DIR_SIZE);
        return false;
    }
    s->fallback_count = 0;
    for (uint32_t i = 0; i < fallback_count; i++) {
        if (!br_loc_name_ok(fallbacks[i])) {
            br_log_error("br_loc_setup: fallback %u '%s' is not a locale name", i, fallbacks[i] != NULL ? fallbacks[i] : "");
            continue;
        }
        br_str_copy(s->fallbacks[s->fallback_count++], BR_LOC_NAME_SIZE, fallbacks[i]);
    }
    br_str_copy(s->default_locale, sizeof(s->default_locale), default_locale);
    if (!br_loc_table_exists(e, default_locale)) {
        char path[BR_LOC_PATH_SIZE];
        br_loc_table_path(s, default_locale, path, sizeof(path));
        br_log_error("br_loc_setup: the default locale '%s' has no table (%s)", default_locale, path);
        return false;
    }
    /* tests and recorded runs must not depend on the machine's language */
    bool reproducible = br_app_deterministic(e) || br_engine_window(e) == NULL || e->frame_limit > 0;
    char chosen[BR_LOC_NAME_SIZE];
    if (reproducible || !br_loc_system_choice(e, chosen, sizeof(chosen))) {
        br_str_copy(chosen, sizeof(chosen), default_locale);
    }
    return br_loc_set_locale(e, chosen);
}

const char *br_loc_locale(br_engine *e)
{
    return e->loc != NULL ? e->loc->locale : "";
}

/* The entry for key in the chain (a string or a plural object), NULL if none. */
static const cJSON *br_loc_find(br_engine *e, const char *key)
{
    br_loc_state *s = e->loc;
    for (uint32_t i = 0; s != NULL && key != NULL && i < s->table_count; i++) {
        const cJSON **v = br_map_get(&s->tables[i]->entries, key);
        if (v != NULL) {
            return *v;
        }
    }
    return NULL;
}

/* The form of an entry for count (have_count false: the "other" form). */
static const char *br_loc_form(const cJSON *v, bool have_count, long long count)
{
    if (cJSON_IsString(v)) {
        return v->valuestring;
    }
    const cJSON *zero = cJSON_GetObjectItemCaseSensitive(v, "zero");
    const cJSON *one = cJSON_GetObjectItemCaseSensitive(v, "one");
    const cJSON *other = cJSON_GetObjectItemCaseSensitive(v, "other");
    if (have_count && count == 0 && zero != NULL) {
        return zero->valuestring;
    }
    if (have_count && (count == 1 || count == -1) && one != NULL) {
        return one->valuestring;
    }
    return other != NULL ? other->valuestring : one->valuestring;
}

static const char *br_loc_missing(br_engine *e, const char *key)
{
    br_loc_state *s = e->loc;
    if (key == NULL) {
        return "";
    }
    if (s != NULL && br_map_get(&s->missing, key) == NULL) {
        bool yes = true;
        br_map_set(&s->missing, key, &yes);
        br_log_warn("loc: no text for '%s' in locale '%s' (the key is shown)", key, s->locale);
    }
    return key;
}

const char *br_loc_get(br_engine *e, const char *key)
{
    const cJSON *v = br_loc_find(e, key);
    return v != NULL ? br_loc_form(v, false, 0) : br_loc_missing(e, key);
}

bool br_loc_has(br_engine *e, const char *key)
{
    return br_loc_find(e, key) != NULL;
}

static void br_loc_append(br_array *out, const char *text, size_t len)
{
    if (len > 0 && len <= UINT32_MAX - out->count) {
        br_array_push_n(out, text, (uint32_t)len);
    }
}

const char *br_loc_format(br_engine *e, const char *key, const br_loc_arg *args, uint32_t arg_count)
{
    br_loc_state *s = e->loc;
    if (s == NULL) {
        return key != NULL ? key : "";
    }
    bool have_count = false;
    long long count = 0;
    for (uint32_t i = 0; args != NULL && i < arg_count; i++) {
        if (args[i].name != NULL && args[i].value != NULL && SDL_strcmp(args[i].name, "count") == 0) {
            char *end = NULL;
            count = SDL_strtoll(args[i].value, &end, 10);
            have_count = end != args[i].value;
        }
    }
    const cJSON *v = br_loc_find(e, key);
    const char *text = v != NULL ? br_loc_form(v, have_count, count) : br_loc_missing(e, key);
    br_array_clear(&s->format);
    const char *p = text;
    while (*p != '\0') {
        const char *open = SDL_strchr(p, '{');
        const char *close = open != NULL ? SDL_strchr(open, '}') : NULL;
        if (open == NULL || close == NULL) {
            br_loc_append(&s->format, p, SDL_strlen(p));
            break;
        }
        br_loc_append(&s->format, p, (size_t)(open - p));
        size_t name_len = (size_t)(close - open - 1);
        const char *value = NULL;
        for (uint32_t i = 0; args != NULL && i < arg_count && value == NULL; i++) {
            if (args[i].name != NULL && SDL_strlen(args[i].name) == name_len &&
                SDL_strncmp(args[i].name, open + 1, name_len) == 0) {
                value = args[i].value != NULL ? args[i].value : "";
            }
        }
        if (value != NULL) {
            br_loc_append(&s->format, value, SDL_strlen(value));
        } else {
            br_loc_append(&s->format, open, (size_t)(close - open + 1)); /* unknown: kept as written */
        }
        p = close + 1;
    }
    char nul = '\0';
    br_array_push(&s->format, &nul);
    return s->format.data != NULL ? (const char *)s->format.data : "";
}

/* ---- text nodes bound to keys */

void br_loc_bind_text(br_engine *e, br_node n, const char *key)
{
    br_loc_state *s = e->loc;
    if (s == NULL || !br_node_valid(e, n)) {
        return;
    }
    if (n.index >= s->bindings.count && !br_array_resize(&s->bindings, n.index + 1)) {
        return;
    }
    br_loc_binding *b = br_array_at(&s->bindings, n.index);
    br_free(b->key);
    b->key = NULL;
    if (key == NULL || key[0] == '\0') {
        return;
    }
    b->key = br_strdup(key);
    b->generation = n.generation;
    br_text_set_text(e, n, br_loc_get(e, key));
}

const char *br_loc_text_key(br_engine *e, br_node n)
{
    br_loc_state *s = e->loc;
    br_loc_binding *b = s != NULL ? br_array_at(&s->bindings, n.index) : NULL;
    return b != NULL && b->key != NULL && b->generation == n.generation ? b->key : "";
}

static void br_loc_node_destroyed(br_engine *e, br_node n, void *user)
{
    (void)user;
    br_loc_binding *b = e->loc != NULL ? br_array_at(&e->loc->bindings, n.index) : NULL;
    if (b != NULL && b->generation == n.generation) {
        br_free(b->key);
        b->key = NULL;
    }
}

/* ---- module */

static void br_loc_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_loc_state *s = e->loc;
    br_loc_release_tables(e);
    for (uint32_t i = 0; i < s->bindings.count; i++) {
        br_free(((br_loc_binding *)br_array_at(&s->bindings, i))->key);
    }
    br_array_free(&s->bindings);
    br_array_free(&s->format);
    br_map_free(&s->missing);
    br_free(s);
    e->loc = NULL;
}

void br_loc_register(br_engine *e)
{
    br_loc_state *s = br_alloc(sizeof(br_loc_state));
    if (s == NULL) {
        return;
    }
    SDL_zerop(s);
    br_map_init(&s->missing, sizeof(bool));
    br_array_init(&s->bindings, sizeof(br_loc_binding));
    br_array_init(&s->format, 1);
    e->loc = s;
    br_asset_loader_desc loader;
    SDL_zero(loader);
    loader.type_name = "strings";
    loader.extensions = "json";
    loader.load = br_loc_table_load;
    loader.unload = br_loc_table_unload;
    loader.test = br_loc_test;
    br_assets_register_loader(e, &loader);
    br_node_on_destroy(e, br_loc_node_destroyed, NULL);
    br_engine_on_shutdown(e, br_loc_shutdown, NULL);
}
