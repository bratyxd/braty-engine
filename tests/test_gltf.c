/* test_gltf - glTF + 3D animation, headless: keyframe sampling (linear, step, cubic spline,
 * quaternion slerp along the shortest path), joint matrices on a two-bone fixture built in code,
 * matrix decomposition, parsing tests/data/tiny.gltf (embedded base64 buffer and image),
 * compressed/required-extension rejection, the animator system, model ref counts and unload. */
#include "braty/braty.h"
#include "br_test.h"

static bool near_eq(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-4f;
}

static bool vec3_eq(br_vec3 a, br_vec3 b)
{
    return near_eq(a.x, b.x) && near_eq(a.y, b.y) && near_eq(a.z, b.z);
}

static br_anim3d_channel make_channel(br_anim3d_path path, br_anim3d_interpolation interp, uint32_t keys,
                                      uint32_t components, float *times, float *values)
{
    br_anim3d_channel ch;
    SDL_zero(ch);
    ch.path = path;
    ch.interpolation = interp;
    ch.key_count = keys;
    ch.components = components;
    ch.times = times;
    ch.values = values;
    return ch;
}

static void test_sampling(void)
{
    float out[4];
    /* linear: keys need not start at 0; clamped outside */
    float lt[2] = { 1.0f, 3.0f };
    float lv[2] = { 0.0f, 10.0f };
    br_anim3d_channel lin = make_channel(BR_ANIM3D_WEIGHTS, BR_ANIM3D_LINEAR, 2, 1, lt, lv);
    br_anim3d_sample(&lin, 2.0f, out);
    BR_CHECK(near_eq(out[0], 5.0f));
    br_anim3d_sample(&lin, 0.0f, out);
    BR_CHECK(near_eq(out[0], 0.0f));
    br_anim3d_sample(&lin, 9.0f, out);
    BR_CHECK(near_eq(out[0], 10.0f));

    /* step: the previous key's value */
    float st[3] = { 0.0f, 1.0f, 2.0f };
    float sv[3] = { 1.0f, 2.0f, 3.0f };
    br_anim3d_channel step = make_channel(BR_ANIM3D_WEIGHTS, BR_ANIM3D_STEP, 3, 1, st, sv);
    br_anim3d_sample(&step, 1.5f, out);
    BR_CHECK(near_eq(out[0], 2.0f));
    br_anim3d_sample(&step, 0.99f, out);
    BR_CHECK(near_eq(out[0], 1.0f));
    br_anim3d_sample(&step, 2.5f, out);
    BR_CHECK(near_eq(out[0], 3.0f));

    /* cubic spline: per key in-tangent, value, out-tangent */
    float ct[2] = { 0.0f, 1.0f };
    float cv[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    br_anim3d_channel cubic = make_channel(BR_ANIM3D_WEIGHTS, BR_ANIM3D_CUBIC_SPLINE, 2, 1, ct, cv);
    br_anim3d_sample(&cubic, 0.5f, out);
    BR_CHECK(near_eq(out[0], 0.5f)); /* flat tangents: smoothstep midpoint */
    br_anim3d_sample(&cubic, 0.25f, out);
    BR_CHECK(near_eq(out[0], 0.15625f)); /* 3u^2 - 2u^3 */
    cv[2] = 2.0f; /* out-tangent of key 0: h10(0.5) * 2 = 0.25 more */
    br_anim3d_sample(&cubic, 0.5f, out);
    BR_CHECK(near_eq(out[0], 0.75f));
    br_anim3d_sample(&cubic, 1.0f, out);
    BR_CHECK(near_eq(out[0], 1.0f));
    /* cubic with dt != 1 scales the tangents: keys at 0 and 2, out-tangent 1 -> h10(0.5) * 1 * 2 = 0.25 */
    float ct2[2] = { 0.0f, 2.0f };
    float cv2[6] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f };
    br_anim3d_channel cubic2 = make_channel(BR_ANIM3D_WEIGHTS, BR_ANIM3D_CUBIC_SPLINE, 2, 1, ct2, cv2);
    br_anim3d_sample(&cubic2, 1.0f, out);
    BR_CHECK(near_eq(out[0], 0.75f));

    /* quaternion slerp takes the shortest path: q1 is -(90 degrees about Y) */
    br_quat q90 = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), BR_PI * 0.5f);
    float rt[2] = { 0.0f, 1.0f };
    float rv[8] = { 0.0f, 0.0f, 0.0f, 1.0f, -q90.x, -q90.y, -q90.z, -q90.w };
    br_anim3d_channel rot = make_channel(BR_ANIM3D_ROTATION, BR_ANIM3D_LINEAR, 2, 4, rt, rv);
    br_anim3d_sample(&rot, 0.5f, out);
    br_quat q45 = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), BR_PI * 0.25f);
    br_quat got = { out[0], out[1], out[2], out[3] };
    BR_CHECK(near_eq(SDL_fabsf(br_quat_dot(got, q45)), 1.0f));
    BR_CHECK(near_eq(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3], 1.0f));
    /* cubic rotations come out normalised */
    float rcv[24] = { 0 };
    rcv[4 + 3] = 1.0f;                    /* key 0 value: identity */
    rcv[12 + 4 + 0] = 0.0f;               /* key 1 value: 90 about Y */
    rcv[12 + 4 + 1] = q90.y;
    rcv[12 + 4 + 3] = q90.w;
    br_anim3d_channel rcub = make_channel(BR_ANIM3D_ROTATION, BR_ANIM3D_CUBIC_SPLINE, 2, 4, rt, rcv);
    br_anim3d_sample(&rcub, 0.5f, out);
    BR_CHECK(near_eq(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3], 1.0f));
}

/* Two bones: bone 0 at the origin, bone 1 one unit up (child). Bend bone 1 by 90 degrees about Z. */
static void test_two_bone_skin(void)
{
    br_mat4 bind[2];
    bind[0] = br_mat4_identity();
    bind[1] = br_mat4_translation(br_vec3_make(0.0f, 1.0f, 0.0f));
    br_mat4 inverse_bind[2];
    bool ok = true;
    inverse_bind[0] = br_mat4_invert(bind[0], &ok);
    inverse_bind[1] = br_mat4_invert(bind[1], &ok);
    BR_CHECK(ok);

    /* posed: bone 1's local = translate(0,1,0) * rotZ(90) */
    br_mat4 world[2];
    world[0] = br_mat4_identity();
    world[1] = br_mat4_mul(world[0], br_mat4_mul(br_mat4_translation(br_vec3_make(0.0f, 1.0f, 0.0f)),
                                                 br_mat4_rotation(br_vec3_make(0.0f, 0.0f, 1.0f), BR_PI * 0.5f)));
    uint32_t joints[2] = { 0, 1 };
    br_mat4 jm[2];
    /* the mesh node sits elsewhere: its transform must cancel out (glTF ignores it for skins) */
    br_mat4 mesh_world = br_mat4_translation(br_vec3_make(5.0f, 0.0f, 0.0f));
    br_anim3d_joint_matrices(mesh_world, world, joints, inverse_bind, 2, jm);

    /* the renderer draws mesh_world * joint * vertex */
    br_vec3 tip = br_vec3_make(0.0f, 2.0f, 0.0f);   /* weighted to bone 1 */
    br_vec3 base = br_vec3_make(0.0f, 0.5f, 0.0f);  /* weighted to bone 0 */
    BR_CHECK(vec3_eq(br_mat4_transform_point(br_mat4_mul(mesh_world, jm[1]), tip), br_vec3_make(-1.0f, 1.0f, 0.0f)));
    BR_CHECK(vec3_eq(br_mat4_transform_point(br_mat4_mul(mesh_world, jm[0]), base), base));
    /* half/half weights at the joint itself stay put */
    br_vec3 mid = br_vec3_make(0.0f, 1.0f, 0.0f);
    br_vec3 a = br_mat4_transform_point(br_mat4_mul(mesh_world, jm[0]), mid);
    br_vec3 b = br_mat4_transform_point(br_mat4_mul(mesh_world, jm[1]), mid);
    BR_CHECK(vec3_eq(br_vec3_lerp(a, b, 0.5f), mid));
    /* bind pose gives identity joint matrices */
    br_anim3d_joint_matrices(br_mat4_identity(), bind, joints, inverse_bind, 2, jm);
    BR_CHECK(br_mat4_nearly_equal(jm[0], br_mat4_identity(), 1e-5f));
    BR_CHECK(br_mat4_nearly_equal(jm[1], br_mat4_identity(), 1e-5f));
}

static void test_decompose(void)
{
    br_quat q = br_quat_from_euler(0.3f, -1.1f, 2.0f);
    br_mat4 m = br_mat4_from_trs(br_vec3_make(1.0f, -2.0f, 3.0f), q, br_vec3_make(2.0f, 0.5f, 3.0f));
    br_vec3 t;
    br_quat r;
    br_vec3 s;
    br_anim3d_decompose(m, &t, &r, &s);
    BR_CHECK(vec3_eq(t, br_vec3_make(1.0f, -2.0f, 3.0f)));
    BR_CHECK(vec3_eq(s, br_vec3_make(2.0f, 0.5f, 3.0f)));
    BR_CHECK(near_eq(SDL_fabsf(br_quat_dot(r, q)), 1.0f));
}

static char *read_text(br_engine *e, const char *path, size_t *size)
{
    void *bytes = NULL;
    BR_CHECK(br_file_read_all(e, path, &bytes, size));
    return bytes;
}

static void test_parse(br_engine *e)
{
    br_model *m = br_model_load(e, "tiny.gltf");
    BR_CHECK(m != NULL);
    if (m == NULL) {
        return;
    }
    BR_CHECK(m->node_count == 5);
    BR_CHECK(m->root_count == 1 && m->roots[0] == 0);
    BR_CHECK(SDL_strcmp(m->nodes[0].name, "Root") == 0);
    BR_CHECK(m->nodes[0].extras != NULL && SDL_strstr(m->nodes[0].extras, "\"health\"") != NULL);
    BR_CHECK(m->nodes[1].extras == NULL);
    BR_CHECK(m->nodes[1].parent == 0 && m->nodes[1].mesh == 0);
    BR_CHECK(vec3_eq(m->nodes[1].translation, br_vec3_make(1.0f, 2.0f, 3.0f)));
    /* matrix node: 90 degrees about +Y, translated */
    br_quat y90 = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), BR_PI * 0.5f);
    BR_CHECK(vec3_eq(m->nodes[2].translation, br_vec3_make(4.0f, 5.0f, 6.0f)));
    BR_CHECK(near_eq(SDL_fabsf(br_quat_dot(m->nodes[2].rotation, y90)), 1.0f));
    BR_CHECK(vec3_eq(m->nodes[2].scale, br_vec3_make(1.0f, 1.0f, 1.0f)));
    BR_CHECK(br_model_find_node(m, "Turned") == 2 && br_model_find_node(m, "nope") == -1);
    /* camera + light */
    BR_CHECK(m->camera_count == 1 && m->nodes[3].camera == 0);
    BR_CHECK(near_eq(m->cameras[0].fov_y, 0.8f) && near_eq(m->cameras[0].far_z, 50.0f) &&
             near_eq(m->cameras[0].aspect, 1.5f));
    BR_CHECK(m->light_count == 1 && m->nodes[4].light == 0);
    BR_CHECK(m->lights[0].kind == BR_LIGHT3D_POINT && near_eq(m->lights[0].intensity, 3.0f) &&
             near_eq(m->lights[0].range, 10.0f) && near_eq(m->lights[0].color.g, 0.5f));
    /* mesh: 3 vertices, generated normals (+Z for this CCW triangle) and tangents */
    BR_CHECK(m->mesh_count == 1 && m->primitive_count == 1);
    uint32_t vc = 0;
    uint32_t ic = 0;
    const br_vertex3d *v = br_geometry3d_vertices(e, m->primitives[0].geometry, &vc);
    const uint32_t *idx = br_geometry3d_indices(e, m->primitives[0].geometry, &ic);
    BR_CHECK(v != NULL && vc == 3 && idx != NULL && ic == 3);
    if (v != NULL && vc == 3) {
        BR_CHECK(vec3_eq(v[1].position, br_vec3_make(1.0f, 0.0f, 0.0f)));
        BR_CHECK(near_eq(v[2].uv0.x, 0.0f) && near_eq(v[2].uv0.y, 0.0f));
        BR_CHECK(vec3_eq(v[0].normal, br_vec3_make(0.0f, 0.0f, 1.0f)));
        BR_CHECK(near_eq(v[0].tangent.x, 1.0f));
    }
    BR_CHECK(br_aabb_is_valid(m->bounds) && vec3_eq(m->bounds.min, br_vec3_make(1.0f, 2.0f, 3.0f)) &&
             vec3_eq(m->bounds.max, br_vec3_make(2.0f, 3.0f, 3.0f)));
    /* material: factors, texture from a data URI, sampler, transform, extensions */
    BR_CHECK(m->material_count == 1 && SDL_strcmp(m->material_names[0], "Red") == 0);
    br_material3d_desc *md = br_material3d_params(e, m->materials[0]);
    BR_CHECK(md != NULL);
    if (md != NULL) {
        BR_CHECK(md->model == BR_MATERIAL3D_PBR);
        BR_CHECK(near_eq(md->base_color.g, 0.5f) && near_eq(md->metallic, 0.25f) && near_eq(md->roughness, 0.75f));
        BR_CHECK(near_eq(md->emissive_strength, 4.0f));
        BR_CHECK(md->alpha_mode == BR_ALPHA_MASK && near_eq(md->alpha_cutoff, 0.3f) && md->double_sided);
        const br_material3d_texture *t = &md->textures[BR_MATERIAL3D_BASE_COLOR];
        BR_CHECK(br_texture_valid(e, t->texture));
        BR_CHECK(t->has_sampler && t->sampler.filter == BR_FILTER_NEAREST &&
                 t->sampler.address_mode == BR_ADDRESS_CLAMP && !t->sampler.mipmaps);
        BR_CHECK(near_eq(t->offset.x, 0.5f) && near_eq(t->scale.x, 2.0f));
        br_texture_info info;
        BR_CHECK(br_texture_get_info(e, t->texture, &info) && info.source_width == 2 && info.source_height == 2);
    }
    /* animation */
    BR_CHECK(m->clip_count == 1 && br_model_find_clip(m, "Move") == 0 && br_model_find_clip(m, "Run") == -1);
    BR_CHECK(near_eq(m->clips[0].duration, 1.0f));
    BR_CHECK(m->clips[0].channel_count == 1 && m->clips[0].channels[0].target == 1 &&
             m->clips[0].channels[0].path == BR_ANIM3D_TRANSLATION);

    /* the same path again: cached, one more reference */
    BR_CHECK(br_model_load(e, "tiny.gltf") == m);
    BR_CHECK(br_assets_ref_count(e, "tiny.gltf") == 2);
    BR_CHECK(SDL_strcmp(br_assets_type(e, "tiny.gltf"), "model") == 0);
    br_assets_unload(e, "tiny.gltf");
    BR_CHECK(br_assets_ref_count(e, "tiny.gltf") == 1);

    /* instance + animator */
    br_node3d inst = br_model_instantiate(e, m);
    BR_CHECK(br_node3d_valid(e, inst) && br_node3d_type_of(e, inst) == br_model3d_type(e));
    BR_CHECK(br_model3d_model(e, inst) == m && m->refs == 2);
    BR_CHECK(br_animator3d_count(e) == 1);
    br_node3d tri = br_model3d_node(e, inst, 1);
    BR_CHECK(SDL_strcmp(br_node3d_label(e, tri), "Tri") == 0);
    BR_CHECK(br_node3d_find_by_label(e, inst, "Turned").index == br_model3d_node(e, inst, 2).index);
    BR_CHECK(br_mesh3d_get(e, br_node3d_first_child(e, tri)) != NULL);
    BR_CHECK(br_camera3d_get(e, br_node3d_first_child(e, br_model3d_node(e, inst, 3))) != NULL);
    BR_CHECK(br_light3d_get(e, br_node3d_first_child(e, br_model3d_node(e, inst, 4))) != NULL);
    BR_CHECK(!br_animator3d_play(e, inst, "Run"));
    BR_CHECK(br_animator3d_play(e, inst, "Move"));
    br_engine_step(e, 0.25f);
    BR_CHECK(vec3_eq(br_node3d_position(e, tri), br_vec3_make(0.5f, 0.0f, 0.0f)));
    br_animator3d *a = br_animator3d_get(e, inst);
    BR_CHECK(a != NULL && near_eq(a->time, 0.25f));
    br_engine_step(e, 1.0f); /* loops: 1.25 -> 0.25 */
    BR_CHECK(vec3_eq(br_node3d_position(e, tri), br_vec3_make(0.5f, 0.0f, 0.0f)));
    a = br_animator3d_get(e, inst);
    a->loop = false;
    br_engine_step(e, 5.0f); /* clamps at the end */
    BR_CHECK(vec3_eq(br_node3d_position(e, tri), br_vec3_make(2.0f, 0.0f, 0.0f)));
    /* a second instance animates independently */
    br_node3d inst2 = br_model_instantiate(e, m);
    BR_CHECK(m->refs == 3 && br_animator3d_count(e) == 2);
    br_animator3d_play(e, inst2, "Move");
    br_animator3d_get(e, inst2)->speed = 0.5f;
    br_engine_step(e, 1.0f);
    BR_CHECK(vec3_eq(br_node3d_position(e, br_model3d_node(e, inst2, 1)), br_vec3_make(1.0f, 0.0f, 0.0f)));
    BR_CHECK(vec3_eq(br_node3d_position(e, tri), br_vec3_make(2.0f, 0.0f, 0.0f)));

    /* unloading the asset while instances live: the cache entry goes, the model stays */
    br_assets_unload(e, "tiny.gltf");
    BR_CHECK(br_assets_ref_count(e, "tiny.gltf") == 0 && br_assets_get(e, "tiny.gltf") == NULL);
    BR_CHECK(m->refs == 2 && br_model3d_model(e, inst2) == m);
    br_node3d_destroy(e, inst); /* outside a frame: freed now; the moved animator keeps working */
    BR_CHECK(br_animator3d_count(e) == 1 && m->refs == 1);
    br_engine_step(e, 0.5f);
    BR_CHECK(vec3_eq(br_node3d_position(e, br_model3d_node(e, inst2, 1)), br_vec3_make(1.5f, 0.0f, 0.0f)));
    br_node3d_destroy(e, inst2); /* last reference: the model is freed */
    BR_CHECK(br_animator3d_count(e) == 0);
}

/* Same document from memory with one more extension listed: compressed ones must fail. */
static void test_rejection(br_engine *e)
{
    size_t size = 0;
    char *text = read_text(e, "tiny.gltf", &size);
    if (text == NULL) {
        return;
    }
    const char *marker = "\"extensionsUsed\": [";
    char *at = SDL_strstr(text, marker);
    BR_CHECK(at != NULL);
    const char *inserts[] = {
        "\"KHR_draco_mesh_compression\", ",
        "\"EXT_meshopt_compression\", ",
        "\"KHR_texture_basisu\", ",
    };
    for (size_t i = 0; at != NULL && i < BR_ARRAY_COUNT(inserts); i++) {
        size_t head = (size_t)(at - text) + SDL_strlen(marker);
        size_t extra = SDL_strlen(inserts[i]);
        char *doc = br_alloc(size + extra + 1);
        SDL_memcpy(doc, text, head);
        SDL_memcpy(doc + head, inserts[i], extra);
        SDL_memcpy(doc + head + extra, text + head, size - head);
        BR_CHECK(br_model_parse(e, "compressed.gltf", doc, size + extra) == NULL);
        br_free(doc);
    }
    /* an unknown REQUIRED extension fails too; the plain file parses */
    const char *req = "{ \"asset\": { \"version\": \"2.0\" }, \"extensionsUsed\": [\"EXT_x\"], \"extensionsRequired\": [\"EXT_x\"] }";
    BR_CHECK(br_model_parse(e, "required.gltf", req, SDL_strlen(req)) == NULL);
    br_model *m = br_model_parse(e, "tiny.gltf", text, size);
    BR_CHECK(m != NULL && m->refs == 1);
    br_model_release(e, m);
    /* garbage and missing files fail cleanly */
    BR_CHECK(br_model_parse(e, "junk.glb", "glTF\x02\0\0\0junk", 12) == NULL);
    BR_CHECK(br_model_load(e, "missing.glb") == NULL);
    br_free(text);
}

int main(void)
{
    test_sampling();
    test_two_bone_skin();
    test_decompose();
    br_engine *e = br_engine_create("test_gltf", "../../tests/data") /* relative to the exe */;
    BR_CHECK(e != NULL);
    if (e != NULL) {
        test_parse(e);
        test_rejection(e);
        br_engine_destroy(e);
    }
    return br_test_finish();
}
