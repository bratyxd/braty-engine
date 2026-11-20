/* Registers the built-in modules on every new engine, in dependency order.
 *
 * Owned by Task 2. Each later module fills its own br_<module>_register; nobody edits this
 * list again. Shutdown callbacks run in reverse order of registration, so a module may use
 * any module registered before it until its own shutdown runs.
 */
#include "app/br_engine_internal.h"
#include "braty/braty.h"
#include "core/br_storage_internal.h"
#include "render/br_render_internal.h"
#include "scene/br_scene_internal.h"
#include "three_d/br_three_d_internal.h"

void br_builtin_register(br_engine *e)
{
    br_arena_register(e);          /* frame arena first: freed last at shutdown */
    br_storage_register(e);        /* files and packs: every later module may read files */
    br_scene_register(e);          /* node table, components, transform/sort/cull systems */
    br_render_register(e);         /* textures, sprite type, batcher, collect/submit systems */
    br_assets_register(e);         /* asset cache (after render: unloads textures before render frees them) */
    br_texture_register_loader(e); /* png/jpg/... -> br_texture */
    br_input_register(e);
    br_events_register(e);
    br_audio_register(e);
    br_graphics_register(e);
    br_text_register(e);
    br_sprites_register(e);
    br_mesh_register(e);
    br_particles_register(e);
    br_filters_register(e);
    br_three_d_register(e);
    br_gltf_register(e);
    br_data_register(e);
    br_debug_register(e);
    br_camera_register(e);
    br_canvas3d_register(e);
    br_scenes_register(e);
    br_animation_register(e);
    br_profiler_register(e);
    br_playtest_register(e);
    br_loc_register(e);
    br_text_field_register(e);
    /* nodes are destroyed by br_engine_destroy before any shutdown callback runs, so every
     * on_destroy and destroy listener (animation, canvas3d, data, the game...) sees its state */
}
