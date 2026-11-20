/* br_animation_internal.h - state shared by the animation module's files. */
#ifndef BR_ANIMATION_INTERNAL_H
#define BR_ANIMATION_INTERNAL_H

#include "app/br_engine_internal.h"
#include "braty/braty.h"

#define BR_ANIM_NONE UINT32_MAX
#define BR_ANIM_NAME_SIZE 32
#define BR_ANIMATOR_MAX_PARAMS 16
#define BR_ANIMATOR_MAX_QUEUE 4
#define BR_ANIMATOR_MAX_MASKS 4
#define BR_ANIM_TREE_MAX 8

/* A registered property row (the name is owned). */
typedef struct br_anim_property_row {
    br_anim_property_desc desc;
} br_anim_property_row;

/* What a resolved path writes: a registry row, or a component field (2D only). */
typedef struct br_anim_prop_ref {
    uint32_t           property;    /* registry id, BR_ANIM_NONE for a component field */
    uint32_t           component;   /* component id (component fields) */
    uint32_t           offset;      /* field offset in the component */
    br_anim_value_type type;
} br_anim_prop_ref;

/* One distinct (node, property) an animator writes. Values are blended here every frame. */
typedef struct br_anim_slot {
    br_anim_target     target;
    br_anim_prop_ref   prop;
    br_anim_set_fn     set;         /* the registry row's setter (NULL: component field) */
    uint32_t           count;       /* floats used */
    uint32_t           layer_mask;  /* bit L set: layer L may write it */
    bool               dead;        /* target destroyed: dropped (warned once) */
    bool               touched;     /* some layer wrote it this frame */
    bool               root;        /* root-motion slot */
    float              rest[BR_ANIM_MAX_COMPONENTS];   /* value when bound (blends start here) */
    br_anim_value      final;       /* blended value, handed to the setter as is */
    float              acc[BR_ANIM_MAX_COMPONENTS];    /* the current layer's blend */
    float              acc_weight;
    float              acc_max;     /* heaviest discrete contribution */
    br_texture         rest_texture;
    br_texture         acc_texture;
    float              faded[BR_ANIM_MAX_COMPONENTS];  /* the pose fading out (cross-fades) */
    br_texture         faded_texture;
    bool               has_faded;
    float              snap[BR_ANIM_MAX_COMPONENTS];   /* base layer pose frozen when a fade started from it */
    br_texture         snap_texture;
    bool               has_snap;
} br_anim_slot;

/* Tracks of one clip bound to one animator's slots (made once, at first play). */
typedef struct br_anim_bound {
    const br_anim_clip *clip;
    uint32_t           *slots;      /* per track: slot index, BR_ANIM_NONE = dropped */
    uint32_t           *hints;      /* per track: last key segment */
    bool                bound;
} br_anim_bound;

/* A blend tree: child clips weighted by one or two parameters. */
typedef struct br_anim_tree {
    char     name[BR_ANIM_NAME_SIZE];
    uint32_t dims;                    /* 1 or 2 */
    uint32_t param[2];                /* parameter indices */
    uint32_t clips[BR_ANIM_TREE_MAX]; /* animator clip indices */
    float    pos[BR_ANIM_TREE_MAX][2];
    float    weights[BR_ANIM_TREE_MAX]; /* this frame */
    uint32_t count;
} br_anim_tree;

/* A motion playing on a layer. motion >= 0: clip index; motion < 0: tree -(motion + 1). */
typedef struct br_anim_player {
    bool         active;
    int32_t      motion;
    float        time;         /* clip seconds; trees: normalised 0..1 */
    float        speed;        /* motion speed (state speed), times the animator's */
    br_anim_loop loop;
    float        dir;          /* +1 / -1 (ping-pong direction) */
    bool         fresh;        /* not ticked yet: a marker at the start time fires */
    bool         ended;        /* ONCE / CLAMP reached the end */
    bool         wrapped;      /* LOOP / PING_PONG turned around in the last tick */
    bool         ended_now;    /* reached the end in the last tick */
} br_anim_player;

typedef struct br_anim_queued {
    int32_t motion;
    float   fade;
} br_anim_queued;

typedef struct br_anim_layer {
    float          weight;
    bool           additive;
    br_anim_target masks[BR_ANIMATOR_MAX_MASKS];
    uint32_t       mask_count;
    br_anim_player cur;
    br_anim_player prev;            /* fading out */
    bool           snap;            /* fading out of the slots' snapshot instead of prev (layer 0) */
    float          fade_elapsed;
    float          fade_duration;
    br_anim_queued queue[BR_ANIMATOR_MAX_QUEUE];
    uint32_t       queue_count;
} br_anim_layer;

typedef enum br_anim_param_type { BR_ANIM_PARAM_FLOAT, BR_ANIM_PARAM_BOOL, BR_ANIM_PARAM_TRIGGER } br_anim_param_type;

typedef struct br_anim_param {
    char               name[BR_ANIM_NAME_SIZE];
    br_anim_param_type type;
    float              value;
    bool               typed;     /* false: made for a graph condition; the first setter decides the type */
    bool               warned;    /* a setter of another type was reported */
} br_anim_param;

/* One animator (dense pool entry). */
typedef struct br_animator_data {
    br_anim_target         root;
    uint32_t               handle;        /* br_pool slot */
    uint32_t               handle_generation;
    br_array               clips;         /* const br_anim_clip * */
    br_array               bounds;        /* br_anim_bound, parallel to clips */
    br_array               slots;         /* br_anim_slot */
    br_array               trees;         /* br_anim_tree */
    br_anim_layer          layers[BR_ANIMATOR_MAX_LAYERS];
    uint32_t               layer_count;
    br_anim_param          params[BR_ANIMATOR_MAX_PARAMS];
    uint32_t               param_count;
    float                  speed;
    bool                   paused;
    const br_anim_graph   *graph;
    int32_t                state;
    int32_t               *state_motion;  /* per graph state: motion index */
    uint32_t              *cond_params;   /* per graph condition: parameter index */
    bool                   root_motion;
    br_anim_target         root_node;
    br_vec3                root_delta;
    br_array               owned_assets;  /* char *: asset paths this animator holds a reference to */
    br_anim_event_fn       on_event;
    void                  *event_user;
    const br_model        *model;         /* model clip cache reference, NULL if none */
    uint32_t               destroy_epoch; /* s->destroy_epoch when the targets were last checked */
} br_animator_data;

/* glTF clips converted once per model, shared by every animator using them. */
typedef struct br_anim_model_clips {
    const br_model *model;
    br_anim_clip  **clips;
    uint32_t        count;
    uint32_t        refs;
} br_anim_model_clips;

/* One tween (dense array entry). */
typedef struct br_tween_data {
    uint32_t         handle;
    uint32_t         handle_generation;
    br_anim_target   target;          /* kind NONE: a callback-only tween */
    br_anim_target   owner;           /* the node the path was given relative to (kill_target) */
    br_anim_prop_ref prop;
    br_anim_set_fn   set;             /* the registry row's setter (NULL: component field) */
    uint32_t         count;
    float            from[BR_ANIM_MAX_COMPONENTS];
    float            to[BR_ANIM_MAX_COMPONENTS];
    bool             has_from;        /* from given (from / from_to) */
    bool             has_to;          /* to given (to / from_to) */
    bool             started;
    bool             dead;
    bool             yoyo;
    float            delay;
    float            duration;
    float            elapsed;         /* in the current pass */
    int32_t          repeat;          /* passes left after this one, -1 = forever */
    uint32_t         pass;
    br_ease_type     ease;
    uint32_t         tag;
    br_tween_fn      on_complete;
    void            *user;
} br_tween_data;

typedef struct br_animation_state {
    br_array properties;      /* br_anim_property_row */
    br_array animators;       /* br_animator_data, dense (swap-remove) */
    br_pool  handles;         /* uint32_t: dense index of each animator */
    br_array by_node2d;       /* uint32_t per 2D node index: handle slot + 1 (0 = none) */
    br_array by_node3d;       /* same for 3D nodes */
    br_array events;          /* br_anim_event of the last update */
    br_array model_clips;     /* br_anim_model_clips */
    /* freed at the start of the next update, so event names of the last one stay valid */
    br_array dead_model_clips; /* br_anim_model_clips without users */
    br_array dead_assets;     /* char *: asset paths to unload */
    br_array tweens;          /* br_tween_data, dense */
    br_pool  tween_handles;   /* uint32_t: dense index of each tween */
    uint32_t animator_component;
    uint32_t tween_component;
    uint32_t stat_animators;
    uint32_t stat_tracks;
    bool     updating;        /* inside the animator loop: removals are deferred */
    uint32_t destroy_epoch;   /* bumped whenever a node dies: animators re-check their targets */
    uint32_t tween_epoch;     /* destroy_epoch when the tweens last checked their targets */
} br_animation_state;

/* ---- br_animation.c */
/* The module state, made on first use (modules registered before this one can add properties). */
br_animation_state *br_anim_state(br_engine *e);

/* ---- br_anim_property.c */
void br_anim_property_register_builtins(br_engine *e);
/* Resolves "<node path>:<property>" below root. Logs nothing. */
bool br_anim_resolve(br_engine *e, br_anim_target root, const char *path, br_anim_target *node,
                     br_anim_prop_ref *prop);
/* Reads / writes a resolved property. */
void br_anim_prop_get(br_engine *e, br_anim_target t, const br_anim_prop_ref *prop, br_anim_value *out);
void br_anim_prop_set(br_engine *e, br_anim_target t, const br_anim_prop_ref *prop, const br_anim_value *value);
/* True if a target is `ancestor` or below it. */
bool br_anim_target_is_under(br_engine *e, br_anim_target t, br_anim_target ancestor);
bool br_anim_target_equal(br_anim_target a, br_anim_target b);

/* ---- br_anim_clip.c */
void br_anim_clip_register_loader(br_engine *e);
/* Reads one JSON value (number, array, "#colour", texture reference) into floats. */
uint32_t br_anim_json_value(br_engine *e, br_anim_clip *clip, const void *json, float *out, uint32_t max);
/* True (and *out set) if json is a number a float can hold (finite, |x| <= 1e30). */
bool br_anim_json_number(const void *json, float *out);
/* "loop" / "once" / "ping_pong" / "clamp". */
bool br_anim_loop_from_name(const char *name, br_anim_loop *out);

/* ---- br_animator.c */
void br_animator_system(br_engine *e, float dt, void *user);
br_animator_data *br_animator_data_get(br_engine *e, br_animator a);
void br_animator_free_all(br_engine *e);
/* Index of a clip or tree by name in the animator's motion space, or INT32_MIN if none. */
int32_t br_animator_find_motion(br_animator_data *d, const char *name);
/* Motion index of a blend tree by name (clips are not searched), INT32_MIN if none. */
int32_t br_animator_find_tree(br_animator_data *d, const char *name);
/* The animator takes over one reference to the asset at `path` (released when it is removed). */
void br_animator_own_asset(br_engine *e, br_animator a, const char *path);
/* Starts a motion on a layer (fade 0 = at once). */
bool br_animator_start(br_engine *e, br_animator_data *d, uint32_t layer, int32_t motion, float fade);
uint32_t br_animator_param_index(br_animator_data *d, const char *name, br_anim_param_type type, bool create);
void br_animator_on_node_destroy(br_engine *e, br_node n, void *user);
void br_animator_on_node3d_destroy(br_engine *e, br_node3d n, void *user);
/* Duration of a motion in seconds (trees: their weighted duration). */
float br_animator_motion_duration(br_animator_data *d, int32_t motion);

/* ---- br_animgraph.c */
void br_anim_graph_register_loader(br_engine *e);
/* Graph hooks used by the animator. */
bool br_anim_graph_attach(br_engine *e, br_animator_data *d);
void br_anim_graph_update(br_engine *e, br_animator_data *d);
const char *br_anim_graph_state_name(const br_anim_graph *g, int32_t state);

/* ---- br_tween.c */
void br_tween_system(br_engine *e, float dt, void *user);
void br_tween_free_all(br_engine *e);

/* ---- br_anim_components.c: "animator" and "tween" scene-file components */
void br_anim_components_register(br_engine *e);
void br_anim_components_system(br_engine *e, float dt, void *user);

/* ---- glTF instances (scene files, autoplay) */
/* The animator of a model3d node holding the model's clips, added on first use (zero handle if n
 * is not a model instance). */
br_animator br_animator_for_model(br_engine *e, br_node3d n);
/* Plays clip `index` on the base layer at once (glTF clips may be unnamed). */
bool br_animator_play_index(br_engine *e, br_animator a, uint32_t index);

#endif
