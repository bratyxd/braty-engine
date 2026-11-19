/* br_spritesheet: the "spritesheet" loader (TexturePacker JSON, hash and array formats). */
#include "assets/br_assets_internal.h"
#include "braty/br_spritesheet.h"

#include "cjson/cJSON.h"

typedef struct br_sheet_frame {
    char      *name;
    br_texture texture;
    bool       has_borders;
    float      borders[4];  /* left, top, right, bottom */
} br_sheet_frame;

typedef struct br_sheet_animation {
    char    *name;
    br_array textures;  /* br_texture */
} br_sheet_animation;

struct br_spritesheet {
    char      *image_path;   /* resolved; the sheet holds one reference */
    br_texture texture;
    br_array   frames;       /* br_sheet_frame, file order */
    br_map     frame_index;  /* name -> uint32_t index into frames */
    br_array   animations;   /* br_sheet_animation */
    br_array   linked;       /* char * paths of related multi-packs (one reference each) */
};

static float br_json_number(const cJSON *object, const char *key, float fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    if (cJSON_IsString(item)) {
        return (float)SDL_atof(item->valuestring); /* TexturePacker writes meta.scale as a string */
    }
    return fallback;
}

/* {x, y, w, h} -> rect. False if the object is missing. */
static bool br_json_rect(const cJSON *object, br_rect *out)
{
    if (!cJSON_IsObject(object)) {
        return false;
    }
    *out = br_rect_make(br_json_number(object, "x", 0.0f), br_json_number(object, "y", 0.0f),
                        br_json_number(object, "w", 0.0f), br_json_number(object, "h", 0.0f));
    return true;
}

static const br_sheet_frame *br_sheet_find_local(const br_spritesheet *sheet, const char *name)
{
    uint32_t *index = br_map_get(&sheet->frame_index, name);
    return index != NULL ? br_array_at(&sheet->frames, *index) : NULL;
}

/* Finds a frame here or in a linked pack. */
static const br_sheet_frame *br_sheet_find(br_engine *e, const br_spritesheet *sheet, const char *name)
{
    const br_sheet_frame *f = br_sheet_find_local(sheet, name);
    for (uint32_t i = 0; f == NULL && i < sheet->linked.count; i++) {
        const char *path = *(char **)br_array_at(&sheet->linked, i);
        const br_spritesheet *other = br_assets_get(e, path);
        if (other != NULL && br_str_equal(br_assets_type(e, path), "spritesheet")) {
            f = br_sheet_find_local(other, name);
        }
    }
    return f;
}

/* One TexturePacker frame entry -> a frame texture. */
static bool br_sheet_add_frame(br_engine *e, br_spritesheet *sheet, const char *name, const cJSON *data)
{
    br_texture_frame tf;
    SDL_zero(tf);
    if (name == NULL || !br_json_rect(cJSON_GetObjectItemCaseSensitive(data, "frame"), &tf.frame)) {
        br_log_error("spritesheet: frame '%s' has no \"frame\" rect", name != NULL ? name : "?");
        return false;
    }
    tf.rotated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "rotated"));
    const cJSON *trimmed = cJSON_GetObjectItemCaseSensitive(data, "trimmed");
    const cJSON *source_size = cJSON_GetObjectItemCaseSensitive(data, "sourceSize");
    br_rect sprite_source;
    if (!cJSON_IsFalse(trimmed) && cJSON_IsObject(source_size) &&
        br_json_rect(cJSON_GetObjectItemCaseSensitive(data, "spriteSourceSize"), &sprite_source)) {
        tf.orig = br_rect_make(0.0f, 0.0f, br_json_number(source_size, "w", tf.frame.width),
                               br_json_number(source_size, "h", tf.frame.height));
        tf.trim = br_rect_make(sprite_source.x, sprite_source.y, tf.frame.width, tf.frame.height);
    }
    const cJSON *anchor = cJSON_GetObjectItemCaseSensitive(data, "anchor");
    if (anchor == NULL) {
        anchor = cJSON_GetObjectItemCaseSensitive(data, "pivot");
    }
    if (cJSON_IsObject(anchor)) {
        tf.default_anchor = br_vec2_make(br_json_number(anchor, "x", 0.0f), br_json_number(anchor, "y", 0.0f));
    }
    br_sheet_frame frame;
    SDL_zero(frame);
    const cJSON *borders = cJSON_GetObjectItemCaseSensitive(data, "borders");
    if (cJSON_IsObject(borders)) {
        frame.has_borders = true;
        frame.borders[0] = br_json_number(borders, "left", 0.0f);
        frame.borders[1] = br_json_number(borders, "top", 0.0f);
        frame.borders[2] = br_json_number(borders, "right", 0.0f);
        frame.borders[3] = br_json_number(borders, "bottom", 0.0f);
    }
    frame.texture = br_texture_create_frame(e, sheet->texture, &tf);
    if (frame.texture.generation == 0) {
        return false;
    }
    frame.name = br_strdup(name);
    uint32_t index = sheet->frames.count;
    br_array_push(&sheet->frames, &frame);
    br_map_set(&sheet->frame_index, name, &index);
    return true;
}

static void br_sheet_add_animations(br_engine *e, br_spritesheet *sheet, const cJSON *animations)
{
    if (!cJSON_IsObject(animations)) {
        return;
    }
    for (const cJSON *anim = animations->child; anim != NULL; anim = anim->next) {
        br_sheet_animation entry;
        entry.name = br_strdup(anim->string);
        br_array_init(&entry.textures, sizeof(br_texture));
        for (const cJSON *f = anim->child; f != NULL; f = f->next) {
            const br_sheet_frame *frame = cJSON_IsString(f) ? br_sheet_find(e, sheet, f->valuestring) : NULL;
            if (frame == NULL) {
                br_log_warn("spritesheet: animation '%s' names a missing frame", anim->string);
                continue;
            }
            br_array_push(&entry.textures, &frame->texture);
        }
        br_array_push(&sheet->animations, &entry);
    }
}

static void br_spritesheet_unload(br_engine *e, void *asset)
{
    br_spritesheet *sheet = asset;
    for (uint32_t i = 0; i < sheet->frames.count; i++) {
        br_sheet_frame *f = br_array_at(&sheet->frames, i);
        br_texture_destroy(e, f->texture);
        br_free(f->name);
    }
    for (uint32_t i = 0; i < sheet->animations.count; i++) {
        br_sheet_animation *anim = br_array_at(&sheet->animations, i);
        br_free(anim->name);
        br_array_free(&anim->textures);
    }
    for (uint32_t i = 0; i < sheet->linked.count; i++) {
        char *path = *(char **)br_array_at(&sheet->linked, i);
        br_assets_unload_resolved(e, path);
        br_free(path);
    }
    if (sheet->image_path != NULL) {
        br_assets_unload_resolved(e, sheet->image_path);
        br_free(sheet->image_path);
    }
    br_array_free(&sheet->frames);
    br_map_free(&sheet->frame_index);
    br_array_free(&sheet->animations);
    br_array_free(&sheet->linked);
    br_free(sheet);
}

/* `name` relative to the directory of `base` (both asset paths). */
static bool br_sheet_sibling(const char *base, const char *name, char *out, size_t out_size)
{
    const char *slash = SDL_strrchr(base, '/');
    if (slash == NULL || br_path_is_absolute(name)) {
        return br_str_copy(out, out_size, name);
    }
    char dir[1024];
    size_t dir_len = (size_t)(slash - base);
    if (dir_len >= sizeof(dir)) {
        return false;
    }
    SDL_memcpy(dir, base, dir_len);
    dir[dir_len] = 0;
    return br_path_join(out, out_size, dir, name);
}

static void *br_spritesheet_load_asset(br_engine *e, const char *path, const void *bytes, size_t size)
{
    br_assets_state *a = e->assets;
    cJSON *root = cJSON_ParseWithLength(bytes, size);
    if (root == NULL) {
        const char *where = cJSON_GetErrorPtr();
        br_log_error("spritesheet '%s': bad JSON at byte %d", path,
                     where != NULL ? (int)(where - (const char *)bytes) : -1);
        return NULL;
    }
    const cJSON *frames = cJSON_GetObjectItemCaseSensitive(root, "frames");
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, "meta");
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(meta, "image");
    char image_path[1024];
    if ((!cJSON_IsObject(frames) && !cJSON_IsArray(frames)) || !cJSON_IsString(image) ||
        !br_sheet_sibling(path, image->valuestring, image_path, sizeof(image_path))) {
        br_log_error("spritesheet '%s': needs \"frames\" and \"meta.image\"", path);
        cJSON_Delete(root);
        return NULL;
    }
    br_spritesheet *sheet = br_alloc(sizeof(br_spritesheet));
    if (sheet == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    br_array_init(&sheet->frames, sizeof(br_sheet_frame));
    br_map_init(&sheet->frame_index, sizeof(uint32_t));
    br_array_init(&sheet->animations, sizeof(br_sheet_animation));
    br_array_init(&sheet->linked, sizeof(char *));

    // ponytail: the atlas image loads synchronously here (on the main thread, also for async loads);
    // dependency-aware jobs would queue it on a worker and finish the sheet when it arrives.
    br_texture *atlas = br_assets_load_resolved(e, image_path, NULL);
    if (atlas == NULL || !br_str_equal(br_assets_type(e, image_path), "texture")) {
        br_log_error("spritesheet '%s': image '%s' is not a loadable texture", path, image_path);
        if (atlas != NULL) {
            br_assets_unload_resolved(e, image_path);
        }
        br_spritesheet_unload(e, sheet);
        cJSON_Delete(root);
        return NULL;
    }
    sheet->image_path = br_strdup(image_path);
    sheet->texture = *atlas;
    float resolution = br_assets_resolution_of(path);
    if (resolution == 1.0f) {
        resolution = br_json_number(meta, "scale", 1.0f);
    }
    if (resolution > 0.0f && resolution != 1.0f) {
        br_texture_set_resolution(e, sheet->texture, resolution); /* before frames: they copy it */
    }

    bool ok = true;
    for (const cJSON *f = frames->child; f != NULL; f = f->next) {
        const char *name = f->string; /* hash format */
        if (cJSON_IsArray(frames)) {
            const cJSON *filename = cJSON_GetObjectItemCaseSensitive(f, "filename");
            name = cJSON_IsString(filename) ? filename->valuestring : NULL;
        }
        ok = br_sheet_add_frame(e, sheet, name, f) && ok;
    }

    const cJSON *related = cJSON_GetObjectItemCaseSensitive(meta, "related_multi_packs");
    if (cJSON_IsArray(related) && a->sheet_nesting == 0) {
        a->sheet_nesting++;
        for (const cJSON *r = related->child; r != NULL; r = r->next) {
            char linked_path[1024];
            if (!cJSON_IsString(r) || !br_sheet_sibling(path, r->valuestring, linked_path, sizeof(linked_path))) {
                continue;
            }
            br_asset_options options;
            SDL_zero(options);
            options.type = "spritesheet";
            if (br_assets_load_resolved(e, linked_path, &options) == NULL) {
                br_log_error("spritesheet '%s': linked pack '%s' failed", path, linked_path);
                ok = false;
                continue;
            }
            char *copy = br_strdup(linked_path);
            br_array_push(&sheet->linked, &copy);
        }
        a->sheet_nesting--;
    }
    br_sheet_add_animations(e, sheet, cJSON_GetObjectItemCaseSensitive(root, "animations"));
    cJSON_Delete(root);
    if (!ok) {
        br_log_error("spritesheet '%s': some frames could not be read", path);
    }
    return sheet;
}

/* Claims .json files that look like TexturePacker output. */
static bool br_spritesheet_test(const char *path, const void *bytes, size_t size)
{
    (void)path;
    (void)size;
    return SDL_strstr(bytes, "\"frames\"") != NULL && SDL_strstr(bytes, "\"meta\"") != NULL;
}

void br_spritesheet_register_loader(br_engine *e)
{
    br_asset_loader_desc desc;
    SDL_zero(desc);
    desc.type_name = "spritesheet";
    desc.extensions = "json";
    desc.load = br_spritesheet_load_asset;
    desc.unload = br_spritesheet_unload;
    desc.test = br_spritesheet_test;
    br_assets_register_loader(e, &desc);
}

/* ---- public API */

br_spritesheet *br_spritesheet_load(br_engine *e, const char *path)
{
    br_asset_options options;
    SDL_zero(options);
    options.type = "spritesheet";
    br_spritesheet *sheet = br_assets_load_with(e, path, &options);
    if (sheet != NULL && !br_str_equal(br_assets_type(e, path), "spritesheet")) {
        br_log_error("br_spritesheet_load: '%s' is cached as a %s", path, br_assets_type(e, path));
        br_assets_unload(e, path);
        return NULL;
    }
    return sheet;
}

br_texture br_spritesheet_frame(br_engine *e, const br_spritesheet *sheet, const char *name)
{
    br_texture none = { 0, 0 };
    const br_sheet_frame *f = sheet != NULL && name != NULL ? br_sheet_find(e, sheet, name) : NULL;
    return f != NULL ? f->texture : none;
}

uint32_t br_spritesheet_frame_count(const br_spritesheet *sheet)
{
    return sheet != NULL ? sheet->frames.count : 0;
}

const char *br_spritesheet_frame_name(const br_spritesheet *sheet, uint32_t index)
{
    const br_sheet_frame *f = sheet != NULL ? br_array_at(&sheet->frames, index) : NULL;
    return f != NULL ? f->name : NULL;
}

bool br_spritesheet_frame_borders(br_engine *e, const br_spritesheet *sheet, const char *name, float out[4])
{
    const br_sheet_frame *f = sheet != NULL && name != NULL ? br_sheet_find(e, sheet, name) : NULL;
    if (f == NULL || !f->has_borders) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = f->borders[i];
    }
    return true;
}

uint32_t br_spritesheet_animation(br_engine *e, const br_spritesheet *sheet, const char *name,
                                  const br_texture **frames)
{
    (void)e;
    *frames = NULL;
    if (sheet == NULL || name == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < sheet->animations.count; i++) {
        const br_sheet_animation *anim = br_array_at(&sheet->animations, i);
        if (br_str_equal(anim->name, name)) {
            *frames = anim->textures.data;
            return anim->textures.count;
        }
    }
    return 0;
}

br_texture br_spritesheet_texture(const br_spritesheet *sheet)
{
    br_texture none = { 0, 0 };
    return sheet != NULL ? sheet->texture : none;
}
