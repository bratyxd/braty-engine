# scene - nodes, node types, components, transforms, bounds, culling

Headers: `engine/include/braty/br_node.h`, `br_sprite.h`. Source: `engine/src/scene/`.
Tests: `tests/test_node.c`, `test_components.c`, `test_transform.c`, `test_query.c`. Example: `examples/sprites.c`.

## The model

- A node is a handle `br_node { index, generation }` into the node table (PixiJS Container).
  Every node has hierarchy, transform (position, scale, rotation, skew, pivot, origin), alpha,
  tint, visible/renderable, z-index, blend mode, mask, effect slots and interaction fields.
- A node has a **type** (`br_node_type_desc`). Per-type data sits in a dense pool:
  `br_node_data(e, n, type_id)`. `BR_NODE_TYPE_CONTAINER` (1) has no data; `"sprite"` is built in.
- **Components** are user structs in dense arrays, iterated by systems.
- Only nodes under `br_stage_root(e)` (the stage) are updated and drawn each frame.
- Every call checks the handle: stale/destroyed handles are ignored (getters return defaults).

## Functions you actually need

| Function | What for |
|---|---|
| `br_node_create(e, type)`, `br_node_create_container(e)`, `br_sprite_create(e, tex)` | New detached node. |
| `br_node_add_child(e, parent, child)` / `_at` / `remove_child` / `reparent` | Hierarchy (Pixi Container API). |
| `br_node_set_position/scale/rotation/skew/pivot/origin` | Transform (radians, y down). |
| `br_node_to_global` / `br_node_to_local` / `br_node_world_transform` | Always current, computed from ancestors. |
| `br_node_set_alpha/tint/visible/z_index/sortable_children/blend_mode` | Appearance and order. |
| `br_node_get_bounds` / `get_local_bounds`, `br_node_set_cullable` | Bounds, culling. |
| `br_node_destroy(e, n)` | Invalid now; freed by the next flush (start of next frame, or at once outside a step). |
| `br_node_type_register(e, &desc)` | Add a node type (from your module's register function). |
| `br_component_register/add/get/remove/count/at/owner_at` | Components. |
| `br_query_begin(e, ids, count)`, `br_query_next(&q)`, `br_query_end(&q)` | Nodes that have 1-4 components at once (`br_query.h`). |

## Snippet: a node type (from examples/sprites.c, the "demo_wave" type)

```c
br_node_type_desc wave;
SDL_zero(wave);
wave.name = "demo_wave";              /* also the JSON "type" */
wave.data_size = sizeof(wave_data);   /* per-node data, zeroed, in a dense pool */
wave.render = wave_render;            /* see docs/agents/render.md */
s->wave_type = br_node_type_register(e, &wave);
br_node wn = br_node_create(e, s->wave_type);
wave_data *wd = br_node_data(e, wn, s->wave_type);
wd->texture = s->checker;
br_node_add_child(e, strip, wn);
```

## Snippet: a component + system (from examples/sprites.c)

```c
br_component_desc desc = { "bunny", sizeof(bunny), NULL, 0 };
s->bunny_component = br_component_register(e, &desc);
bunny *data = br_component_add(e, b, s->bunny_component);

static void bunny_system(br_engine *e, float dt, void *user)
{
    demo_state *s = user;
    uint32_t count = br_component_count(e, s->bunny_component);
    for (uint32_t i = 0; i < count; i++) {
        bunny *b = br_component_at(e, s->bunny_component, i);
        br_node n = br_component_owner_at(e, s->bunny_component, i);
        ...
    }
}
```

## Snippet: nodes with several components (`br_query.h`, from tests/test_query.c)

```c
/* a + b: even nodes; data[] follows the ids order whatever array drives */
uint32_t ab[] = { id.a, id.b };
br_query q = br_query_begin(e, ab, 2);
int visited = 0;
bool data_ok = true;
while (br_query_next(&q)) {
    comp_a *a = q.data[0];
    comp_b *b = q.data[1];
    data_ok = data_ok && b->value == a->value * 10.0f && br_component_get(e, q.node, id.a) == a;
    visited++;
}
br_query_end(&q);
```

The query walks the smallest of the component arrays and looks the others up, so it costs what
the rarest component costs. `br_query_without(&q, id)` skips nodes that have a component. Inside
the loop, add / remove components or destroy nodes only through `br_query_defer_add / _remove /
_destroy` (applied by `br_query_end`); direct changes to the walked array stop the loop with an
error.

Field descriptions (`br_field_desc { name, BR_FIELD_*, offsetof(...) }`) let scene files fill a
component; strings/asset paths are inline `char[BR_FIELD_STRING_SIZE]`.

## Engine systems (all registered by `br_scene_register`)

| System | Phase / priority | Does |
|---|---|---|
| `scene.flush_destroyed` | INPUT -1000000 | frees nodes destroyed last frame |
| `scene.transform` | PRE_RENDER -1000 | local/world matrices, world alpha/tint, dirty subtrees only |
| `scene.sort` | PRE_RENDER -900 | stable z-index sort of queued parents (and render layers) |
| `scene.cull` | PRE_RENDER -800 | culls `cullable` nodes outside the window, or outside the stage rect the cameras show when cameras are active (visits only those nodes) |

Camera hooks on nodes: `br_node_set_layer_bits(e, n, bits)` (0 = inherit the parent's, the stage
is layer 1) picks which cameras draw a node (`culling_mask`, `docs/agents/camera.md`).

## For other engine modules (extension points)

- **Per-node data of your module**: register an internal component (`br_component_register`
  with a `"<module>.<name>"` name) - dense, removed automatically when the node dies - or keep
  your own table indexed by `n.index` and check `n.generation`.
- **Cleanup when nodes die**: `br_node_on_destroy(e, fn, user)`; called at the flush with
  the (already invalid) handle. Type data is cleaned in `desc.on_destroy`.
- **Events (Task 7) read**: `br_node_event_mode`, `br_node_interactive_children`,
  `br_node_hit_area` + `br_hit_area_contains`, `br_node_cursor`, `br_node_type_hit_test`,
  `br_node_to_local`, `br_node_mask` (hit tests should respect masks), `br_node_child_at`
  (iterate in reverse for top-most first; skip `!br_node_valid` children).
- **Filters (Task 8) own**: `br_node_set_filters(e, n, ptr)` (opaque), `cache_as_texture`,
  `BR_MASK_ALPHA/COLOR`, blend modes `>= BR_BLEND_ADVANCED_FIRST`. All four route the node to the
  effects hook (`br_node_needs_effects`; see render.md). Free your filter list in a destroy listener.
- **Scene files (Task 10)**: `br_node_type_set_from_json(e, type_id, fn)` installs JSON parsing
  for any type; `br_component_get_desc` exposes field lists.
- Engine code may read the tables directly through `engine/src/scene/br_scene_internal.h`
  (read-only: setters maintain dirty flags).

## Gotchas

- Destroy is deferred inside `br_engine_step`: the handle is invalid at once, children lists and
  component arrays keep the dead entries until the next frame (so loops do not shift). Check
  `br_node_valid(e, br_component_owner_at(...))` when iterating components after destroying.
- Outside a step (init, tests) destroy is immediate: the flush runs inside `br_node_destroy`.
  One rule for callbacks either way: a `br_node_destroy` from an `on_destroy` or destroy listener
  marks the node and the running flush frees it before returning (the flush never re-enters), so
  a type that owns helper nodes may destroy them in `on_destroy`.
- The flush removes dead children from each live parent (and dead members from each layer) in one
  pass per list, so destroying thousands of children of one container is linear.
- Tree walks (transform update, bounds, culling, destroy, label search) use explicit stacks:
  hierarchies of any depth work. The render walk and pointer hit tests recurse and stop at 2048
  levels (logged once).
- Relations that would loop are rejected by the setters (logged): `br_node_set_mask` with `n`
  inside the mask's subtree, `br_node_layer_attach` with the layer inside the member's subtree.
  Rearranging the tree afterwards can still build such a loop: the renderer ignores masks met
  while drawing a mask, and the depth limit stops a layer loop.
- Render groups (`br_node_set_render_group`): descendants' cached world matrices are relative to
  the group. `br_node_world_transform`/`to_global` are always absolute.
- Masks: the mask node is not drawn as content and does not count in its parent's bounds. It is
  drawn when it is the top node of `br_render_subtree_to_texture` (alpha/colour masks, render.md).
- `br_node_child_count` includes children destroyed this frame until the flush;
  `br_node_count` does not (it counts live nodes).
- Component pointers die when the array grows: re-fetch after any create (prefab, scene load).
- Scene *objects* (load/unload/switch, persistent nodes) are the `scenes` module: `docs/agents/scenes.md`.
- Hot paths read the tables directly: transform internals cache cos/sin of the rotation
  (`br_scene_set_rotation` is the only writer) and the packed world colour (`world_color`, updated
  with world tint/alpha). Engine code that writes `rotation` or tint/alpha fields directly would
  leave them stale: use the setters.
