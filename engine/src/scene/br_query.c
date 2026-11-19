/* br_query: multi-component iteration over the dense component arrays (see br_query.h). */
#include "braty/br_query.h"
#include "scene/br_scene_internal.h"

typedef enum br_query_op_kind {
    BR_QUERY_OP_ADD,
    BR_QUERY_OP_REMOVE,
    BR_QUERY_OP_DESTROY
} br_query_op_kind;

typedef struct br_query_op {
    br_query_op_kind kind;
    br_node          node;
    uint32_t         component_id;
    uint32_t         bytes_offset;   /* into op_bytes, ADD only */
    uint32_t         bytes_size;     /* 0 = start zeroed */
} br_query_op;

static br_component_entry *br_query_entry(br_engine *e, uint32_t component_id)
{
    if (component_id == 0 || e->scene == NULL || component_id > e->scene->components.count) {
        return NULL;
    }
    return (br_component_entry *)e->scene->components.data + (component_id - 1);
}

/* Data of node `index` in component c, NULL if it has none. */
static void *br_query_lookup(br_component_entry *c, uint32_t index)
{
    if (index >= c->sparse.count) {
        return NULL;
    }
    uint32_t slot = ((const uint32_t *)c->sparse.data)[index];
    if (slot == 0) {
        return NULL;
    }
    return (uint8_t *)c->data.data + (size_t)(slot - 1) * c->desc.size;
}

br_query br_query_begin(br_engine *e, const uint32_t *ids, uint32_t count)
{
    br_query q;
    SDL_zero(q);
    q.e = e;
    br_array_init(&q.ops, sizeof(br_query_op));
    br_array_init(&q.op_bytes, 1);
    if (ids == NULL || count == 0 || count > BR_QUERY_MAX_COMPONENTS) {
        br_log_error("br_query_begin: needs 1..%d component ids, got %u", BR_QUERY_MAX_COMPONENTS, count);
        return q;
    }
    uint32_t smallest = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        br_component_entry *c = br_query_entry(e, ids[i]);
        if (c == NULL) {
            br_log_error("br_query_begin: unknown component id %u", ids[i]);
            return q;
        }
        q.ids[i] = ids[i];
        if (c->data.count < smallest) {
            smallest = c->data.count;
            q.driver = i;
        }
    }
    q.count = count;
    q.driver_count = smallest;
    return q;
}

void br_query_without(br_query *q, uint32_t component_id)
{
    if (component_id != 0 && br_query_entry(q->e, component_id) == NULL) {
        br_log_error("br_query_without: unknown component id %u", component_id);
        return;
    }
    q->without = component_id;
}

uint32_t br_query_upper_bound(const br_query *q)
{
    return q->count > 0 ? q->driver_count : 0;
}

bool br_query_next(br_query *q)
{
    if (q->count == 0) {
        return false;
    }
    br_scene_state *s = q->e->scene;
    br_component_entry *driver = br_query_entry(q->e, q->ids[q->driver]);
    if (driver->data.count != q->driver_count) {
        br_log_error("br_query: component '%s' changed size during iteration; use br_query_defer_* (loop stopped)",
                     driver->name);
        q->count = 0;
        return false;
    }
    br_component_entry *without = q->without != 0 ? br_query_entry(q->e, q->without) : NULL;
    const br_node *owners = driver->owners.data;
    while (q->row < q->driver_count) {
        uint32_t row = q->row;
        q->row++;
        br_node n = owners[row];
        if (br_scene_record_at(s, n.index)->destroyed) {
            continue;
        }
        if (without != NULL && br_query_lookup(without, n.index) != NULL) {
            continue;
        }
        bool all = true;
        for (uint32_t i = 0; i < q->count; i++) {
            if (i == q->driver) {
                q->data[i] = (uint8_t *)driver->data.data + (size_t)row * driver->desc.size;
                continue;
            }
            q->data[i] = br_query_lookup((br_component_entry *)s->components.data + (q->ids[i] - 1), n.index);
            if (q->data[i] == NULL) {
                all = false;
                break;
            }
        }
        if (all) {
            q->node = n;
            return true;
        }
    }
    return false;
}

static void br_query_push_op(br_query *q, br_query_op_kind kind, br_node node, uint32_t component_id)
{
    br_query_op op;
    SDL_zero(op);
    op.kind = kind;
    op.node = node;
    op.component_id = component_id;
    br_array_push(&q->ops, &op);
}

void br_query_defer_add(br_query *q, br_node node, uint32_t component_id, const void *initial, size_t size)
{
    br_query_op op;
    SDL_zero(op);
    op.kind = BR_QUERY_OP_ADD;
    op.node = node;
    op.component_id = component_id;
    if (initial != NULL && size > 0) {
        op.bytes_offset = q->op_bytes.count;
        op.bytes_size = (uint32_t)size;
        if (br_array_push_n(&q->op_bytes, initial, (uint32_t)size) == NULL) {
            return;
        }
    }
    br_array_push(&q->ops, &op);
}

void br_query_defer_remove(br_query *q, br_node node, uint32_t component_id)
{
    br_query_push_op(q, BR_QUERY_OP_REMOVE, node, component_id);
}

void br_query_defer_destroy(br_query *q, br_node node)
{
    br_query_push_op(q, BR_QUERY_OP_DESTROY, node, 0);
}

void br_query_end(br_query *q)
{
    for (uint32_t i = 0; i < q->ops.count; i++) {
        const br_query_op *op = (const br_query_op *)q->ops.data + i;
        if (op->kind == BR_QUERY_OP_ADD) {
            void *data = br_component_add(q->e, op->node, op->component_id);
            const br_component_desc *desc = br_component_get_desc(q->e, op->component_id);
            if (data != NULL && op->bytes_size > 0) {
                size_t size = op->bytes_size < desc->size ? op->bytes_size : desc->size;
                SDL_memcpy(data, (const uint8_t *)q->op_bytes.data + op->bytes_offset, size);
            }
        } else if (op->kind == BR_QUERY_OP_REMOVE) {
            br_component_remove(q->e, op->node, op->component_id);
        } else {
            br_node_destroy(q->e, op->node);
        }
    }
    br_array_free(&q->ops);
    br_array_free(&q->op_bytes);
    q->count = 0;
}
