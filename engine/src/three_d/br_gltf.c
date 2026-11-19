/* br_gltf: the .gltf/.glb loader (cgltf) -> br_model, and module registration.
 *
 * decode (worker thread when async): parse, resolve buffers and images, decode images with
 * stb_image, build vertex/index arrays, convert nodes, skins, cameras, lights and animations.
 * finish (main thread): GPU textures (mipmapped), materials, geometries. Instances and the
 * model3d node type live in br_gltf_instance.c, the animator in br_anim3d.c.
 */
#include "three_d/br_gltf_internal.h"

#include "cgltf/cgltf.h"
#include "stb/stb_image.h"

#define BR_GLTF_EXT_NAME 64
#define BR_GLTF_MAX_UNKNOWN 16

/* Decoded image: straight RGBA8 from stb_image (NULL if it failed). */
typedef struct br_gltf_image {
    stbi_uc *pixels;
    int      width;
    int      height;
} br_gltf_image;

typedef struct br_gltf_prim_data {
    br_vertex3d *vertices;
    uint32_t     vertex_count;
    uint32_t    *indices;
    uint32_t     index_count;
    br_vec3     *morph_positions;
    br_vec3     *morph_normals;
    uint32_t     morph_count;
} br_gltf_prim_data;

typedef struct br_gltf_material_data {
    br_material3d_desc desc;
    int32_t            image[BR_MATERIAL3D_SLOT_COUNT];   /* -1 = no texture */
} br_gltf_material_data;

/* What decode hands to finish. */
typedef struct br_gltf_decoded {
    br_model              *model;
    br_gltf_image         *images;
    uint32_t               image_count;
    br_gltf_prim_data     *prims;
    br_gltf_material_data *mats;
    char                   unknown[BR_GLTF_MAX_UNKNOWN][BR_GLTF_EXT_NAME];
    uint32_t               unknown_count;
} br_gltf_decoded;

/* ---- cgltf callbacks: engine allocator and file access (asset root) */

static void *br_gltf_alloc(void *user, cgltf_size size)
{
    (void)user;
    return br_alloc(size > 0 ? size : 1);
}

static void br_gltf_free_cb(void *user, void *ptr)
{
    (void)user;
    br_free(ptr);
}

static cgltf_result br_gltf_read_file(const cgltf_memory_options *memory, const cgltf_file_options *file,
                                      const char *path, cgltf_size *size, void **data)
{
    (void)memory;
    void *bytes = NULL;
    size_t n = 0;
    /* ponytail: br_file_read_all only reads the engine's asset root, which is what the assets
     * workers do too; a per-load file context would make that explicit. */
    if (!br_file_read_all((br_engine *)file->user_data, path, &bytes, &n)) {
        return cgltf_result_file_not_found;
    }
    if (*size != 0 && n < *size) {
        br_free(bytes);
        return cgltf_result_data_too_short;
    }
    *size = n;
    *data = bytes;
    return cgltf_result_success;
}

static void br_gltf_release_file(const cgltf_memory_options *memory, const cgltf_file_options *file, void *data,
                                 cgltf_size size)
{
    (void)memory;
    (void)file;
    (void)size;
    br_free(data);
}

static const char *br_gltf_result_text(cgltf_result r)
{
    switch (r) {
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "not a glTF file";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "a referenced file was not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
    default: return "error";
    }
}

/* ---- small helpers */

static char *br_gltf_strdup(const char *s)
{
    return br_strdup(s != NULL ? s : "");
}

static br_mat4 br_gltf_mat4(const float *m)
{
    br_mat4 r;
    SDL_memcpy(r.m, m, sizeof(r.m));
    return r;
}

/* Joins the directory of `base` with `uri` (decoded %XX) into a new string. */
static char *br_gltf_join(const char *base, const char *uri)
{
    size_t dir = 0;
    for (size_t i = 0; base[i] != '\0'; i++) {
        if (base[i] == '/' || base[i] == '\\') {
            dir = i + 1;
        }
    }
    size_t len = SDL_strlen(uri);
    char *out = br_alloc(dir + len + 1);
    if (out == NULL) {
        return NULL;
    }
    SDL_memcpy(out, base, dir);
    SDL_memcpy(out + dir, uri, len + 1);
    cgltf_decode_uri(out + dir);
    return out;
}

/* Reads one accessor as floats (count * components), NULL on failure. */
static float *br_gltf_floats(const cgltf_accessor *acc, size_t *out_count)
{
    size_t n = acc->count * cgltf_num_components(acc->type);
    float *f = br_alloc(sizeof(float) * (n > 0 ? n : 1));
    if (f == NULL || cgltf_accessor_unpack_floats(acc, f, n) != n) {
        br_free(f);
        return NULL;
    }
    *out_count = n;
    return f;
}

/* ---- extensions */

static bool br_gltf_in_list(const char *name, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (SDL_strcmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool br_gltf_check_extensions(const char *path, const cgltf_data *data, br_gltf_decoded *d)
{
    static const char *const compressed[] = { "KHR_draco_mesh_compression", "EXT_meshopt_compression",
                                              "KHR_meshopt_compression", "KHR_texture_basisu" };
    static const char *const supported[] = { "KHR_lights_punctual", "KHR_materials_emissive_strength",
                                             "KHR_materials_unlit", "KHR_texture_transform",
                                             "KHR_mesh_quantization", "KHR_materials_transmission",
                                             "EXT_mesh_gpu_instancing" };
    for (size_t i = 0; i < data->extensions_used_count; i++) {
        const char *name = data->extensions_used[i];
        if (br_gltf_in_list(name, compressed, BR_ARRAY_COUNT(compressed))) {
            // ponytail: compressed glTF needs C decoders (Draco, meshoptimizer, Basis) that are not vendored; deferred by design.
            br_log_error("glTF '%s': uses %s, compressed glTF is not supported (re-export without compression)",
                         path, name);
            return false;
        }
        if (!br_gltf_in_list(name, supported, BR_ARRAY_COUNT(supported)) && d->unknown_count < BR_GLTF_MAX_UNKNOWN) {
            br_str_copy(d->unknown[d->unknown_count], BR_GLTF_EXT_NAME, name);
            d->unknown_count++;
        }
    }
    for (size_t i = 0; i < data->extensions_required_count; i++) {
        const char *name = data->extensions_required[i];
        if (!br_gltf_in_list(name, supported, BR_ARRAY_COUNT(supported))) {
            br_log_error("glTF '%s': requires the unsupported extension %s", path, name);
            return false;
        }
    }
    return true;
}

/* ---- images */

static void br_gltf_decode_image(br_engine *e, const char *path, const cgltf_options *options,
                                 const cgltf_image *img, br_gltf_image *out)
{
    const uint8_t *bytes = NULL;
    size_t size = 0;
    void *owned = NULL;
    bool owned_by_cgltf = false;
    if (img->buffer_view != NULL) {
        bytes = cgltf_buffer_view_data(img->buffer_view);
        size = img->buffer_view->size;
    } else if (img->uri != NULL && SDL_strncmp(img->uri, "data:", 5) == 0) {
        const char *comma = SDL_strchr(img->uri, ',');
        if (comma != NULL && comma - img->uri >= 7 && SDL_strncmp(comma - 7, ";base64", 7) == 0) {
            const char *b64 = comma + 1;
            size_t len = SDL_strlen(b64);
            size_t pad = 0;
            while (pad < 2 && len > pad && b64[len - 1 - pad] == '=') {
                pad++;
            }
            size = len / 4 * 3 - pad;
            if (cgltf_load_buffer_base64(options, size, b64, &owned) == cgltf_result_success) {
                bytes = owned;
                owned_by_cgltf = true;
            }
        }
    } else if (img->uri != NULL) {
        char *file = br_gltf_join(path, img->uri);
        if (file != NULL && br_file_read_all(e, file, &owned, &size)) {
            bytes = owned;
        }
        br_free(file);
    }
    if (bytes != NULL) {
        int comp = 0;
        out->pixels = stbi_load_from_memory(bytes, (int)size, &out->width, &out->height, &comp, 4);
    }
    if (out->pixels == NULL) {
        br_log_warn("glTF '%s': image '%s' could not be decoded (%s)", path,
                    img->name != NULL ? img->name : (img->uri != NULL && SDL_strncmp(img->uri, "data:", 5) != 0 ? img->uri : "embedded"),
                    bytes == NULL ? "not found" : stbi_failure_reason());
    }
    if (owned_by_cgltf) {
        options->memory.free_func(options->memory.user_data, owned);
    } else {
        br_free(owned);
    }
}

/* ---- materials */

static void br_gltf_texture_slot(const cgltf_data *data, const cgltf_texture_view *tv, br_gltf_material_data *md,
                                 br_material3d_slot slot)
{
    if (tv->texture == NULL || tv->texture->image == NULL) {
        return;
    }
    br_material3d_texture *t = &md->desc.textures[slot];
    md->image[slot] = (int32_t)cgltf_image_index(data, tv->texture->image);
    t->uv_set = tv->texcoord > 0 ? 1u : 0u;
    t->has_sampler = true;
    t->sampler.filter = BR_FILTER_LINEAR;
    t->sampler.address_mode = BR_ADDRESS_REPEAT;
    t->sampler.mipmaps = true;
    t->sampler.max_anisotropy = 4.0f;
    const cgltf_sampler *s = tv->texture->sampler;
    if (s != NULL) {
        // ponytail: one filter and one address mode per sampler (br_texture_style); glTF's separate min/mag filter and wrap_t follow mag/wrap_s.
        t->sampler.filter = s->mag_filter == cgltf_filter_type_nearest ? BR_FILTER_NEAREST : BR_FILTER_LINEAR;
        t->sampler.address_mode = s->wrap_s == cgltf_wrap_mode_clamp_to_edge   ? BR_ADDRESS_CLAMP
                                  : s->wrap_s == cgltf_wrap_mode_mirrored_repeat ? BR_ADDRESS_MIRROR
                                                                                 : BR_ADDRESS_REPEAT;
        t->sampler.mipmaps = s->min_filter == cgltf_filter_type_undefined || s->min_filter >= cgltf_filter_type_nearest_mipmap_nearest;
        if (!t->sampler.mipmaps) {
            t->sampler.max_anisotropy = 0.0f;
        }
    }
    if (tv->has_transform) {
        t->offset = br_vec2_make(tv->transform.offset[0], tv->transform.offset[1]);
        t->rotation = tv->transform.rotation;
        t->scale = br_vec2_make(tv->transform.scale[0], tv->transform.scale[1]);
        if (tv->transform.has_texcoord) {
            t->uv_set = tv->transform.texcoord > 0 ? 1u : 0u;
        }
    }
}

static void br_gltf_material(const cgltf_data *data, const cgltf_material *m, br_gltf_material_data *md)
{
    md->desc = br_material3d_default(m->unlit ? BR_MATERIAL3D_FLAT : BR_MATERIAL3D_PBR);
    for (int i = 0; i < BR_MATERIAL3D_SLOT_COUNT; i++) {
        md->image[i] = -1;
    }
    br_material3d_desc *d = &md->desc;
    const cgltf_pbr_metallic_roughness *pbr = &m->pbr_metallic_roughness;
    if (m->has_pbr_metallic_roughness) {
        d->base_color = br_color_make(pbr->base_color_factor[0], pbr->base_color_factor[1], pbr->base_color_factor[2],
                                      pbr->base_color_factor[3]);
        d->metallic = pbr->metallic_factor;
        d->roughness = pbr->roughness_factor;
        br_gltf_texture_slot(data, &pbr->base_color_texture, md, BR_MATERIAL3D_BASE_COLOR);
        br_gltf_texture_slot(data, &pbr->metallic_roughness_texture, md, BR_MATERIAL3D_METALLIC_ROUGHNESS);
    } else {
        d->metallic = 1.0f; /* glTF defaults */
        d->roughness = 1.0f;
    }
    br_gltf_texture_slot(data, &m->normal_texture, md, BR_MATERIAL3D_NORMAL);
    br_gltf_texture_slot(data, &m->occlusion_texture, md, BR_MATERIAL3D_OCCLUSION);
    br_gltf_texture_slot(data, &m->emissive_texture, md, BR_MATERIAL3D_EMISSIVE);
    if (m->normal_texture.texture != NULL) {
        d->normal_scale = m->normal_texture.scale;
    }
    if (m->occlusion_texture.texture != NULL) {
        d->occlusion_strength = m->occlusion_texture.scale;
    }
    d->emissive = br_color_make(m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2], 1.0f);
    d->emissive_strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
    d->alpha_mode = m->alpha_mode == cgltf_alpha_mode_mask    ? BR_ALPHA_MASK
                    : m->alpha_mode == cgltf_alpha_mode_blend ? BR_ALPHA_BLEND
                                                              : BR_ALPHA_OPAQUE;
    d->alpha_cutoff = m->alpha_cutoff;
    d->double_sided = m->double_sided != 0;
    if (m->has_transmission && m->transmission.transmission_factor > 0.0f) {
        // ponytail: transmission as plain alpha blending (alpha = 1 - 0.7 * factor); no refraction or screen-space background.
        d->alpha_mode = BR_ALPHA_BLEND;
        d->base_color.a *= 1.0f - 0.7f * m->transmission.transmission_factor;
    }
}

/* EXT_mesh_gpu_instancing: one matrix per instance from the TRANSLATION / ROTATION / SCALE
 * accessors (missing ones are identity). False if the attributes disagree on the count. */
static bool br_gltf_gpu_instances(const cgltf_node *n, br_model_node *mn)
{
    const cgltf_mesh_gpu_instancing *gi = &n->mesh_gpu_instancing;
    const cgltf_accessor *t = NULL;
    const cgltf_accessor *r = NULL;
    const cgltf_accessor *sc = NULL;
    size_t count = 0;
    for (size_t i = 0; i < gi->attributes_count; i++) {
        const cgltf_attribute *a = &gi->attributes[i];
        if (a->name == NULL || a->data == NULL) {
            continue;
        }
        if (SDL_strcmp(a->name, "TRANSLATION") == 0) {
            t = a->data;
        } else if (SDL_strcmp(a->name, "ROTATION") == 0) {
            r = a->data;
        } else if (SDL_strcmp(a->name, "SCALE") == 0) {
            sc = a->data;
        } else {
            continue;
        }
        if (count != 0 && a->data->count != count) {
            return false;
        }
        count = a->data->count;
    }
    if (count == 0) {
        return true;
    }
    mn->instances = br_alloc(sizeof(br_mat4) * count);
    if (mn->instances == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        float tv[3] = { 0.0f, 0.0f, 0.0f };
        float rv[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float sv[3] = { 1.0f, 1.0f, 1.0f };
        if (t != NULL) {
            cgltf_accessor_read_float(t, i, tv, 3);
        }
        if (r != NULL) {
            cgltf_accessor_read_float(r, i, rv, 4);
        }
        if (sc != NULL) {
            cgltf_accessor_read_float(sc, i, sv, 3);
        }
        br_quat q = { rv[0], rv[1], rv[2], rv[3] };
        mn->instances[i] = br_mat4_from_trs(br_vec3_make(tv[0], tv[1], tv[2]), br_quat_normalize(q),
                                            br_vec3_make(sv[0], sv[1], sv[2]));
    }
    mn->instance_count = (uint32_t)count;
    return true;
}

/* ---- primitives */

/* Triangle list indices of a primitive (strips and fans converted). False if not triangles. */
static bool br_gltf_indices(const cgltf_primitive *p, uint32_t vertex_count, br_gltf_prim_data *out)
{
    uint32_t n = p->indices != NULL ? (uint32_t)p->indices->count : vertex_count;
    uint32_t *src = br_alloc(sizeof(uint32_t) * (n > 0 ? n : 1));
    if (src == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        src[i] = p->indices != NULL ? (uint32_t)cgltf_accessor_read_index(p->indices, i) : i;
        if (src[i] >= vertex_count) {
            br_free(src);
            return false;
        }
    }
    if (p->type == cgltf_primitive_type_triangles) {
        out->indices = src;
        out->index_count = n - n % 3;
        return true;
    }
    uint32_t tris = n >= 3 ? n - 2 : 0;
    uint32_t *list = br_alloc(sizeof(uint32_t) * 3 * (tris > 0 ? tris : 1));
    if (list == NULL) {
        br_free(src);
        return false;
    }
    for (uint32_t i = 0; i < tris; i++) {
        if (p->type == cgltf_primitive_type_triangle_fan) {
            list[i * 3 + 0] = src[0];
            list[i * 3 + 1] = src[i + 1];
            list[i * 3 + 2] = src[i + 2];
        } else { /* strip: every other triangle flips to keep CCW */
            list[i * 3 + 0] = src[i % 2 == 0 ? i : i + 1];
            list[i * 3 + 1] = src[i % 2 == 0 ? i + 1 : i];
            list[i * 3 + 2] = src[i + 2];
        }
    }
    br_free(src);
    out->indices = list;
    out->index_count = tris * 3;
    return true;
}

static bool br_gltf_primitive(const char *path, const cgltf_primitive *p, br_gltf_prim_data *out)
{
    if (p->type != cgltf_primitive_type_triangles && p->type != cgltf_primitive_type_triangle_strip &&
        p->type != cgltf_primitive_type_triangle_fan) {
        // ponytail: points and lines primitives are skipped; a line material + LINELIST pipeline would draw them.
        br_log_warn("glTF '%s': skipping a points/lines primitive", path);
        return false;
    }
    const cgltf_accessor *pos = cgltf_find_accessor(p, cgltf_attribute_type_position, 0);
    if (pos == NULL || pos->count == 0) {
        return false;
    }
    uint32_t vc = (uint32_t)pos->count;
    br_vertex3d *v = br_alloc(sizeof(br_vertex3d) * vc);
    if (v == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < vc; i++) {
        v[i].tangent = br_vec4_make(1.0f, 0.0f, 0.0f, 1.0f);
        v[i].color = br_vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
    }
    bool has_normal = false;
    bool has_tangent = false;
    for (size_t a = 0; a < p->attributes_count; a++) {
        const cgltf_attribute *attr = &p->attributes[a];
        const cgltf_accessor *acc = attr->data;
        if (acc == NULL || acc->count != vc) {
            continue;
        }
        for (uint32_t i = 0; i < vc; i++) {
            float f[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            switch (attr->type) {
            case cgltf_attribute_type_position:
                cgltf_accessor_read_float(acc, i, f, 3);
                v[i].position = br_vec3_make(f[0], f[1], f[2]);
                break;
            case cgltf_attribute_type_normal:
                cgltf_accessor_read_float(acc, i, f, 3);
                v[i].normal = br_vec3_make(f[0], f[1], f[2]);
                has_normal = true;
                break;
            case cgltf_attribute_type_tangent:
                cgltf_accessor_read_float(acc, i, f, 4);
                v[i].tangent = br_vec4_make(f[0], f[1], f[2], f[3] < 0.0f ? -1.0f : 1.0f);
                has_tangent = true;
                break;
            case cgltf_attribute_type_texcoord:
                cgltf_accessor_read_float(acc, i, f, 2);
                if (attr->index == 0) {
                    v[i].uv0 = br_vec2_make(f[0], f[1]);
                } else if (attr->index == 1) {
                    v[i].uv1 = br_vec2_make(f[0], f[1]);
                }
                break;
            case cgltf_attribute_type_color:
                if (attr->index == 0) {
                    cgltf_accessor_read_float(acc, i, f, cgltf_num_components(acc->type) >= 4 ? 4 : 3);
                    v[i].color = br_vec4_make(f[0], f[1], f[2], f[3]);
                }
                break;
            case cgltf_attribute_type_joints:
                if (attr->index == 0) {
                    cgltf_uint j[4] = { 0, 0, 0, 0 };
                    cgltf_accessor_read_uint(acc, i, j, 4);
                    for (int k = 0; k < 4; k++) {
                        v[i].joints[k] = (uint16_t)j[k];
                    }
                }
                break;
            case cgltf_attribute_type_weights:
                if (attr->index == 0) {
                    cgltf_accessor_read_float(acc, i, f, 4);
                    v[i].weights = br_vec4_make(f[0], f[1], f[2], f[3]);
                }
                break;
            default:
                break;
            }
        }
    }
    out->vertices = v;
    out->vertex_count = vc;
    if (!br_gltf_indices(p, vc, out)) {
        br_log_warn("glTF '%s': skipping a primitive with bad indices", path);
        br_free(v);
        out->vertices = NULL;
        return false;
    }
    if (!has_normal) {
        // ponytail: smooth normals; glTF asks for flat normals when NORMAL is missing (would need unshared vertices).
        br_geometry3d_compute_normals(v, vc, out->indices, out->index_count);
    }
    if (!has_tangent) {
        // ponytail: per-triangle accumulated tangents, not MikkTSpace; normal maps baked with MikkTSpace can show faint seams.
        br_geometry3d_compute_tangents(v, vc, out->indices, out->index_count);
    }

    uint32_t targets = (uint32_t)p->targets_count;
    if (targets > BR_MESH3D_MAX_MORPH_WEIGHTS) {
        br_log_warn("glTF '%s': %u morph targets, only the first %d are used", path, targets, BR_MESH3D_MAX_MORPH_WEIGHTS);
        targets = BR_MESH3D_MAX_MORPH_WEIGHTS;
    }
    if (targets > 0) {
        out->morph_positions = br_alloc(sizeof(br_vec3) * vc * targets);
        out->morph_normals = br_alloc(sizeof(br_vec3) * vc * targets);
        out->morph_count = out->morph_positions != NULL && out->morph_normals != NULL ? targets : 0;
    }
    for (uint32_t t = 0; t < out->morph_count; t++) {
        const cgltf_morph_target *mt = &p->targets[t];
        for (size_t a = 0; a < mt->attributes_count; a++) {
            const cgltf_attribute *attr = &mt->attributes[a];
            br_vec3 *dst = attr->type == cgltf_attribute_type_position ? out->morph_positions
                           : attr->type == cgltf_attribute_type_normal ? out->morph_normals
                                                                        : NULL;
            if (dst == NULL || attr->data == NULL || attr->data->count != vc) {
                continue;
            }
            for (uint32_t i = 0; i < vc; i++) {
                float f[3] = { 0.0f, 0.0f, 0.0f };
                cgltf_accessor_read_float(attr->data, i, f, 3);
                dst[t * vc + i] = br_vec3_make(f[0], f[1], f[2]);
            }
        }
    }
    return true;
}

/* ---- animations */

static bool br_gltf_channel(const cgltf_data *data, const cgltf_animation_channel *c, br_anim3d_channel *out)
{
    const cgltf_animation_sampler *s = c->sampler;
    if (c->target_node == NULL || s == NULL || s->input == NULL || s->output == NULL || s->input->count == 0) {
        return false;
    }
    switch (c->target_path) {
    case cgltf_animation_path_type_translation: out->path = BR_ANIM3D_TRANSLATION; break;
    case cgltf_animation_path_type_rotation: out->path = BR_ANIM3D_ROTATION; break;
    case cgltf_animation_path_type_scale: out->path = BR_ANIM3D_SCALE; break;
    case cgltf_animation_path_type_weights: out->path = BR_ANIM3D_WEIGHTS; break;
    default: return false;
    }
    out->interpolation = s->interpolation == cgltf_interpolation_type_step           ? BR_ANIM3D_STEP
                         : s->interpolation == cgltf_interpolation_type_cubic_spline ? BR_ANIM3D_CUBIC_SPLINE
                                                                                     : BR_ANIM3D_LINEAR;
    out->target = (uint32_t)cgltf_node_index(data, c->target_node);
    uint32_t keys = (uint32_t)s->input->count;
    uint32_t per_key = out->interpolation == BR_ANIM3D_CUBIC_SPLINE ? 3u : 1u;
    size_t time_count = 0;
    size_t value_count = 0;
    float *times = br_gltf_floats(s->input, &time_count);
    float *values = br_gltf_floats(s->output, &value_count);
    uint32_t components = (uint32_t)(value_count / ((size_t)keys * per_key));
    bool ok = times != NULL && values != NULL && time_count == keys && components > 0 &&
              value_count == (size_t)keys * per_key * components;
    if (ok && out->path == BR_ANIM3D_WEIGHTS && components > BR_MESH3D_MAX_MORPH_WEIGHTS) {
        /* keep the first targets only (the geometry keeps that many too) */
        uint32_t keep = BR_MESH3D_MAX_MORPH_WEIGHTS;
        for (size_t v = 0; v < (size_t)keys * per_key; v++) {
            SDL_memmove(values + v * keep, values + v * components, sizeof(float) * keep);
        }
        components = keep;
    }
    if (!ok || (out->path != BR_ANIM3D_WEIGHTS && components != (out->path == BR_ANIM3D_ROTATION ? 4u : 3u))) {
        br_free(times);
        br_free(values);
        return false;
    }
    out->key_count = keys;
    out->components = components;
    out->times = times;
    out->values = values;
    return true;
}

static void br_gltf_animation(const cgltf_data *data, const cgltf_animation *anim, br_anim3d_clip *clip)
{
    clip->name = br_gltf_strdup(anim->name);
    clip->channels = br_alloc(sizeof(br_anim3d_channel) * (anim->channels_count > 0 ? anim->channels_count : 1));
    if (clip->channels == NULL) {
        return;
    }
    for (size_t i = 0; i < anim->channels_count; i++) {
        br_anim3d_channel *ch = &clip->channels[clip->channel_count];
        if (br_gltf_channel(data, &anim->channels[i], ch)) {
            float last = ch->times[ch->key_count - 1];
            clip->duration = last > clip->duration ? last : clip->duration;
            clip->channel_count++;
        }
    }
}

/* ---- model free */

static void br_gltf_free_model(br_engine *e, br_model *m)
{
    for (uint32_t i = 0; i < m->node_count; i++) {
        br_free(m->nodes[i].name);
        br_free(m->nodes[i].extras);
        br_free(m->nodes[i].instances);
    }
    for (uint32_t i = 0; i < m->mesh_count; i++) {
        br_free(m->meshes[i].name);
    }
    for (uint32_t i = 0; i < m->primitive_count; i++) {
        br_geometry3d_destroy(e, m->primitives[i].geometry);
    }
    for (uint32_t i = 0; i < m->material_count; i++) {
        br_material3d_destroy(e, m->materials[i]);
        if (m->material_names != NULL) {
            br_free(m->material_names[i]);
        }
    }
    br_material3d_destroy(e, m->default_material);
    for (uint32_t i = 0; i < m->texture_count; i++) {
        if (m->textures[i].generation != 0) {
            br_texture_destroy(e, m->textures[i]);
        }
    }
    for (uint32_t i = 0; i < m->skin_count; i++) {
        br_free(m->skins[i].name);
        br_free(m->skins[i].joints);
        br_free(m->skins[i].inverse_binds);
    }
    for (uint32_t i = 0; i < m->clip_count; i++) {
        br_anim3d_clip *c = &m->clips[i];
        for (uint32_t k = 0; k < c->channel_count; k++) {
            br_free(c->channels[k].times);
            br_free(c->channels[k].values);
        }
        br_free(c->channels);
        br_free(c->name);
    }
    br_free(m->nodes);
    br_free(m->roots);
    br_free(m->meshes);
    br_free(m->primitives);
    br_free(m->materials);
    br_free(m->material_names);
    br_free(m->textures);
    br_free(m->skins);
    br_free(m->cameras);
    br_free(m->lights);
    br_free(m->clips);
    br_free(m->path);
    br_free(m->scene_extras);
    br_free(m);
}

static void br_gltf_free_decoded(br_engine *e, br_gltf_decoded *d)
{
    if (d == NULL) {
        return;
    }
    for (uint32_t i = 0; i < d->image_count; i++) {
        stbi_image_free(d->images[i].pixels);
    }
    if (d->model != NULL) {
        for (uint32_t i = 0; d->prims != NULL && i < d->model->primitive_count; i++) {
            br_free(d->prims[i].vertices);
            br_free(d->prims[i].indices);
            br_free(d->prims[i].morph_positions);
            br_free(d->prims[i].morph_normals);
        }
        br_gltf_free_model(e, d->model);
    }
    br_free(d->images);
    br_free(d->prims);
    br_free(d->mats);
    br_free(d);
}

/* ---- decode: everything that needs no GPU */

static void *br_gltf_alloc_array(size_t count, size_t size, bool *ok)
{
    void *p = br_alloc(size * (count > 0 ? count : 1));
    if (p == NULL) {
        *ok = false;
    }
    return p;
}

static bool br_gltf_convert(br_engine *e, const char *path, const cgltf_options *options, const cgltf_data *data,
                            br_gltf_decoded *d)
{
    bool ok = true;
    br_model *m = br_alloc(sizeof(br_model));
    if (m == NULL) {
        return false;
    }
    d->model = m;
    m->refs = 1;

    /* images */
    d->image_count = (uint32_t)data->images_count;
    d->images = br_gltf_alloc_array(data->images_count, sizeof(br_gltf_image), &ok);
    for (uint32_t i = 0; ok && i < d->image_count; i++) {
        br_gltf_decode_image(e, path, options, &data->images[i], &d->images[i]);
    }
    m->texture_count = d->image_count;
    m->textures = br_gltf_alloc_array(m->texture_count, sizeof(br_texture), &ok);

    /* materials */
    m->material_count = (uint32_t)data->materials_count;
    m->materials = br_gltf_alloc_array(m->material_count, sizeof(br_material3d), &ok);
    m->material_names = br_gltf_alloc_array(m->material_count, sizeof(char *), &ok);
    d->mats = br_gltf_alloc_array(m->material_count, sizeof(br_gltf_material_data), &ok);
    for (uint32_t i = 0; ok && i < m->material_count; i++) {
        br_gltf_material(data, &data->materials[i], &d->mats[i]);
        m->material_names[i] = br_gltf_strdup(data->materials[i].name);
    }

    /* meshes and primitives */
    m->mesh_count = (uint32_t)data->meshes_count;
    m->meshes = br_gltf_alloc_array(m->mesh_count, sizeof(br_model_mesh), &ok);
    size_t prim_total = 0;
    for (size_t i = 0; i < data->meshes_count; i++) {
        prim_total += data->meshes[i].primitives_count;
    }
    m->primitives = br_gltf_alloc_array(prim_total, sizeof(br_model_primitive), &ok);
    d->prims = br_gltf_alloc_array(prim_total, sizeof(br_gltf_prim_data), &ok);
    for (uint32_t i = 0; ok && i < m->mesh_count; i++) {
        const cgltf_mesh *mesh = &data->meshes[i];
        br_model_mesh *mm = &m->meshes[i];
        mm->name = br_gltf_strdup(mesh->name);
        mm->first_primitive = m->primitive_count;
        for (size_t p = 0; p < mesh->primitives_count; p++) {
            br_gltf_prim_data *pd = &d->prims[m->primitive_count];
            if (br_gltf_primitive(path, &mesh->primitives[p], pd)) {
                const cgltf_material *mat = mesh->primitives[p].material;
                m->primitives[m->primitive_count].material = mat != NULL ? (int32_t)cgltf_material_index(data, mat) : -1;
                m->primitive_count++;
            }
        }
        mm->primitive_count = m->primitive_count - mm->first_primitive;
    }

    /* skins */
    m->skin_count = (uint32_t)data->skins_count;
    m->skins = br_gltf_alloc_array(m->skin_count, sizeof(br_model_skin), &ok);
    for (uint32_t i = 0; ok && i < m->skin_count; i++) {
        const cgltf_skin *skin = &data->skins[i];
        br_model_skin *ms = &m->skins[i];
        ms->name = br_gltf_strdup(skin->name);
        ms->joint_count = (uint32_t)skin->joints_count;
        ms->joints = br_gltf_alloc_array(skin->joints_count, sizeof(uint32_t), &ok);
        ms->inverse_binds = br_gltf_alloc_array(skin->joints_count, sizeof(br_mat4), &ok);
        for (uint32_t j = 0; ok && j < ms->joint_count; j++) {
            ms->joints[j] = (uint32_t)cgltf_node_index(data, skin->joints[j]);
            ms->inverse_binds[j] = br_mat4_identity();
            if (skin->inverse_bind_matrices != NULL && j < skin->inverse_bind_matrices->count) {
                cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ms->inverse_binds[j].m, 16);
            }
        }
    }

    /* cameras */
    m->camera_count = (uint32_t)data->cameras_count;
    m->cameras = br_gltf_alloc_array(m->camera_count, sizeof(br_camera3d), &ok);
    for (uint32_t i = 0; ok && i < m->camera_count; i++) {
        const cgltf_camera *c = &data->cameras[i];
        if (c->type == cgltf_camera_type_orthographic) {
            const cgltf_camera_orthographic *o = &c->data.orthographic;
            m->cameras[i] = br_camera3d_orthographic(o->ymag, o->znear, o->zfar);
            m->cameras[i].aspect = o->ymag > 0.0f ? o->xmag / o->ymag : 0.0f;
        } else {
            const cgltf_camera_perspective *p = &c->data.perspective;
            /* ponytail: infinite perspective (no zfar) becomes a far plane of 1000 units */
            m->cameras[i] = br_camera3d_perspective(p->yfov, p->znear, p->has_zfar ? p->zfar : 1000.0f);
            m->cameras[i].aspect = p->has_aspect_ratio ? p->aspect_ratio : 0.0f;
        }
    }

    /* lights (KHR_lights_punctual) */
    m->light_count = (uint32_t)data->lights_count;
    m->lights = br_gltf_alloc_array(m->light_count, sizeof(br_light3d), &ok);
    for (uint32_t i = 0; ok && i < m->light_count; i++) {
        const cgltf_light *l = &data->lights[i];
        br_light3d_kind kind = l->type == cgltf_light_type_directional ? BR_LIGHT3D_DIRECTIONAL
                               : l->type == cgltf_light_type_spot      ? BR_LIGHT3D_SPOT
                                                                       : BR_LIGHT3D_POINT;
        br_light3d *ml = &m->lights[i];
        *ml = br_light3d_default(kind);
        ml->color = br_color_make(l->color[0], l->color[1], l->color[2], 1.0f);
        // ponytail: glTF intensities (lux / candela) are passed through unscaled; exporters disagree on units anyway.
        ml->intensity = l->intensity;
        ml->range = l->range;
        if (kind == BR_LIGHT3D_SPOT) {
            ml->inner_angle = l->spot_inner_cone_angle;
            ml->outer_angle = l->spot_outer_cone_angle;
        }
    }

    /* nodes */
    m->node_count = (uint32_t)data->nodes_count;
    m->nodes = br_gltf_alloc_array(m->node_count, sizeof(br_model_node), &ok);
    for (uint32_t i = 0; ok && i < m->node_count; i++) {
        const cgltf_node *n = &data->nodes[i];
        br_model_node *mn = &m->nodes[i];
        mn->name = br_gltf_strdup(n->name);
        mn->extras = n->extras.data != NULL ? br_strdup(n->extras.data) : NULL;
        mn->parent = n->parent != NULL ? (int32_t)cgltf_node_index(data, n->parent) : -1;
        mn->translation = br_vec3_make(0.0f, 0.0f, 0.0f);
        mn->rotation = br_quat_identity();
        mn->scale = br_vec3_make(1.0f, 1.0f, 1.0f);
        if (n->has_matrix) {
            br_anim3d_decompose(br_gltf_mat4(n->matrix), &mn->translation, &mn->rotation, &mn->scale);
        } else {
            if (n->has_translation) {
                mn->translation = br_vec3_make(n->translation[0], n->translation[1], n->translation[2]);
            }
            if (n->has_rotation) {
                br_quat q = { n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3] };
                mn->rotation = br_quat_normalize(q);
            }
            if (n->has_scale) {
                mn->scale = br_vec3_make(n->scale[0], n->scale[1], n->scale[2]);
            }
        }
        mn->mesh = n->mesh != NULL ? (int32_t)cgltf_mesh_index(data, n->mesh) : -1;
        mn->skin = n->skin != NULL ? (int32_t)cgltf_skin_index(data, n->skin) : -1;
        mn->camera = n->camera != NULL ? (int32_t)cgltf_camera_index(data, n->camera) : -1;
        mn->light = n->light != NULL ? (int32_t)cgltf_light_index(data, n->light) : -1;
        /* rest weights: the node's, else the mesh's, else 0 for every target */
        const float *weights = n->weights_count > 0 ? n->weights : (n->mesh != NULL ? n->mesh->weights : NULL);
        size_t given = n->weights_count > 0 ? n->weights_count : (n->mesh != NULL ? n->mesh->weights_count : 0);
        size_t targets = given;
        if (n->mesh != NULL && n->mesh->primitives_count > 0 && n->mesh->primitives[0].targets_count > targets) {
            targets = n->mesh->primitives[0].targets_count;
        }
        mn->weight_count = targets < BR_MESH3D_MAX_MORPH_WEIGHTS ? (uint32_t)targets : BR_MESH3D_MAX_MORPH_WEIGHTS;
        for (uint32_t w = 0; w < mn->weight_count; w++) {
            mn->weights[w] = (weights != NULL && w < given) ? weights[w] : 0.0f;
        }
        if (n->has_mesh_gpu_instancing && n->mesh != NULL && !br_gltf_gpu_instances(n, mn)) {
            br_log_warn("glTF '%s': node '%s': bad EXT_mesh_gpu_instancing attributes; drawn once", path, mn->name);
        }
    }

    /* scene roots: the default scene, else the first, else every parentless node */
    const cgltf_scene *scene = data->scene != NULL ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : NULL);
    m->path = br_strdup(path);
    m->scene_extras = scene != NULL && scene->extras.data != NULL ? br_strdup(scene->extras.data) : NULL;
    m->roots = br_gltf_alloc_array(m->node_count, sizeof(uint32_t), &ok);
    for (uint32_t i = 0; ok && scene != NULL && i < scene->nodes_count; i++) {
        m->roots[m->root_count++] = (uint32_t)cgltf_node_index(data, scene->nodes[i]);
    }
    for (uint32_t i = 0; ok && scene == NULL && i < m->node_count; i++) {
        if (m->nodes[i].parent < 0) {
            m->roots[m->root_count++] = i;
        }
    }

    /* rest-pose bounds of the scene's meshes */
    m->bounds = br_aabb_empty();
    for (uint32_t i = 0; ok && i < m->node_count; i++) {
        if (m->nodes[i].mesh < 0) {
            continue;
        }
        float world[16];
        cgltf_node_transform_world(&data->nodes[i], world);
        const br_model_mesh *mm = &m->meshes[m->nodes[i].mesh];
        for (uint32_t p = 0; p < mm->primitive_count; p++) {
            const br_gltf_prim_data *pd = &d->prims[mm->first_primitive + p];
            br_aabb box = br_aabb_empty();
            for (uint32_t v = 0; v < pd->vertex_count; v++) {
                box = br_aabb_extend(box, pd->vertices[v].position);
            }
            uint32_t copies = m->nodes[i].instance_count;
            for (uint32_t k = 0; k < (copies > 0 ? copies : 1); k++) {
                br_mat4 w = copies > 0 ? br_mat4_mul(br_gltf_mat4(world), m->nodes[i].instances[k]) : br_gltf_mat4(world);
                m->bounds = br_aabb_union(m->bounds, br_aabb_transform(box, w));
            }
        }
    }

    /* animations */
    m->clips = br_gltf_alloc_array(data->animations_count, sizeof(br_anim3d_clip), &ok);
    for (uint32_t i = 0; ok && i < (uint32_t)data->animations_count; i++) {
        br_gltf_animation(data, &data->animations[i], &m->clips[i]);
        m->clip_count++;
    }
    if (!ok) {
        br_log_error("glTF '%s': out of memory", path);
    }
    return ok;
}

static void *br_gltf_decode(br_engine *e, const char *path, const void *bytes, size_t size)
{
    br_gltf_decoded *d = br_alloc(sizeof(br_gltf_decoded));
    if (d == NULL) {
        return NULL;
    }
    cgltf_options options;
    SDL_zero(options);
    options.memory.alloc_func = br_gltf_alloc;
    options.memory.free_func = br_gltf_free_cb;
    options.file.read = br_gltf_read_file;
    options.file.release = br_gltf_release_file;
    options.file.user_data = e;
    cgltf_data *data = NULL;
    cgltf_result r = cgltf_parse(&options, bytes, size, &data);
    if (r != cgltf_result_success) {
        br_log_error("glTF '%s': %s", path, br_gltf_result_text(r));
        br_free(d);
        return NULL;
    }
    bool ok = br_gltf_check_extensions(path, data, d);
    if (ok) {
        r = cgltf_load_buffers(&options, data, path);
        if (r != cgltf_result_success) {
            br_log_error("glTF '%s': cannot load buffers: %s", path, br_gltf_result_text(r));
            ok = false;
        }
    }
    if (ok) {
        r = cgltf_validate(data);
        if (r != cgltf_result_success) {
            br_log_error("glTF '%s': %s (validation failed)", path, br_gltf_result_text(r));
            ok = false;
        }
    }
    if (ok) {
        ok = br_gltf_convert(e, path, &options, data, d);
    }
    cgltf_free(data);
    if (!ok) {
        br_gltf_free_decoded(e, d);
        return NULL;
    }
    return d;
}

/* ---- finish: GPU resources (main thread) */

static void *br_gltf_finish(br_engine *e, const char *path, void *decoded)
{
    br_gltf_decoded *d = decoded;
    br_model *m = d->model;
    br_gltf_state *s = e->gltf;

    for (uint32_t i = 0; s != NULL && i < d->unknown_count; i++) {
        bool seen = false;
        for (uint32_t k = 0; k < s->logged_ext.count && !seen; k++) {
            seen = SDL_strcmp(br_array_at(&s->logged_ext, k), d->unknown[i]) == 0;
        }
        if (!seen) {
            br_log_info("glTF '%s': ignoring unsupported optional extension %s (logged once)", path, d->unknown[i]);
            br_array_push(&s->logged_ext, d->unknown[i]);
        }
    }

    br_texture_style style;
    SDL_zero(style);
    style.address_mode = BR_ADDRESS_REPEAT;
    style.mipmaps = true; /* generates the mip chain; material slots pick their own sampler */
    style.max_anisotropy = 4.0f;
    for (uint32_t i = 0; i < d->image_count; i++) {
        const br_gltf_image *img = &d->images[i];
        if (img->pixels != NULL) {
            m->textures[i] = br_texture_create_rgba8(e, (uint32_t)img->width, (uint32_t)img->height, img->pixels, &style);
        }
    }
    for (uint32_t i = 0; i < m->material_count; i++) {
        br_gltf_material_data *md = &d->mats[i];
        for (int slot = 0; slot < BR_MATERIAL3D_SLOT_COUNT; slot++) {
            if (md->image[slot] >= 0) {
                md->desc.textures[slot].texture = m->textures[md->image[slot]];
            }
        }
        m->materials[i] = br_material3d_create(e, &md->desc);
    }
    br_material3d_desc def = br_material3d_default(BR_MATERIAL3D_PBR);
    m->default_material = br_material3d_create(e, &def);
    for (uint32_t i = 0; i < m->primitive_count; i++) {
        const br_gltf_prim_data *pd = &d->prims[i];
        br_geometry3d_desc gd;
        SDL_zero(gd);
        gd.vertices = pd->vertices;
        gd.vertex_count = pd->vertex_count;
        gd.indices = pd->indices;
        gd.index_count = pd->index_count;
        gd.morph_positions = pd->morph_positions;
        gd.morph_normals = pd->morph_normals;
        gd.morph_target_count = pd->morph_count;
        m->primitives[i].geometry = br_geometry3d_create(e, &gd);
    }
    d->model = NULL; /* the model survives; the rest goes */
    br_gltf_free_decoded(e, d);
    return m;
}

static void br_gltf_unload(br_engine *e, void *asset)
{
    br_model_release(e, asset);
}

/* ---- public model functions */

br_model *br_model_load(br_engine *e, const char *path)
{
    br_asset_options options;
    SDL_zero(options);
    options.type = "model";
    void *asset = br_assets_load_with(e, path, &options);
    const char *type = br_assets_type(e, path);
    if (asset != NULL && (type == NULL || SDL_strcmp(type, "model") != 0)) {
        br_log_error("model '%s': already loaded as a %s asset", path, type != NULL ? type : "different");
        br_assets_unload(e, path);
        return NULL;
    }
    return asset;
}

br_model *br_model_parse(br_engine *e, const char *path, const void *bytes, size_t size)
{
    void *decoded = br_gltf_decode(e, path, bytes, size);
    return decoded != NULL ? br_gltf_finish(e, path, decoded) : NULL;
}

void br_model_release(br_engine *e, br_model *model)
{
    if (model == NULL) {
        return;
    }
    if (model->refs > 1) {
        model->refs--;
        return;
    }
    br_gltf_free_model(e, model);
}

int32_t br_model_find_clip(const br_model *model, const char *name)
{
    for (uint32_t i = 0; model != NULL && name != NULL && i < model->clip_count; i++) {
        if (SDL_strcmp(model->clips[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t br_model_find_node(const br_model *model, const char *name)
{
    for (uint32_t i = 0; model != NULL && name != NULL && i < model->node_count; i++) {
        if (SDL_strcmp(model->nodes[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* ---- module */

static void br_gltf_shutdown(br_engine *e, void *user)
{
    (void)user;
    br_gltf_state *s = e->gltf;
    br_array_free(&s->anims);
    br_array_free(&s->logged_ext);
    br_gltf_components_free(e);
    br_free(s);
    e->gltf = NULL;
}

void br_gltf_register(br_engine *e)
{
    br_gltf_state *s = br_alloc(sizeof(br_gltf_state));
    if (s == NULL) {
        br_log_error("out of memory registering the glTF module");
        return;
    }
    e->gltf = s;
    br_array_init(&s->anims, sizeof(br_gltf_anim));
    br_array_init(&s->logged_ext, BR_GLTF_EXT_NAME);
    br_gltf_instance_register(e);
    br_gltf_components_register(e);

    br_asset_loader_desc desc;
    SDL_zero(desc);
    desc.type_name = "model";
    desc.extensions = "gltf,glb";
    desc.decode = br_gltf_decode;
    desc.finish = br_gltf_finish;
    desc.unload = br_gltf_unload;
    br_assets_register_loader(e, &desc);

    /* after game UPDATE systems (they start clips), before PRE_RENDER collects the views */
    br_system_add(e, "gltf.animate", BR_PHASE_LATE_UPDATE, 0, br_anim3d_system, NULL);
    br_engine_on_shutdown(e, br_gltf_shutdown, NULL);
}
