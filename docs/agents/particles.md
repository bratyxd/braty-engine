# particles - ParticleContainer (one instanced draw for up to millions of quads)

Header: `engine/include/braty/br_particles.h`. Source: `engine/src/particles/br_particles.c`,
shader `engine/shaders/particles.vert.hlsl` (+ `textured.frag.hlsl`). Example:
`examples/particles_demo.c` (`--count N --novsync`). Test: `tests/test_sprites.c`.

## What it does

A `"particle_container"` node owns a flat array of `br_particle` (x, y, scale, anchor, rotation,
tint, alpha, texture frame). Your systems edit the array in place. Rendering: one in-pass render
callback with its own pipeline; the vertex shader expands each instance into a quad (no vertex
buffer). Properties are grouped (`BR_PARTICLE_VERTEX`, `_POSITION`, `_ROTATION`, `_UVS`,
`_COLOR`): dynamic groups are packed into the frame's transient buffer every frame, static
groups live in a container-owned GPU buffer re-uploaded only when flagged dirty.

Measured (Release, RTX 3060 Ti, 1280x720, vsync off, POSITION+ROTATION dynamic, the demo's
update loop included): 100k = 0.56 ms/frame (D3D12) / 0.68 ms (Vulkan); 1M = 6.4 ms (D3D12) /
6.1 ms (Vulkan), of which ~3 ms is the demo's update loop and ~2.4 ms packing in collect.

## Functions you actually need

| Function | What for |
|---|---|
| `br_particle_container_create(e, tex, dynamic_mask)` | Container; frames must share tex's source. |
| `br_particle_container_add(e, n, count)` | Append particles (defaults: scale 1, white, alpha 1). |
| `br_particle_container_particles(e, n, &count)` | The array to edit each frame. |
| `br_particle_container_update(e, n)` | Re-upload static groups after you changed them. |
| `br_particle_container_set_dynamic` / `_remove` / `_clear` / `_set_texture` | The rest. |

## Snippet (from examples/particles_demo.c)

```c
br_node pc = br_particle_container_create(e, s->atlas, BR_PARTICLE_POSITION | BR_PARTICLE_ROTATION);
br_particle *p = br_particle_container_add(e, pc, (uint32_t)g_count);
for (int i = 0; p != NULL && i < g_count; i++) {
    p[i].x = rnd(s) * (float)w;
    p[i].y = rnd(s) * (float)h;
    p[i].scale_x = p[i].scale_y = 0.5f + rnd(s);
    p[i].anchor_x = p[i].anchor_y = 0.5f;
    p[i].texture = s->frames[i % FRAMES];
    p[i].tint = random_tint(s);
    p[i].alpha = 0.6f + 0.4f * rnd(s);
}
br_node_add_child(e, br_scene_root(e), pc);
```

## Gotchas

- Changing a static group without `br_particle_container_update` shows the old values.
  Make a group dynamic if it changes every frame (default: only POSITION).
- One texture source per container; particle textures are frames of it (rotated/trimmed OK).
- The container's transform, tint, alpha and blend mode apply to all particles; stencil masks
  work, scissor masks do not (`ponytail:` in br_particles.c: the callback info has no scissor).
- A container drawn twice in one frame (render-to-texture and stage) uses the last matrix for
  both (`ponytail:`).
- `local_bounds` is the AABB of positions (frame sizes ignored) and is O(n).
- Array pointers die on add/remove; re-fetch each frame.
- Particles do not appear in `br_render_get_stats` draw calls (callback draws are not counted).
