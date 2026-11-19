/* br_query.h - iterate the nodes that have several components at once.
 *
 *   uint32_t ids[] = { s->position_id, s->velocity_id };
 *   br_query q = br_query_begin(e, ids, 2);
 *   br_query_without(&q, s->frozen_id);              // optional: skip nodes that have this one
 *   while (br_query_next(&q)) {
 *       my_position *p = q.data[0];                    // data[i] belongs to ids[i]
 *       my_velocity *v = q.data[1];
 *       p->x += v->x * dt;
 *       if (p->x > 1000.0f) {
 *           br_query_defer_destroy(&q, q.node);        // applied by br_query_end
 *       }
 *   }
 *   br_query_end(&q);
 *
 * The query walks the smallest of the component arrays and looks the others up by node, so its
 * cost follows the rarest component. Nodes destroyed this frame (waiting for the flush) are
 * skipped.
 *
 * Structural changes (adding or removing components, destroying nodes) while iterating go
 * through br_query_defer_*: they are queued and applied in order by br_query_end, so data
 * pointers stay valid for the whole loop. Calling br_component_add / _remove / br_node_destroy
 * directly inside the loop is a bug: if it changes the array being walked, br_query_next logs an
 * error and stops the loop. data[] pointers are valid until the next br_query_next.
 */
#ifndef BR_QUERY_H
#define BR_QUERY_H

#include "braty/br_node.h"

#define BR_QUERY_MAX_COMPONENTS 4

typedef struct br_query {
    br_engine *e;
    uint32_t   ids[BR_QUERY_MAX_COMPONENTS];
    uint32_t   count;          /* components asked for (1..4); 0 = empty query, next returns false */
    uint32_t   without;        /* component a match must NOT have, 0 = none */
    uint32_t   driver;         /* index into ids of the array being walked */
    uint32_t   driver_count;   /* its row count at begin (changes are detected) */
    uint32_t   row;            /* next row to look at */
    br_node    node;           /* the current match */
    void      *data[BR_QUERY_MAX_COMPONENTS]; /* the current match's components, in ids order */
    br_array   ops;            /* deferred changes (allocated on first defer) */
    br_array   op_bytes;       /* initial data of deferred adds */
} br_query;

/* Starts a query over nodes having all `count` (1..4) components. Unknown ids give an empty query (logged). */
br_query br_query_begin(br_engine *e, const uint32_t *ids, uint32_t count);
/* Skips nodes that have `component_id`. Call before the first br_query_next. */
void br_query_without(br_query *q, uint32_t component_id);
/* Advances to the next match (q->node, q->data). False when done. */
bool br_query_next(br_query *q);
/* Number of nodes the query walks (the smallest array): an upper bound on the matches. */
uint32_t br_query_upper_bound(const br_query *q);
/* Queues br_component_add(node, id) and copies `size` bytes of `initial` into it (initial may be NULL). */
void br_query_defer_add(br_query *q, br_node node, uint32_t component_id, const void *initial, size_t size);
/* Queues br_component_remove(node, id). */
void br_query_defer_remove(br_query *q, br_node node, uint32_t component_id);
/* Queues br_node_destroy(node). */
void br_query_defer_destroy(br_query *q, br_node node);
/* Applies the queued changes in order and frees the query's memory. Always call it. */
void br_query_end(br_query *q);

#endif
