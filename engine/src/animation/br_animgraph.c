/* br_animgraph: state machines from *.animgraph.json - states (a clip or a blend tree),
 * transitions with conditions on float / bool / trigger parameters, exit time, blend duration. */
#include "animation/br_animation_internal.h"

#include "cjson/cJSON.h"

#define BR_ANIM_GRAPH_MAX_CONDS 4

typedef enum br_anim_op { BR_ANIM_OP_GT, BR_ANIM_OP_LT, BR_ANIM_OP_GE, BR_ANIM_OP_LE, BR_ANIM_OP_EQ, BR_ANIM_OP_NE, BR_ANIM_OP_SET } br_anim_op;

typedef struct br_anim_graph_cond {
    char       param[BR_ANIM_NAME_SIZE];
    br_anim_op op;
    float      value;
} br_anim_graph_cond;

typedef struct br_anim_graph_transition {
    int32_t            from;        /* state index, -1 = any state */
    int32_t            to;
    br_anim_graph_cond conds[BR_ANIM_GRAPH_MAX_CONDS];
    uint32_t           cond_count;
    float              exit_time;   /* normalised time of the current state, < 0 = none */
    float              duration;    /* cross-fade seconds */
} br_anim_graph_transition;

typedef struct br_anim_graph_state {
    char     name[BR_ANIM_NAME_SIZE];
    char     clip[64];              /* a clip, or "" for a blend tree */
    float    speed;
    int      loop;                  /* br_anim_loop, -1 = the clip's */
    uint32_t dims;                  /* blend tree: 1 or 2 */
    char     param[2][BR_ANIM_NAME_SIZE];
    char     clips[BR_ANIM_TREE_MAX][64];
    float    pos[BR_ANIM_TREE_MAX][2];
    uint32_t count;
} br_anim_graph_state;

typedef struct br_anim_graph_param {
    char               name[BR_ANIM_NAME_SIZE];
    br_anim_param_type type;
    float              value;
} br_anim_graph_param;

struct br_anim_graph {
    br_anim_graph_state      *states;
    uint32_t                  state_count;
    br_anim_graph_transition *transitions;
    uint32_t                  transition_count;
    br_anim_graph_param       params[BR_ANIMATOR_MAX_PARAMS];
    uint32_t                  param_count;
    int32_t                   entry;
};


static int32_t br_anim_graph_find(const br_anim_graph *g, const char *name)
{
    for (uint32_t i = 0; name != NULL && i < g->state_count; i++) {
        if (SDL_strcmp(g->states[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

const char *br_anim_graph_state_name(const br_anim_graph *g, int32_t state)
{
    return g != NULL && state >= 0 && (uint32_t)state < g->state_count ? g->states[state].name : NULL;
}

/* ---- parsing */

static const char *br_ag_string(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool br_ag_state(const char *file, const cJSON *js, uint32_t i, br_anim_graph_state *st)
{
    const char *name = br_ag_string(js, "name");
    if (name == NULL) {
        br_log_error("%s: states[%u]: a state needs a \"name\"", file, i);
        return false;
    }
    SDL_strlcpy(st->name, name, sizeof(st->name));
    const cJSON *speed = cJSON_GetObjectItemCaseSensitive(js, "speed");
    st->speed = cJSON_IsNumber(speed) ? (float)speed->valuedouble : 1.0f;
    st->loop = -1;
    br_anim_loop loop;
    const char *lname = br_ag_string(js, "loop");
    if (lname != NULL) {
        if (br_anim_loop_from_name(lname, &loop)) {
            st->loop = (int)loop;
        } else {
            br_log_error("%s: states[%u].loop: '%s' is not loop, once, ping_pong or clamp", file, i, lname);
        }
    }
    const char *clip = br_ag_string(js, "clip");
    if (clip != NULL) {
        SDL_strlcpy(st->clip, clip, sizeof(st->clip));
        return true;
    }
    const cJSON *blend = cJSON_GetObjectItemCaseSensitive(js, "blend");
    st->dims = 1;
    if (blend == NULL) {
        blend = cJSON_GetObjectItemCaseSensitive(js, "blend2d");
        st->dims = 2;
    }
    const cJSON *motions = cJSON_GetObjectItemCaseSensitive(blend, "motions");
    const char *px = br_ag_string(blend, st->dims == 1 ? "parameter" : "x");
    const char *py = br_ag_string(blend, "y");
    int n = cJSON_GetArraySize(motions);
    if (!cJSON_IsObject(blend) || px == NULL || (st->dims == 2 && py == NULL) || n < 1 || n > BR_ANIM_TREE_MAX) {
        br_log_error("%s: states[%u]: needs \"clip\", \"blend\": { \"parameter\", \"motions\" } or \"blend2d\": "
                     "{ \"x\", \"y\", \"motions\" } with 1..%d motions", file, i, BR_ANIM_TREE_MAX);
        return false;
    }
    SDL_strlcpy(st->param[0], px, sizeof(st->param[0]));
    SDL_strlcpy(st->param[1], py != NULL ? py : "", sizeof(st->param[1]));
    for (int k = 0; k < n; k++) {
        const cJSON *m = cJSON_GetArrayItem(motions, k);
        const char *mc = br_ag_string(m, "clip");
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(m, "at");
        bool ok = mc != NULL && (st->dims == 1 ? cJSON_IsNumber(at) : cJSON_GetArraySize(at) == 2);
        if (!ok) {
            br_log_error("%s: states[%u].motions[%d]: expected { \"clip\": name, \"at\": %s }", file, i, k,
                         st->dims == 1 ? "number" : "[x, y]");
            return false;
        }
        SDL_strlcpy(st->clips[k], mc, sizeof(st->clips[k]));
        if (st->dims == 1) {
            st->pos[k][0] = (float)at->valuedouble;
        } else {
            st->pos[k][0] = (float)cJSON_GetArrayItem(at, 0)->valuedouble;
            st->pos[k][1] = (float)cJSON_GetArrayItem(at, 1)->valuedouble;
        }
    }
    st->count = (uint32_t)n;
    return true;
}

static bool br_ag_cond(const char *file, const cJSON *jc, const char *where, br_anim_graph_cond *c)
{
    int n = cJSON_GetArraySize(jc);
    const cJSON *p = cJSON_GetArrayItem(jc, 0);
    if (!cJSON_IsArray(jc) || !cJSON_IsString(p) || (n != 1 && n != 3)) {
        br_log_error("%s: %s: a condition is [\"param\"] or [\"param\", \"op\", value]", file, where);
        return false;
    }
    SDL_strlcpy(c->param, p->valuestring, sizeof(c->param));
    c->op = BR_ANIM_OP_SET;
    c->value = 1.0f;
    if (n == 1) {
        return true;
    }
    static const char *const ops[] = { ">", "<", ">=", "<=", "==", "!=" };
    const cJSON *op = cJSON_GetArrayItem(jc, 1);
    const cJSON *v = cJSON_GetArrayItem(jc, 2);
    bool found = false;
    for (int i = 0; cJSON_IsString(op) && i < (int)BR_ARRAY_COUNT(ops); i++) {
        if (SDL_strcmp(op->valuestring, ops[i]) == 0) {
            c->op = (br_anim_op)i;
            found = true;
        }
    }
    if (!found || !(cJSON_IsNumber(v) || cJSON_IsBool(v))) {
        br_log_error("%s: %s: operator must be > < >= <= == != and the value a number or bool", file, where);
        return false;
    }
    c->value = cJSON_IsNumber(v) ? (float)v->valuedouble : (cJSON_IsTrue(v) ? 1.0f : 0.0f);
    return true;
}

br_anim_graph *br_anim_graph_parse(br_engine *e, const char *name, const char *json, size_t size)
{
    (void)e;
    const char *file = name != NULL ? name : "(animgraph)";
    cJSON *root = cJSON_ParseWithLength(json, size);
    if (root == NULL) {
        br_log_error("%s: not valid JSON", file);
        return NULL;
    }
    const cJSON *states = cJSON_GetObjectItemCaseSensitive(root, "states");
    const cJSON *transitions = cJSON_GetObjectItemCaseSensitive(root, "transitions");
    int sn = cJSON_GetArraySize(states);
    int tn = cJSON_GetArraySize(transitions);
    br_anim_graph *g = br_alloc(sizeof(br_anim_graph));
    if (g == NULL || !cJSON_IsArray(states) || sn == 0) {
        br_log_error("%s: a graph needs a non-empty \"states\" array", file);
        br_free(g);
        cJSON_Delete(root);
        return NULL;
    }
    g->states = br_alloc(sizeof(br_anim_graph_state) * (size_t)sn);
    g->transitions = br_alloc(sizeof(br_anim_graph_transition) * (size_t)(tn > 0 ? tn : 1));
    bool ok = g->states != NULL && g->transitions != NULL;
    for (int i = 0; ok && i < sn; i++) {
        if (br_ag_state(file, cJSON_GetArrayItem(states, i), (uint32_t)i, &g->states[g->state_count])) {
            g->state_count++;
        }
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "parameters");
    for (const cJSON *p = params != NULL ? params->child : NULL; p != NULL && g->param_count < BR_ANIMATOR_MAX_PARAMS; p = p->next) {
        br_anim_graph_param *gp = &g->params[g->param_count];
        SDL_strlcpy(gp->name, p->string, sizeof(gp->name));
        if (cJSON_IsNumber(p)) {
            gp->type = BR_ANIM_PARAM_FLOAT;
            gp->value = (float)p->valuedouble;
        } else if (cJSON_IsBool(p)) {
            gp->type = BR_ANIM_PARAM_BOOL;
            gp->value = cJSON_IsTrue(p) ? 1.0f : 0.0f;
        } else if (cJSON_IsString(p) && SDL_strcmp(p->valuestring, "trigger") == 0) {
            gp->type = BR_ANIM_PARAM_TRIGGER;
            gp->value = 0.0f;
        } else {
            br_log_error("%s: parameters.%s: expected a number, a bool or \"trigger\"", file, p->string);
            continue;
        }
        g->param_count++;
    }
    for (int i = 0; ok && i < tn; i++) {
        const cJSON *jt = cJSON_GetArrayItem(transitions, i);
        br_anim_graph_transition *t = &g->transitions[g->transition_count];
        SDL_zerop(t);
        const char *from = br_ag_string(jt, "from");
        const char *to = br_ag_string(jt, "to");
        t->from = from != NULL && SDL_strcmp(from, "*") == 0 ? -1 : br_anim_graph_find(g, from);
        t->to = br_anim_graph_find(g, to);
        if ((t->from < 0 && (from == NULL || SDL_strcmp(from, "*") != 0)) || t->to < 0) {
            br_log_error("%s: transitions[%d]: \"from\" (a state or \"*\") and \"to\" must name states", file, i);
            continue;
        }
        const cJSON *exit_time = cJSON_GetObjectItemCaseSensitive(jt, "exit_time");
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(jt, "duration");
        t->exit_time = cJSON_IsNumber(exit_time) ? (float)exit_time->valuedouble : -1.0f;
        t->duration = cJSON_IsNumber(duration) ? (float)duration->valuedouble : 0.25f;
        const cJSON *when = cJSON_GetObjectItemCaseSensitive(jt, "when");
        bool conds_ok = true;
        for (int k = 0; k < cJSON_GetArraySize(when) && conds_ok; k++) {
            char where[48];
            SDL_snprintf(where, sizeof(where), "transitions[%d].when[%d]", i, k);
            if (t->cond_count >= BR_ANIM_GRAPH_MAX_CONDS) {
                br_log_error("%s: %s: at most %d conditions", file, where, BR_ANIM_GRAPH_MAX_CONDS);
                conds_ok = false;
            } else {
                conds_ok = br_ag_cond(file, cJSON_GetArrayItem(when, k), where, &t->conds[t->cond_count]);
                t->cond_count += conds_ok ? 1 : 0;
            }
        }
        if (conds_ok) {
            g->transition_count++;
        }
    }
    const char *entry = br_ag_string(root, "entry");
    g->entry = entry != NULL ? br_anim_graph_find(g, entry) : 0;
    if (g->entry < 0 || g->state_count == 0) {
        br_log_error("%s: \"entry\" names no state", file);
        ok = false;
    }
    cJSON_Delete(root);
    if (!ok) {
        br_anim_graph_free(g);
        return NULL;
    }
    return g;
}

void br_anim_graph_free(br_anim_graph *graph)
{
    if (graph != NULL) {
        br_free(graph->states);
        br_free(graph->transitions);
        br_free(graph);
    }
}

/* ---- running a graph on an animator */

static void br_anim_graph_enter(br_engine *e, br_animator_data *d, int32_t state, float fade)
{
    const br_anim_graph_state *st = &d->graph->states[state];
    br_animator_start(e, d, 0, d->state_motion[state], fade);
    d->layers[0].cur.speed = st->speed;
    if (st->loop >= 0) {
        d->layers[0].cur.loop = (br_anim_loop)st->loop;
    }
    d->state = state;
}

bool br_anim_graph_attach(br_engine *e, br_animator_data *d)
{
    const br_anim_graph *g = d->graph;
    br_animator handle = { d->handle, d->handle_generation };
    for (uint32_t i = 0; i < g->param_count; i++) {
        uint32_t p = br_animator_param_index(d, g->params[i].name, g->params[i].type, true);
        if (p != BR_ANIM_NONE) {
            d->params[p].type = g->params[i].type;
            d->params[p].value = g->params[i].value;
        }
    }
    br_free(d->state_motion);
    br_free(d->cond_params);
    d->state_motion = br_alloc(sizeof(int32_t) * g->state_count);
    d->cond_params = br_alloc(sizeof(uint32_t) * BR_ANIM_GRAPH_MAX_CONDS * (g->transition_count > 0 ? g->transition_count : 1));
    if (d->state_motion == NULL || d->cond_params == NULL) {
        return false;
    }
    /* conditions read parameters by index every frame: resolve the names once */
    for (uint32_t i = 0; i < g->transition_count; i++) {
        for (uint32_t k = 0; k < g->transitions[i].cond_count; k++) {
            d->cond_params[i * BR_ANIM_GRAPH_MAX_CONDS + k] =
                br_animator_param_index(d, g->transitions[i].conds[k].param, BR_ANIM_PARAM_FLOAT, true);
        }
    }
    for (uint32_t i = 0; i < g->state_count; i++) {
        const br_anim_graph_state *st = &g->states[i];
        if (st->clip[0] == '\0' && br_animator_find_motion(d, st->name) == INT32_MIN) {
            const char *clips[BR_ANIM_TREE_MAX];
            float pos[BR_ANIM_TREE_MAX * 2];
            for (uint32_t k = 0; k < st->count; k++) {
                clips[k] = st->clips[k];
                pos[k * st->dims] = st->pos[k][0];
                if (st->dims == 2) {
                    pos[k * 2 + 1] = st->pos[k][1];
                }
            }
            bool ok;
            if (st->dims == 1) {
                ok = br_animator_add_blend1d(e, handle, st->name, st->param[0], clips, pos, st->count);
            } else {
                br_vec2 p2[BR_ANIM_TREE_MAX];
                for (uint32_t k = 0; k < st->count; k++) {
                    p2[k] = br_vec2_make(pos[k * 2], pos[k * 2 + 1]);
                }
                ok = br_animator_add_blend2d(e, handle, st->name, st->param[0], st->param[1], clips, p2, st->count);
            }
            if (!ok) {
                return false;
            }
        }
        int32_t m = br_animator_find_motion(d, st->clip[0] != '\0' ? st->clip : st->name);
        if (m == INT32_MIN) {
            br_log_error("animgraph: state '%s': the animator has no clip '%s'", st->name, st->clip);
            return false;
        }
        d->state_motion[i] = m;
    }
    br_anim_graph_enter(e, d, g->entry, 0.0f);
    return true;
}

static bool br_anim_graph_holds(const br_animator_data *d, const br_anim_graph_cond *c, uint32_t p)
{
    float v = p != BR_ANIM_NONE ? d->params[p].value : 0.0f;
    switch (c->op) {
    case BR_ANIM_OP_GT: return v > c->value;
    case BR_ANIM_OP_LT: return v < c->value;
    case BR_ANIM_OP_GE: return v >= c->value;
    case BR_ANIM_OP_LE: return v <= c->value;
    case BR_ANIM_OP_EQ: return v == c->value;
    case BR_ANIM_OP_NE: return v != c->value;
    default: return v >= 0.5f;
    }
}

void br_anim_graph_update(br_engine *e, br_animator_data *d)
{
    const br_anim_graph *g = d->graph;
    const br_anim_layer *l = &d->layers[0];
    if (d->state < 0 || l->prev.active) {
        return; /* no transition interrupts a running one */
    }
    for (uint32_t i = 0; i < g->transition_count; i++) {
        const br_anim_graph_transition *t = &g->transitions[i];
        if ((t->from >= 0 && t->from != d->state) || (t->from < 0 && t->to == d->state)) {
            continue;
        }
        if (t->exit_time >= 0.0f) {
            float normalised = 1.0f;
            if (!l->cur.ended && l->cur.active) {
                float dur = l->cur.motion >= 0 ? br_animator_motion_duration(d, l->cur.motion) : 1.0f;
                normalised = dur > 0.0f ? l->cur.time / dur : 1.0f;
            }
            if (normalised < t->exit_time) {
                continue;
            }
        }
        bool all = true;
        const uint32_t *params = &d->cond_params[i * BR_ANIM_GRAPH_MAX_CONDS];
        for (uint32_t k = 0; k < t->cond_count && all; k++) {
            all = br_anim_graph_holds(d, &t->conds[k], params[k]);
        }
        if (!all) {
            continue;
        }
        for (uint32_t k = 0; k < t->cond_count; k++) {
            uint32_t p = params[k];
            if (p != BR_ANIM_NONE && d->params[p].type == BR_ANIM_PARAM_TRIGGER) {
                d->params[p].value = 0.0f; /* triggers are consumed by the transition that used them */
            }
        }
        br_anim_graph_enter(e, d, t->to, t->duration);
        return;
    }
}

bool br_animator_set_graph(br_engine *e, br_animator a, const br_anim_graph *graph)
{
    br_animator_data *d = br_animator_data_get(e, a);
    if (d == NULL) {
        return false;
    }
    d->graph = graph;
    d->state = -1;
    if (graph == NULL) {
        return true;
    }
    if (!br_anim_graph_attach(e, d)) {
        d = br_animator_data_get(e, a);
        d->graph = NULL;
        return false;
    }
    return true;
}

const char *br_animator_state(br_engine *e, br_animator a)
{
    br_animator_data *d = br_animator_data_get(e, a);
    return d != NULL ? br_anim_graph_state_name(d->graph, d->state) : NULL;
}

/* ---- the asset loader */

static bool br_anim_graph_test(const char *path, const void *bytes, size_t size)
{
    (void)bytes;
    (void)size;
    const char *suffix = ".animgraph.json";
    size_t a = SDL_strlen(path);
    size_t b = SDL_strlen(suffix);
    return a >= b && SDL_strcmp(path + a - b, suffix) == 0;
}

static void *br_anim_graph_asset_load(br_engine *e, const char *path, const void *bytes, size_t size)
{
    return br_anim_graph_parse(e, path, bytes, size);
}

static void br_anim_graph_asset_unload(br_engine *e, void *asset)
{
    (void)e;
    br_anim_graph_free(asset);
}

const br_anim_graph *br_anim_graph_load(br_engine *e, const char *path)
{
    br_asset_options opt;
    SDL_zero(opt);
    opt.type = "animgraph";
    return br_assets_load_with(e, path, &opt);
}

void br_anim_graph_register_loader(br_engine *e)
{
    br_asset_loader_desc d;
    SDL_zero(d);
    d.type_name = "animgraph";
    d.extensions = "json";
    d.test = br_anim_graph_test;
    d.load = br_anim_graph_asset_load;
    d.unload = br_anim_graph_asset_unload;
    br_assets_register_loader(e, &d);
}
