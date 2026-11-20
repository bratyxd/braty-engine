/* br_material3d.h - 3D materials: PBR metallic-roughness, Phong, Flat (unlit), Toon, custom.
 *
 * A material is a handle to a plain parameter struct (br_material3d_desc). Edit it live through
 * br_material3d_get: changes show on the next frame (animation writes here too).
 *
 * Colours are LINEAR. Textures: base colour and emissive are decoded from sRGB in the shader;
 * metallic-roughness (G = roughness, B = metallic, glTF layout), normal (tangent space) and
 * occlusion (R) are linear. Any br_texture works, render textures included (2D-in-3D); frame
 * textures are sampled over their whole source. Each slot has optional sampler state, a uv set
 * and a KHR_texture_transform (offset, rotation, scale).
 *
 * Custom materials: model BR_MATERIAL3D_CUSTOM + your fragment shader. It must include
 * engine/shaders/mesh3d_common.hlsli (vertex varyings + the frame/light cbuffer at b0),
 * read your uniform block at register(b1, space3) and your textures at t0.. / s0.. (space2),
 * and return premultiplied linear HDR colour. See docs/agents/three_d.md.
 */
#ifndef BR_MATERIAL3D_H
#define BR_MATERIAL3D_H

#include "braty/br_view3d.h"

/* Bytes of uniform data a custom material can carry. */
#define BR_MATERIAL3D_CUSTOM_UNIFORM_SIZE 256
/* Textures a custom material can bind. */
#define BR_MATERIAL3D_CUSTOM_TEXTURES 4

typedef enum br_material3d_model {
    BR_MATERIAL3D_PBR,      /* metallic-roughness (glTF) */
    BR_MATERIAL3D_PHONG,    /* Blinn-Phong: diffuse + specular colour + shininess */
    BR_MATERIAL3D_FLAT,     /* unlit: base colour + emissive */
    BR_MATERIAL3D_TOON,     /* banded diffuse + hard specular + rim */
    BR_MATERIAL3D_CUSTOM    /* your fragment shader */
} br_material3d_model;

typedef enum br_alpha_mode {
    BR_ALPHA_OPAQUE,        /* alpha ignored */
    BR_ALPHA_MASK,          /* discard below alpha_cutoff */
    BR_ALPHA_BLEND          /* transparent pass, sorted back to front */
} br_alpha_mode;

typedef enum br_material3d_slot {
    BR_MATERIAL3D_BASE_COLOR,
    BR_MATERIAL3D_METALLIC_ROUGHNESS,
    BR_MATERIAL3D_NORMAL,
    BR_MATERIAL3D_OCCLUSION,
    BR_MATERIAL3D_EMISSIVE,
    BR_MATERIAL3D_SLOT_COUNT
} br_material3d_slot;

/* One texture slot. A zero texture handle = no texture. */
typedef struct br_material3d_texture {
    br_texture       texture;
    bool             has_sampler;   /* false = use the texture's own style */
    br_texture_style sampler;       /* glTF sampler: filter, wrap, mipmaps */
    uint32_t         uv_set;        /* 0 = uv0, 1 = uv1 */
    br_vec2          offset;        /* KHR_texture_transform */
    float            rotation;      /* radians */
    br_vec2          scale;         /* (0,0) means (1,1) */
} br_material3d_texture;

/* Every material parameter. Start from br_material3d_default(model). */
typedef struct br_material3d_desc {
    br_material3d_model   model;
    br_color              base_color;          /* linear, alpha used by MASK/BLEND */
    float                 metallic;            /* PBR */
    float                 roughness;           /* PBR */
    br_color              emissive;            /* linear rgb, a ignored */
    float                 emissive_strength;   /* KHR_materials_emissive_strength (default 1) */
    float                 normal_scale;
    float                 occlusion_strength;
    br_alpha_mode         alpha_mode;
    float                 alpha_cutoff;        /* MASK (default 0.5) */
    bool                  double_sided;        /* no back-face culling, back faces lit flipped */
    bool                  receive_shadows;     /* default true */
    br_color              specular;            /* Phong / Toon specular colour */
    float                 shininess;           /* Phong exponent */
    uint32_t              toon_bands;          /* Toon: number of diffuse bands (default 3) */
    br_material3d_texture textures[BR_MATERIAL3D_SLOT_COUNT];
    /* custom */
    const br_shader_blob *custom_fragment_shader;
    uint8_t               custom_uniforms[BR_MATERIAL3D_CUSTOM_UNIFORM_SIZE];
    uint32_t              custom_uniform_size;
    br_texture            custom_textures[BR_MATERIAL3D_CUSTOM_TEXTURES];
    uint32_t              custom_texture_count;
} br_material3d_desc;

/* Defaults for a model: white, metallic 0, roughness 0.5, opaque, single-sided. */
br_material3d_desc br_material3d_default(br_material3d_model model);
/* Creates a material (desc copied). Zero handle on failure. */
br_material3d br_material3d_create(br_engine *e, const br_material3d_desc *desc);
/* Destroys a material (meshes using it draw nothing). */
void br_material3d_destroy(br_engine *e, br_material3d m);
/* True if live. */
bool br_material3d_valid(br_engine *e, br_material3d m);
/* The live parameters to edit in place, NULL if stale. Valid until the next material create (the
 * pool may move; re-fetch it, e.g. after loading a model). Custom counts over the limits are
 * clamped when drawn. */
/* Pointer rule (br_core.h): valid until the next create or destroy of this kind of object. */
br_material3d_desc *br_material3d_get(br_engine *e, br_material3d m);
/* Sets a texture slot with default sampling and no transform. */
void br_material3d_set_texture(br_engine *e, br_material3d m, br_material3d_slot slot, br_texture texture);

#endif
