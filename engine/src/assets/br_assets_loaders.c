/* br_assets_loaders: the json, text and binary loaders.
 *
 * JSON parses on the main thread (load, no decode split): cJSON keeps its parse-error
 * position in a global, so parsing on several threads at once would race. The worker still
 * reads the file. */
#include "assets/br_assets_internal.h"

#include "cjson/cJSON.h"

static void *br_json_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    (void)e;
    cJSON *json = cJSON_ParseWithLength(bytes, size);
    if (json == NULL) {
        const char *where = cJSON_GetErrorPtr();
        br_log_error("json '%s': parse error at byte %d", path,
                     where != NULL ? (int)(where - (const char *)bytes) : -1);
    }
    return json;
}

static void br_json_asset_unload(br_engine *e, void *asset)
{
    (void)e;
    cJSON_Delete(asset);
}

static void *br_text_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    (void)e;
    (void)path;
    char *text = br_alloc(size + 1); /* zeroed: the copy is NUL-terminated */
    if (text != NULL) {
        SDL_memcpy(text, bytes, size);
    }
    return text;
}

static void br_free_asset(br_engine *e, void *asset)
{
    (void)e;
    br_free(asset);
}

static void *br_binary_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    (void)e;
    (void)path;
    br_asset_blob *blob = br_alloc(sizeof(br_asset_blob) + size + 1); /* one block: header + bytes */
    if (blob == NULL) {
        return NULL;
    }
    blob->size = size;
    blob->data = (uint8_t *)(blob + 1);
    SDL_memcpy(blob->data, bytes, size);
    return blob;
}

void br_assets_register_basic_loaders(br_engine *e)
{
    br_asset_loader_desc desc;
    SDL_zero(desc);
    desc.type_name = "json";
    desc.extensions = "json";
    desc.load = br_json_asset_load;
    desc.unload = br_json_asset_unload;
    br_assets_register_loader(e, &desc);

    SDL_zero(desc);
    desc.type_name = "text";
    desc.extensions = "txt,csv";
    desc.load = br_text_asset_load;
    desc.unload = br_free_asset;
    br_assets_register_loader(e, &desc);

    SDL_zero(desc);
    desc.type_name = "binary";
    desc.extensions = "bin";
    desc.load = br_binary_asset_load;
    desc.unload = br_free_asset;
    br_assets_register_loader(e, &desc);
}
