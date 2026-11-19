/* br_particles.h - ParticleContainer (PixiJS ParticleContainer + Particle): up to millions of
 * textured quads in one instanced draw.
 *
 * A container ("particle_container" node type) holds a flat array of br_particle. You edit
 * the array in place (br_particle_container_particles) from your own systems. Particles are
 * not nodes: no hierarchy, no events, no per-particle blend mode; the container's transform,
 * tint, alpha and blend mode apply to all of them.
 *
 * Properties are grouped (br_particle_property). Dynamic groups are uploaded every frame;
 * static groups only after br_particle_container_update (or when the particle count changes).
 * Default: only POSITION is dynamic. Make a group dynamic if you change it every frame.
 *
 * All particle textures must be frames of one source (the container texture's source):
 * one draw call, one texture. Rotated and trimmed atlas frames are fine.
 */
#ifndef BR_PARTICLES_H
#define BR_PARTICLES_H

#include "braty/br_render.h"

/* Registers the module (node types, loaders, systems). Called once by br_builtin.c. */
void br_particles_register(br_engine *e);

/* One particle (PixiJS Particle). */
typedef struct br_particle {
    float      x, y;
    float      scale_x, scale_y;
    float      anchor_x, anchor_y;   /* fraction of the frame size */
    float      rotation;             /* radians */
    uint32_t   tint;                 /* 0xRRGGBB */
    float      alpha;
    br_texture texture;              /* zero handle = the container's texture */
} br_particle;

/* Property groups, OR-ed into a dynamic mask. */
typedef enum br_particle_property {
    BR_PARTICLE_VERTEX   = 1 << 0,   /* scale, anchor, frame size */
    BR_PARTICLE_POSITION = 1 << 1,
    BR_PARTICLE_ROTATION = 1 << 2,
    BR_PARTICLE_UVS      = 1 << 3,   /* texture frame */
    BR_PARTICLE_COLOR    = 1 << 4    /* tint, alpha */
} br_particle_property;

/* Type id of particle containers. */
uint32_t br_particle_container_type(br_engine *e);
/* Creates a container drawing frames of `texture`'s source. dynamic: br_particle_property mask. */
br_node br_particle_container_create(br_engine *e, br_texture texture, uint32_t dynamic_properties);
/* Appends `count` particles (scale 1, alpha 1, white, container texture) and returns the first.
 * The pointer dies at the next add/remove. Static groups are re-uploaded. */
br_particle *br_particle_container_add(br_engine *e, br_node n, uint32_t count);
/* Removes `count` particles starting at `first` (order kept). */
void br_particle_container_remove(br_engine *e, br_node n, uint32_t first, uint32_t count);
/* Removes every particle. */
void br_particle_container_clear(br_engine *e, br_node n);
/* The particles to edit in place (NULL if not a container). */
br_particle *br_particle_container_particles(br_engine *e, br_node n, uint32_t *count);
/* Marks the static groups dirty: they are uploaded with the next frame (PixiJS update()). */
void br_particle_container_update(br_engine *e, br_node n);
/* Changes which groups are uploaded every frame. */
void br_particle_container_set_dynamic(br_engine *e, br_node n, uint32_t dynamic_properties);
/* Changes the container texture (static groups are re-uploaded). */
void br_particle_container_set_texture(br_engine *e, br_node n, br_texture texture);

#endif
