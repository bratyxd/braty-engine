/* br_gltf_components: 3D components (the shared component registry, dense rows keyed by 3D
 * nodes), the "gltf.collider" component, and asset groups owned by 3D instances. */
#include "three_d/br_gltf_internal.h"

/* The 3D rows of a component id, created on first use (NULL for an unknown id). */
static br_component3d_set *br_component3d_set_get(br_engine *e, uint32_t component_id, bool create)
{
    br_gltf_state *s = e->gltf;
    const br_component_desc *desc = br_component_get_desc(e, component_id);
    if (s == NULL || desc == NULL) {
        return NULL;
    }
    if (component_id > s->components3d.count) {
        if (!create) {
            return NULL;
        }
        uint32_t old = s->components3d.count;
        if (!br_array_resize(&s->components3d, component_id)) {
            return NULL;
        }
        for (uint32_t i = old; i < component_id; i++) {
            br_component3d_set *set = br_array_at(&s->components3d, i);
            const br_component_desc *d = br_component_get_desc(e, i + 1);
            br_array_init(&set->data, (uint32_t)(d != NULL ? d->size : 1));
            br_array_init(&set->owners, sizeof(br_node3d));
            br_array_init(&set->sparse, sizeof(uint32_t));
        }
    }
    return br_array_at(&s->components3d, component_id - 1);
}

/* Dense row + 1 of a live node, 0 if it has no row. */
static uint32_t br_component3d_slot(const br_component3d_set *set, br_node3d n)
{
    const uint32_t *slot = br_array_at(&set->sparse, n.index);
    if (slot == NULL || *slot == 0) {
        return 0;
    }
    const br_node3d *owner = br_array_at(&set->owners, *slot - 1);
    return owner->generation == n.generation ? *slot : 0;
}

void *br_component3d_add(br_engine *e, br_node3d n, uint32_t component_id)
{
    br_component3d_set *set = br_component3d_set_get(e, component_id, true);
    if (set == NULL || !br_node3d_valid(e, n)) {
        br_log_warn("br_component3d_add: invalid 3D node or component %u", component_id);
        return NULL;
    }
    uint32_t slot = br_component3d_slot(set, n);
    if (slot != 0) {
        return br_array_at(&set->data, slot - 1);
    }
    if (set->sparse.count <= n.index && !br_array_resize(&set->sparse, n.index + 1)) {
        return NULL;
    }
    void *data = br_array_push(&set->data, NULL);
    if (data == NULL || br_array_push(&set->owners, &n) == NULL) {
        return NULL;
    }
    *(uint32_t *)br_array_at(&set->sparse, n.index) = set->data.count;
    return br_array_at(&set->data, set->data.count - 1);
}

void *br_component3d_get(br_engine *e, br_node3d n, uint32_t component_id)
{
    br_component3d_set *set = br_component3d_set_get(e, component_id, false);
    if (set == NULL || !br_node3d_valid(e, n)) {
        return NULL;
    }
    uint32_t slot = br_component3d_slot(set, n);
    return slot != 0 ? br_array_at(&set->data, slot - 1) : NULL;
}

/* Swap-remove of the row of node index `index` (any generation). */
static void br_component3d_remove_index(br_component3d_set *set, uint32_t index)
{
    uint32_t *slot = br_array_at(&set->sparse, index);
    if (slot == NULL || *slot == 0) {
        return;
    }
    uint32_t row = *slot - 1;
    uint32_t last = set->data.count - 1;
    if (row != last) {
        SDL_memcpy(br_array_at(&set->data, row), br_array_at(&set->data, last), set->data.elem_size);
        br_node3d moved = *(br_node3d *)br_array_at(&set->owners, last);
        *(br_node3d *)br_array_at(&set->owners, row) = moved;
        *(uint32_t *)br_array_at(&set->sparse, moved.index) = row + 1;
    }
    br_array_pop(&set->data);
    br_array_pop(&set->owners);
    *(uint32_t *)br_array_at(&set->sparse, index) = 0;
}

void br_component3d_remove(br_engine *e, br_node3d n, uint32_t component_id)
{
    br_component3d_set *set = br_component3d_set_get(e, component_id, false);
    if (set != NULL && br_component3d_slot(set, n) != 0) {
        br_component3d_remove_index(set, n.index);
    }
}

uint32_t br_component3d_count(br_engine *e, uint32_t component_id)
{
    br_component3d_set *set = br_component3d_set_get(e, component_id, false);
    return set != NULL ? set->data.count : 0;
}

void *br_component3d_at(br_engine *e, uint32_t component_id, uint32_t i)
{
    br_component3d_set *set = br_component3d_set_get(e, component_id, false);
    return set != NULL ? br_array_at(&set->data, i) : NULL;
}

br_node3d br_component3d_owner_at(br_engine *e, uint32_t component_id, uint32_t i)
{
    br_component3d_set *set = br_component3d_set_get(e, component_id, false);
    const br_node3d *owner = set != NULL ? br_array_at(&set->owners, i) : NULL;
    br_node3d none = { 0, 0 };
    return owner != NULL ? *owner : none;
}

uint32_t br_gltf_collider_component(br_engine *e)
{
    return e->gltf != NULL ? e->gltf->collider_id : 0;
}

bool br_gltf_name_has_convention(const char *name)
{
    static const char *const suffixes[] = { "-col", "-colonly", "-trigger", "-norender" };
    size_t len = name != NULL ? SDL_strlen(name) : 0;
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(suffixes); i++) {
        size_t n = SDL_strlen(suffixes[i]);
        if (len > n && SDL_strcmp(name + len - n, suffixes[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* A 3D node's memory is freed: drop its component rows and the asset group it owned. */
static void br_gltf_node3d_freed(br_engine *e, br_node3d n, void *user)
{
    (void)user;
    br_gltf_state *s = e->gltf;
    if (s == NULL) {
        return; /* shutdown: the assets module frees every group */
    }
    for (uint32_t i = 0; i < s->components3d.count; i++) {
        br_component3d_remove_index(br_array_at(&s->components3d, i), n.index);
    }
    br_gltf_owned *owned = br_array_at(&s->owned, n.index);
    if (owned != NULL && owned->generation == n.generation && owned->group != NULL) {
        br_asset_group *group = owned->group;
        SDL_zerop(owned);
        br_asset_group_destroy(group);
    }
}

void br_gltf_components_register(br_engine *e)
{
    br_gltf_state *s = e->gltf;
    br_array_init(&s->components3d, sizeof(br_component3d_set));
    br_array_init(&s->owned, sizeof(br_gltf_owned));
    static const br_field_desc fields[] = {
        { "center", BR_FIELD_VEC3, offsetof(br_gltf_collider, center) },
        { "size", BR_FIELD_VEC3, offsetof(br_gltf_collider, size) },
        { "trigger", BR_FIELD_BOOL, offsetof(br_gltf_collider, trigger) },
    };
    br_component_desc desc = { "gltf.collider", sizeof(br_gltf_collider), fields, BR_ARRAY_COUNT(fields) };
    s->collider_id = br_component_register(e, &desc);
    br_node3d_on_destroy(e, br_gltf_node3d_freed, NULL);
}

void br_gltf_components_free(br_engine *e)
{
    br_gltf_state *s = e->gltf;
    for (uint32_t i = 0; i < s->components3d.count; i++) {
        br_component3d_set *set = br_array_at(&s->components3d, i);
        br_array_free(&set->data);
        br_array_free(&set->owners);
        br_array_free(&set->sparse);
    }
    br_array_free(&s->components3d);
    br_array_free(&s->owned);
}
