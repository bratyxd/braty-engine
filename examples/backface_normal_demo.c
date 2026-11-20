/* backface_normal_demo - a double-sided, normal-mapped plane seen from the front and the back.
 *
 *   backface_normal_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Two copies of one plane with a bump normal map, lit by the same light from the camera's side:
 * the left one shows its front, the right one is turned upside down so the camera sees its back
 * face. A double-sided back face flips its whole tangent frame, so both must show the same lit
 * bumps (mirrored); a shader that flips only the normal lights the back one wrongly.
 */
#include "braty/braty_main.h"

enum { TEX = 256 };

/* Tangent-space normal map of round bumps, one per 64-pixel cell. */
static br_texture make_bumps(br_engine *e)
{
    static uint8_t px[TEX * TEX * 4];
    for (int y = 0; y < TEX; y++) {
        for (int x = 0; x < TEX; x++) {
            float fx = (float)(x % 64) / 32.0f - 1.0f;
            float fy = (float)(y % 64) / 32.0f - 1.0f;
            br_vec3 n = br_vec3_make(0.0f, 0.0f, 1.0f);
            if (fx * fx + fy * fy < 0.8f) {
                n = br_vec3_normalize(br_vec3_make(fx * 0.9f, -fy * 0.9f, 1.0f)); /* +Y = image top */
            }
            uint8_t *p = &px[(y * TEX + x) * 4];
            p[0] = (uint8_t)((n.x * 0.5f + 0.5f) * 255.0f);
            p[1] = (uint8_t)((n.y * 0.5f + 0.5f) * 255.0f);
            p[2] = (uint8_t)((n.z * 0.5f + 0.5f) * 255.0f);
            p[3] = 255;
        }
    }
    br_texture_style style = { BR_FILTER_LINEAR, BR_ADDRESS_REPEAT, true, 8.0f };
    return br_texture_create_rgba8(e, TEX, TEX, px, &style);
}

static bool demo_init(br_engine *e)
{
    br_node view = br_view3d_create(e, 1280.0f, 720.0f);
    br_view3d_set_auto_resize(e, view, true);
    br_view3d_set_background(e, view, br_color_to_linear(br_color_from_hex(0x1D3557, 1.0f)));
    br_view3d_post(e, view)->bloom = false;
    br_node_add_child(e, br_stage_root(e), view);
    br_node3d root = br_view3d_root(e, view);
    br_node3d cam = br_view3d_camera(e, view);
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 5.0f, 3.5f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));

    br_light3d sun = br_light3d_default(BR_LIGHT3D_DIRECTIONAL);
    sun.intensity = 1.2f;
    br_node3d sun_node = br_light3d_create(e, &sun);
    br_node3d_set_position(e, sun_node, br_vec3_make(-3.0f, 4.0f, 2.0f));
    br_node3d_look_at(e, sun_node, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_node3d_add_child(e, root, sun_node);

    br_material3d_desc md = br_material3d_default(BR_MATERIAL3D_PBR);
    md.base_color = br_color_to_linear(br_color_from_hex(0xA8A296, 1.0f));
    md.roughness = 0.6f;
    md.double_sided = true;
    br_material3d m = br_material3d_create(e, &md);
    br_material3d_set_texture(e, m, BR_MATERIAL3D_NORMAL, make_bumps(e));
    br_geometry3d plane = br_geometry3d_plane(e, 3.0f, 3.0f, 1, 1);

    br_node3d front = br_mesh3d_create(e, plane, m);
    br_node3d_set_position(e, front, br_vec3_make(-1.8f, 0.0f, 0.0f));
    br_node3d_add_child(e, root, front);
    br_node3d back = br_mesh3d_create(e, plane, m);
    br_node3d_set_position(e, back, br_vec3_make(1.8f, 0.0f, 0.0f));
    br_node3d_set_rotation(e, back, br_quat_from_axis_angle(br_vec3_make(0.0f, 0.0f, 1.0f), BR_PI));
    br_node3d_add_child(e, root, back);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - back-face normal map demo";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
