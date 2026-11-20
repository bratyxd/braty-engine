/* br_anim_clip: clips (flat keyframe arrays), sampling, clip sources (flipbooks, spritesheets,
 * glTF) and *.anim.json libraries. */
#include "animation/br_animation_internal.h"

#include "cjson/cJSON.h"

#define BR_ANIM_JSON_MAX_KEYS 100000   /* keys per track in a file (27 minutes at 60 keys/s) */

/* ---- building clips */

br_anim_clip *br_anim_clip_create(const char *name, float duration, br_anim_loop loop)
{
    br_anim_clip *clip = br_alloc(sizeof(br_anim_clip));
    if (clip == NULL) {
        return NULL;
    }
    clip->name = br_strdup(name != NULL ? name : "");
    clip->duration = duration > 0.0f ? duration : 0.0f;
    clip->loop = loop;
    return clip;
}

void br_anim_clip_destroy(br_anim_clip *clip)
{
    if (clip == NULL) {
        return;
    }
    for (uint32_t i = 0; i < clip->track_count; i++) {
        br_anim_track *t = &clip->tracks[i];
        br_free(t->path);
        if (t->owns_keys) {
            br_free((float *)t->times);
            br_free((float *)t->values);
        }
    }
    for (uint32_t i = 0; i < clip->marker_count; i++) {
        br_free(clip->markers[i].name);
    }
    br_free(clip->tracks);
    br_free(clip->markers);
    br_free(clip->textures);
    br_free(clip->name);
    br_free(clip);
}

static uint32_t br_anim_floats_per_key(br_anim_interpolation interpolation, uint32_t components)
{
    bool tangents = interpolation == BR_ANIM_CUBIC || interpolation == BR_ANIM_BEZIER;
    return tangents ? components * 3 : components;
}

static br_anim_track *br_anim_add(br_anim_clip *clip, const char *path, br_anim_interpolation interpolation,
                                  uint32_t components, uint32_t key_count, const float *times, const float *values,
                                  bool copy)
{
    if (clip == NULL || path == NULL || components == 0 || components > BR_ANIM_MAX_COMPONENTS || key_count == 0 ||
        times == NULL || values == NULL) {
        br_log_error("br_anim_clip_add_track: bad arguments (path, 1..8 components, keys required)");
        return NULL;
    }
    br_anim_track *tracks = br_realloc(clip->tracks, sizeof(br_anim_track) * (clip->track_count + 1));
    if (tracks == NULL) {
        return NULL;
    }
    clip->tracks = tracks;
    br_anim_track *t = &tracks[clip->track_count];
    SDL_zerop(t);
    t->path = br_strdup(path);
    t->interpolation = interpolation;
    t->ease = BR_EASE_LINEAR;
    t->components = components;
    t->key_count = key_count;
    t->owns_keys = copy;
    if (copy) {
        size_t value_floats = (size_t)key_count * br_anim_floats_per_key(interpolation, components);
        float *tc = br_alloc(sizeof(float) * key_count);
        float *vc = br_alloc(sizeof(float) * value_floats);
        if (tc == NULL || vc == NULL || t->path == NULL) {
            br_free(tc);
            br_free(vc);
            br_free(t->path);
            return NULL;
        }
        SDL_memcpy(tc, times, sizeof(float) * key_count);
        SDL_memcpy(vc, values, sizeof(float) * value_floats);
        t->times = tc;
        t->values = vc;
    } else {
        t->times = times;
        t->values = values;
    }
    clip->track_count++;
    clip->duration = SDL_max(clip->duration, times[key_count - 1]);
    return t;
}

br_anim_track *br_anim_clip_add_track(br_anim_clip *clip, const char *path, br_anim_interpolation interpolation,
                                      uint32_t components, uint32_t key_count, const float *times,
                                      const float *values)
{
    return br_anim_add(clip, path, interpolation, components, key_count, times, values, true);
}

br_anim_track *br_anim_clip_add_track_borrowed(br_anim_clip *clip, const char *path,
                                               br_anim_interpolation interpolation, uint32_t components,
                                               uint32_t key_count, const float *times, const float *values)
{
    return br_anim_add(clip, path, interpolation, components, key_count, times, values, false);
}

bool br_anim_clip_add_marker(br_anim_clip *clip, float time, const char *name)
{
    if (clip == NULL || name == NULL) {
        return false;
    }
    br_anim_marker *m = br_realloc(clip->markers, sizeof(br_anim_marker) * (clip->marker_count + 1));
    if (m == NULL) {
        return false;
    }
    clip->markers = m;
    m[clip->marker_count].time = time;
    m[clip->marker_count].name = br_strdup(name);
    clip->marker_count++;
    return true;
}

uint32_t br_anim_clip_add_texture(br_anim_clip *clip, br_texture texture)
{
    for (uint32_t i = 0; i < clip->texture_count; i++) {
        if (clip->textures[i].index == texture.index && clip->textures[i].generation == texture.generation) {
            return i;
        }
    }
    br_texture *t = br_realloc(clip->textures, sizeof(br_texture) * (clip->texture_count + 1));
    if (t == NULL) {
        return 0;
    }
    clip->textures = t;
    t[clip->texture_count] = texture;
    return clip->texture_count++;
}

/* ---- sampling */

static void br_anim_normalize4(float *q)
{
    float len = SDL_sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 0.0f) {
        for (int i = 0; i < 4; i++) {
            q[i] /= len;
        }
    }
}

/* The segment lo with times[lo] <= t < times[lo + 1]; times[0] < t < times[n - 1]. Tries the
 * cached segment and the next one before a binary search. */
static uint32_t br_anim_segment(const float *times, uint32_t n, float t, uint32_t *hint)
{
    uint32_t h = hint != NULL ? *hint : 0;
    if (h + 1 < n && times[h] <= t && t < times[h + 1]) {
        return h;
    }
    if (h + 2 < n && times[h + 1] <= t && t < times[h + 2]) {
        h++;
    } else {
        uint32_t lo = 0;
        uint32_t hi = n - 1;
        while (hi - lo > 1) {
            uint32_t mid = (lo + hi) / 2;
            if (times[mid] <= t) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        h = lo;
    }
    if (hint != NULL) {
        *hint = h;
    }
    return h;
}

void br_anim_track_sample(const br_anim_track *track, float t, bool quaternion, uint32_t *key_hint, float *out)
{
    uint32_t n = track->key_count;
    uint32_t c = track->components;
    if (n == 0 || c == 0) {
        return;
    }
    bool tangents = track->interpolation == BR_ANIM_CUBIC || track->interpolation == BR_ANIM_BEZIER;
    uint32_t stride = tangents ? c * 3 : c; /* floats per key */
    uint32_t value = tangents ? c : 0;      /* the value inside a key (after the in-tangent) */
    const float *times = track->times;

    if (n == 1 || t <= times[0] || t >= times[n - 1]) {
        uint32_t k = (n > 1 && t >= times[n - 1]) ? n - 1 : 0;
        SDL_memcpy(out, track->values + k * stride + value, sizeof(float) * c);
        if (quaternion && c == 4) {
            br_anim_normalize4(out);
        }
        return;
    }
    uint32_t lo = br_anim_segment(times, n, t, key_hint);
    float dt = times[lo + 1] - times[lo];
    float u = dt > 0.0f ? (t - times[lo]) / dt : 0.0f;
    const float *a = track->values + lo * stride;
    const float *b = a + stride;

    if (track->interpolation == BR_ANIM_STEP) {
        SDL_memcpy(out, a + value, sizeof(float) * c);
    } else if (track->interpolation == BR_ANIM_CUBIC) {
        /* p0 = value(lo), m0 = out-tangent(lo) * dt, p1 = value(hi), m1 = in-tangent(hi) * dt */
        for (uint32_t i = 0; i < c; i++) {
            out[i] = br_spline_hermitef(a[c + i], a[2 * c + i] * dt, b[c + i], b[i] * dt, u);
        }
    } else if (track->interpolation == BR_ANIM_BEZIER) {
        /* Bezier p0 p1 p2 p3 is the Hermite curve p0 -> p3 with tangents 3(p1 - p0), 3(p3 - p2) */
        if (track->ease != BR_EASE_LINEAR) {
            u = br_ease_apply(track->ease, u);
        }
        for (uint32_t i = 0; i < c; i++) {
            float p0 = a[c + i];
            float p1 = a[2 * c + i];
            float p2 = b[i];
            float p3 = b[c + i];
            out[i] = br_spline_hermitef(p0, 3.0f * (p1 - p0), p3, 3.0f * (p3 - p2), u);
        }
    } else {
        if (track->ease != BR_EASE_LINEAR) {
            u = br_ease_apply(track->ease, u);
        }
        if (quaternion && c == 4) {
            br_quat qa = { a[0], a[1], a[2], a[3] };
            br_quat qb = { b[0], b[1], b[2], b[3] };
            br_quat q = br_quat_slerp(qa, qb, u); /* takes the short way round */
            out[0] = q.x;
            out[1] = q.y;
            out[2] = q.z;
            out[3] = q.w;
        } else {
            for (uint32_t i = 0; i < c; i++) {
                out[i] = a[i] + (b[i] - a[i]) * u;
            }
        }
    }
    if (quaternion && c == 4) {
        br_anim_normalize4(out);
    }
}

/* ---- clip sources */

br_anim_clip *br_anim_clip_from_frames(const char *name, const br_anim_frame *frames, uint32_t count)
{
    br_anim_clip *clip = br_anim_clip_create(name, 0.0f, BR_ANIM_LOOP);
    float *times = br_alloc(sizeof(float) * (count > 0 ? count : 1));
    float *values = br_alloc(sizeof(float) * (count > 0 ? count : 1));
    uint32_t keys = 0;
    float t = 0.0f;
    for (uint32_t i = 0; clip != NULL && times != NULL && values != NULL && i < count; i++) {
        if (frames[i].duration <= 0.0f) {
            continue; /* like the animated sprite: frames without a duration are skipped */
        }
        times[keys] = t;
        values[keys] = (float)br_anim_clip_add_texture(clip, frames[i].texture);
        keys++;
        t += frames[i].duration;
    }
    bool ok = clip != NULL && keys > 0 &&
              br_anim_clip_add_track(clip, "sprite.texture", BR_ANIM_STEP, 1, keys, times, values) != NULL;
    br_free(times);
    br_free(values);
    if (!ok) {
        br_log_error("br_anim_clip_from_frames: '%s' has no frame with a duration", name != NULL ? name : "");
        br_anim_clip_destroy(clip);
        return NULL;
    }
    clip->duration = t;
    return clip;
}

br_anim_clip *br_anim_clip_from_spritesheet(br_engine *e, const br_spritesheet *sheet, const char *animation,
                                            float fps)
{
    const br_texture *textures = NULL;
    uint32_t count = sheet != NULL && animation != NULL ? br_spritesheet_animation(e, sheet, animation, &textures) : 0;
    if (count == 0 || fps <= 0.0f) {
        br_log_error("br_anim_clip_from_spritesheet: no animation '%s' (or fps <= 0)", animation != NULL ? animation : "");
        return NULL;
    }
    br_anim_frame *frames = br_alloc(sizeof(br_anim_frame) * count);
    if (frames == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        frames[i].texture = textures[i];
        frames[i].duration = 1.0f / fps;
    }
    br_anim_clip *clip = br_anim_clip_from_frames(animation, frames, count);
    br_free(frames);
    return clip;
}

br_anim_clip *br_anim_clip_from_gltf(const br_model *model, uint32_t index)
{
    if (model == NULL || index >= model->clip_count) {
        return NULL;
    }
    static const char *const paths[4] = { "position", "rotation", "scale", "morph_weights" };
    const br_anim3d_clip *src = &model->clips[index];
    br_anim_clip *clip = br_anim_clip_create(src->name, src->duration, BR_ANIM_LOOP);
    for (uint32_t i = 0; clip != NULL && i < src->channel_count; i++) {
        const br_anim3d_channel *ch = &src->channels[i];
        br_anim_interpolation interp = BR_ANIM_LINEAR;
        if (ch->interpolation == BR_ANIM3D_STEP) {
            interp = BR_ANIM_STEP;
        } else if (ch->interpolation == BR_ANIM3D_CUBIC_SPLINE) {
            interp = BR_ANIM_CUBIC;
        }
        char path[64];
        SDL_snprintf(path, sizeof(path), "#%u:%s", ch->target, paths[ch->path]);
        uint32_t components = SDL_min(ch->components, (uint32_t)BR_ANIM_MAX_COMPONENTS);
        if (ch->key_count > 0 && components > 0) {
            br_anim_clip_add_track_borrowed(clip, path, interp, components, ch->key_count, ch->times, ch->values);
        }
    }
    if (clip != NULL) {
        clip->duration = src->duration;
    }
    return clip;
}

/* ---- *.anim.json */

bool br_anim_json_number(const void *json, float *out)
{
    const cJSON *v = json;
    /* NaN fails both comparisons; 1e999 parses as infinity */
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= -1.0e30 && v->valuedouble <= 1.0e30)) {
        return false;
    }
    *out = (float)v->valuedouble;
    return true;
}

uint32_t br_anim_json_value(br_engine *e, br_anim_clip *clip, const void *json, float *out, uint32_t max)
{
    const cJSON *v = json;
    if (cJSON_IsNumber(v)) {
        return br_anim_json_number(v, out) ? 1 : 0;
    }
    if (cJSON_IsBool(v)) {
        out[0] = cJSON_IsTrue(v) ? 1.0f : 0.0f;
        return 1;
    }
    if (cJSON_IsArray(v)) {
        uint32_t n = (uint32_t)cJSON_GetArraySize(v);
        if (n == 0 || n > max) {
            return 0;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (!br_anim_json_number(cJSON_GetArrayItem(v, (int)i), &out[i])) {
                return 0;
            }
        }
        return n;
    }
    if (cJSON_IsString(v)) {
        br_color c;
        if (max >= 4 && br_color_parse(v->valuestring, &c)) {
            out[0] = c.r;
            out[1] = c.g;
            out[2] = c.b;
            out[3] = c.a;
            return 4;
        }
        if (clip != NULL) {
            out[0] = (float)br_anim_clip_add_texture(clip, br_data_texture(e, v->valuestring));
            return 1;
        }
    }
    return 0;
}

typedef struct br_anim_names {
    const char *name;
    int         value;
} br_anim_names;

static const br_anim_names br_anim_interp_names[] = {
    { "step", BR_ANIM_STEP }, { "linear", BR_ANIM_LINEAR }, { "cubic", BR_ANIM_CUBIC }, { "bezier", BR_ANIM_BEZIER },
};
static const br_anim_names br_anim_loop_names[] = {
    { "loop", BR_ANIM_LOOP }, { "once", BR_ANIM_ONCE }, { "ping_pong", BR_ANIM_PING_PONG }, { "clamp", BR_ANIM_CLAMP },
};

static bool br_anim_name_lookup(const br_anim_names *table, uint32_t count, const char *name, int *out)
{
    for (uint32_t i = 0; name != NULL && i < count; i++) {
        if (SDL_strcmp(table[i].name, name) == 0) {
            *out = table[i].value;
            return true;
        }
    }
    return false;
}

bool br_anim_loop_from_name(const char *name, br_anim_loop *out)
{
    int v = 0;
    if (!br_anim_name_lookup(br_anim_loop_names, BR_ARRAY_COUNT(br_anim_loop_names), name, &v)) {
        return false;
    }
    *out = (br_anim_loop)v;
    return true;
}

static const char *br_anim_json_string(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* One track; errors are logged with the JSON path and the track is skipped. */
static void br_anim_json_track(br_engine *e, const char *file, br_anim_clip *clip, const cJSON *jt, const char *where)
{
    const char *target = br_anim_json_string(jt, "target");
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(jt, "keys");
    if (target == NULL || !cJSON_IsArray(keys) || cJSON_GetArraySize(keys) == 0) {
        br_log_error("%s: %s: a track needs \"target\" and a non-empty \"keys\" array", file, where);
        return;
    }
    int interp = BR_ANIM_LINEAR;
    const char *iname = br_anim_json_string(jt, "interpolation");
    if (iname != NULL && !br_anim_name_lookup(br_anim_interp_names, BR_ARRAY_COUNT(br_anim_interp_names), iname, &interp)) {
        br_log_error("%s: %s.interpolation: '%s' is not step, linear, cubic or bezier", file, where, iname);
        return;
    }
    br_ease_type ease = BR_EASE_LINEAR;
    const char *ename = br_anim_json_string(jt, "ease");
    if (ename != NULL) {
        bool ok = false;
        ease = br_ease_from_name(ename, &ok);
        if (!ok) {
            br_log_warn("%s: %s.ease: unknown easing '%s' (linear used)", file, where, ename);
        }
    }
    uint32_t n = (uint32_t)cJSON_GetArraySize(keys);
    if (n > BR_ANIM_JSON_MAX_KEYS) {
        br_log_error("%s: %s.keys: %u keys, at most %d", file, where, n, BR_ANIM_JSON_MAX_KEYS);
        return;
    }
    uint32_t per_key = br_anim_floats_per_key((br_anim_interpolation)interp, BR_ANIM_MAX_COMPONENTS);
    float *times = br_alloc(sizeof(float) * n);
    float *values = br_alloc(sizeof(float) * n * per_key);
    uint32_t components = 0;
    bool ok = times != NULL && values != NULL;
    for (uint32_t k = 0; ok && k < n; k++) {
        const cJSON *key = cJSON_GetArrayItem(keys, (int)k);
        const cJSON *jtime = NULL;
        const cJSON *jvalue = NULL;
        const cJSON *jin = NULL;
        const cJSON *jout = NULL;
        if (cJSON_IsArray(key) && cJSON_GetArraySize(key) == 2) {
            jtime = cJSON_GetArrayItem(key, 0);
            jvalue = cJSON_GetArrayItem(key, 1);
        } else if (cJSON_IsObject(key)) {
            jtime = cJSON_GetObjectItemCaseSensitive(key, "time");
            jvalue = cJSON_GetObjectItemCaseSensitive(key, "value");
            jin = cJSON_GetObjectItemCaseSensitive(key, "in");
            jout = cJSON_GetObjectItemCaseSensitive(key, "out");
        }
        float v[BR_ANIM_MAX_COMPONENTS];
        uint32_t c = jvalue != NULL ? br_anim_json_value(e, clip, jvalue, v, BR_ANIM_MAX_COMPONENTS) : 0;
        if (!br_anim_json_number(jtime, &times[k]) || c == 0 || (components != 0 && c != components)) {
            br_log_error("%s: %s.keys[%u]: expected [time, value] or {\"time\", \"value\"} with %s", file, where, k,
                         components != 0 ? "the same number of values as the first key" : "a number, array or string");
            ok = false;
            break;
        }
        if (k > 0 && times[k] < times[k - 1]) {
            br_log_error("%s: %s.keys[%u]: times must ascend", file, where, k);
            ok = false;
            break;
        }
        components = c;
        float *dst = values + k * br_anim_floats_per_key((br_anim_interpolation)interp, c);
        if (interp == BR_ANIM_CUBIC || interp == BR_ANIM_BEZIER) {
            float in[BR_ANIM_MAX_COMPONENTS];
            float out[BR_ANIM_MAX_COMPONENTS];
            /* defaults: flat tangents (cubic) / handles on the key (bezier) */
            for (uint32_t i = 0; i < c; i++) {
                in[i] = interp == BR_ANIM_CUBIC ? 0.0f : v[i];
                out[i] = in[i];
            }
            if ((jin != NULL && br_anim_json_value(e, NULL, jin, in, c) != c) ||
                (jout != NULL && br_anim_json_value(e, NULL, jout, out, c) != c)) {
                br_log_error("%s: %s.keys[%u]: \"in\" / \"out\" need as many values as \"value\"", file, where, k);
                ok = false;
                break;
            }
            SDL_memcpy(dst, in, sizeof(float) * c);
            SDL_memcpy(dst + c, v, sizeof(float) * c);
            SDL_memcpy(dst + 2 * c, out, sizeof(float) * c);
        } else {
            SDL_memcpy(dst, v, sizeof(float) * c);
        }
    }
    if (ok) {
        br_anim_track *t = br_anim_clip_add_track(clip, target, (br_anim_interpolation)interp, components, n, times, values);
        if (t != NULL) {
            t->ease = ease;
        }
    }
    br_free(times);
    br_free(values);
}

static br_anim_clip *br_anim_json_clip(br_engine *e, const char *file, const cJSON *jc, uint32_t index)
{
    if (!cJSON_IsObject(jc)) {
        br_log_error("%s: clips[%u]: expected a clip object", file, index);
        return NULL;
    }
    const char *name = br_anim_json_string(jc, "name");
    const cJSON *jd = cJSON_GetObjectItemCaseSensitive(jc, "duration");
    br_anim_loop loop = BR_ANIM_LOOP;
    const char *lname = br_anim_json_string(jc, "loop");
    if (lname != NULL && !br_anim_loop_from_name(lname, &loop)) {
        br_log_error("%s: clips[%u].loop: '%s' is not loop, once, ping_pong or clamp", file, index, lname);
    }
    float duration = 0.0f;
    if (jd != NULL && !br_anim_json_number(jd, &duration)) {
        br_log_error("%s: clips[%u].duration: expected a number of seconds (the last key time is used)", file, index);
    }
    br_anim_clip *clip = br_anim_clip_create(name != NULL ? name : "", duration, loop);
    if (clip == NULL) {
        return NULL;
    }
    const cJSON *tracks = cJSON_GetObjectItemCaseSensitive(jc, "tracks");
    for (int i = 0; i < cJSON_GetArraySize(tracks); i++) {
        char where[64];
        SDL_snprintf(where, sizeof(where), "clips[%u].tracks[%d]", index, i);
        br_anim_json_track(e, file, clip, cJSON_GetArrayItem(tracks, i), where);
    }
    const cJSON *events = cJSON_GetObjectItemCaseSensitive(jc, "events");
    for (int i = 0; i < cJSON_GetArraySize(events); i++) {
        const cJSON *ev = cJSON_GetArrayItem(events, i);
        float time = 0.0f;
        const char *ename = br_anim_json_string(ev, "name");
        if (!br_anim_json_number(cJSON_GetObjectItemCaseSensitive(ev, "time"), &time) || ename == NULL) {
            br_log_error("%s: clips[%u].events[%d]: expected { \"time\": seconds, \"name\": \"...\" }", file, index, i);
            continue;
        }
        br_anim_clip_add_marker(clip, time, ename);
    }
    return clip;
}

br_anim_library *br_anim_library_parse(br_engine *e, const char *name, const char *json, size_t size)
{
    const char *file = name != NULL ? name : "(anim)";
    cJSON *root = cJSON_ParseWithLength(json, size);
    if (root == NULL) {
        br_log_error("%s: not valid JSON", file);
        return NULL;
    }
    const cJSON *clips = cJSON_GetObjectItemCaseSensitive(root, "clips");
    bool single = !cJSON_IsArray(clips) && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "tracks"));
    uint32_t n = single ? 1 : (uint32_t)cJSON_GetArraySize(clips);
    br_anim_library *lib = br_alloc(sizeof(br_anim_library));
    if (lib == NULL || (!single && !cJSON_IsArray(clips))) {
        br_log_error("%s: expected { \"clips\": [ ... ] } or one clip with \"tracks\"", file);
        br_free(lib);
        cJSON_Delete(root);
        return NULL;
    }
    lib->clips = br_alloc(sizeof(br_anim_clip *) * (n > 0 ? n : 1));
    for (uint32_t i = 0; lib->clips != NULL && i < n; i++) {
        br_anim_clip *c = br_anim_json_clip(e, file, single ? root : cJSON_GetArrayItem(clips, (int)i), i);
        if (c != NULL) {
            lib->clips[lib->count++] = c;
        }
    }
    cJSON_Delete(root);
    return lib;
}

void br_anim_library_free(br_anim_library *lib)
{
    if (lib == NULL) {
        return;
    }
    for (uint32_t i = 0; i < lib->count; i++) {
        br_anim_clip_destroy(lib->clips[i]);
    }
    br_free(lib->clips);
    br_free(lib);
}

const br_anim_clip *br_anim_library_find(const br_anim_library *lib, const char *name)
{
    for (uint32_t i = 0; lib != NULL && name != NULL && i < lib->count; i++) {
        if (SDL_strcmp(lib->clips[i]->name, name) == 0) {
            return lib->clips[i];
        }
    }
    return NULL;
}

/* ---- the asset loader */

static bool br_anim_ends_with(const char *s, const char *suffix)
{
    size_t a = SDL_strlen(s);
    size_t b = SDL_strlen(suffix);
    return a >= b && SDL_strcmp(s + a - b, suffix) == 0;
}

static bool br_anim_library_test(const char *path, const void *bytes, size_t size)
{
    (void)bytes;
    (void)size;
    return br_anim_ends_with(path, ".anim.json");
}

static void *br_anim_library_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    return br_anim_library_parse(e, path, bytes, size);
}

static void br_anim_library_asset_unload(br_engine *e, void *asset)
{
    (void)e;
    br_anim_library_free(asset);
}

const br_anim_library *br_anim_library_load(br_engine *e, const char *path)
{
    br_asset_options opt;
    SDL_zero(opt);
    opt.type = "anim";
    return br_assets_load_with(e, path, &opt);
}

void br_anim_clip_register_loader(br_engine *e)
{
    br_asset_loader_desc d;
    SDL_zero(d);
    d.type_name = "anim";
    d.extensions = "json";
    d.test = br_anim_library_test;
    d.load = br_anim_library_asset_load;
    d.unload = br_anim_library_asset_unload;
    br_assets_register_loader(e, &d);
}
