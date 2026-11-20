/* assets_demo - manifests, background bundle loading, spritesheets, compressed textures.
 *
 *   assets_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Phase 1: a progress bar driven by br_assets_load_bundle("game") running on worker threads
 *          (the frame budget is set to 0 so exactly one asset finishes per frame and the bar
 *          is visible even though the files are tiny). Around frame 20 everything is in.
 * Phase 2: top row - the whole atlas, then frames from it: gem (pivot = default anchor),
 *          9-slice panel. Middle row: an untrimmed star next to the same star stored trimmed,
 *          an arrow next to the same arrow stored rotated in the atlas (each pair must look
 *          identical), the 4-frame "walk" animation. Bottom row: BC1 DDS with mips (picked over
 *          the PNG by the format preference), BC7 DDS (DX10 header), BC1 KTX, and the magenta
 *          fallback for a file the manifest lists but that does not exist.
 * Assets come from examples/assets (generate_assets.py).
 */
#include "braty/braty_main.h"

#include "cjson/cJSON.h"

typedef struct demo_state {
    br_node          loading;      /* progress bar group */
    br_node          bar_fill;
    float            progress;
    bool             loaded;
    bool             built;
    br_spritesheet  *sheet;
    br_node          walker;
    const br_texture *walk_frames;
    uint32_t         walk_count;
} demo_state;

static br_node add_sprite(br_engine *e, br_node parent, br_texture t, float x, float y)
{
    br_node n = br_sprite_create(e, t);
    br_node_set_position(e, n, br_vec2_make(x, y));
    br_node_add_child(e, parent, n);
    return n;
}

/* A flat rectangle (the white texture, tinted and sized). */
static br_node add_rect(br_engine *e, br_node parent, float x, float y, float w, float h, uint32_t rgb)
{
    br_node n = add_sprite(e, parent, br_texture_white(e), x, y);
    br_sprite_set_size(e, n, w, h);
    br_node_set_tint(e, n, br_color_from_hex(rgb, 1.0f));
    return n;
}

/* ---- snippet for docs/agents/assets.md: bundle loading with progress */

static void on_bundle_progress(br_engine *e, const char *bundle, float progress, void *user)
{
    demo_state *s = user;
    (void)e;
    (void)bundle;
    s->progress = progress;
    s->loaded = progress >= 1.0f; /* the last call has progress 1, failures included */
}

static void start_loading(br_engine *e, demo_state *s)
{
    br_assets_set_preferences(e, 1.0f, "dds,png");  /* "checker.{png,dds}" resolves to checker.dds */
    br_assets_load_manifest(e, "manifest.json");     /* bundles + aliases */
    br_assets_set_frame_budget(e, 0.0f);             /* demo only: one asset per frame */
    br_assets_load_bundle(e, "game", on_bundle_progress, s);
}

static void build_scene(br_engine *e, demo_state *s)
{
    br_spritesheet *sheet = br_assets_get(e, "atlas");       /* loaded by the bundle */
    br_node root = br_stage_root(e);
    add_sprite(e, root, br_spritesheet_frame(e, sheet, "gem.png"), 340.0f, 60.0f);
    const br_texture *walk = NULL;
    uint32_t walk_count = br_spritesheet_animation(e, sheet, "walk", &walk);
    br_texture missing = br_texture_load(e, "missing");      /* magenta fallback, logged */
    /* ---- end of snippet */
    s->sheet = sheet;
    s->walk_frames = walk;
    s->walk_count = walk_count;

    /* top row: atlas, panel with its 9-slice borders drawn as lines */
    add_sprite(e, root, br_spritesheet_texture(sheet), 20.0f, 20.0f);
    br_node panel = add_sprite(e, root, br_spritesheet_frame(e, sheet, "panel.png"), 400.0f, 36.0f);
    br_node_set_scale(e, panel, br_vec2_make(2.0f, 2.0f));
    float borders[4];
    if (br_spritesheet_frame_borders(e, sheet, "panel.png", borders)) {
        add_rect(e, root, 400.0f + borders[0] * 2.0f, 30.0f, 1.0f, 108.0f, 0xff4040);
        add_rect(e, root, 400.0f + (48.0f - borders[2]) * 2.0f, 30.0f, 1.0f, 108.0f, 0xff4040);
    }

    /* middle row: trimmed / rotated pairs over light boxes showing the original frame size */
    float y = 190.0f;
    add_rect(e, root, 20.0f, y, 64.0f, 64.0f, 0x2a3040);
    add_rect(e, root, 100.0f, y, 64.0f, 64.0f, 0x2a3040);
    add_sprite(e, root, br_spritesheet_frame(e, sheet, "star_full.png"), 20.0f, y);
    add_sprite(e, root, br_spritesheet_frame(e, sheet, "star_trimmed.png"), 100.0f, y);
    br_node a1 = add_sprite(e, root, br_spritesheet_frame(e, sheet, "arrow_full.png"), 200.0f, y);
    br_node a2 = add_sprite(e, root, br_spritesheet_frame(e, sheet, "arrow_rotated.png"), 280.0f, y);
    br_node_set_scale(e, a1, br_vec2_make(1.0f, 2.0f));
    br_node_set_scale(e, a2, br_vec2_make(1.0f, 2.0f));
    s->walker = add_sprite(e, root, walk_count > 0 ? walk[0] : missing, 380.0f, y);
    br_node_set_scale(e, s->walker, br_vec2_make(2.0f, 2.0f));
    for (uint32_t i = 0; i < walk_count; i++) {
        add_sprite(e, root, walk[i], 460.0f + 40.0f * (float)i, y + 16.0f);
    }

    /* bottom row: compressed textures and the fallback */
    y = 300.0f;
    add_sprite(e, root, br_texture_load(e, "checker"), 20.0f, y);
    add_sprite(e, root, br_texture_load(e, "gradient"), 170.0f, y);
    br_node badge = add_sprite(e, root, br_texture_load(e, "badge"), 320.0f, y);
    br_node_set_scale(e, badge, br_vec2_make(2.0f, 2.0f));
    br_node miss = add_sprite(e, root, missing, 470.0f, y);
    br_node_set_scale(e, miss, br_vec2_make(32.0f, 32.0f));
    /* mip chain: the checker drawn small samples its lower levels */
    br_node small = add_sprite(e, root, br_texture_load(e, "checker"), 620.0f, y);
    br_node_set_scale(e, small, br_vec2_make(0.25f, 0.25f));

    /* the json loader and the spritesheet loader share .json: config.json stays plain JSON */
    const cJSON *config = br_assets_get(e, "config");
    const cJSON *bg = config != NULL ? cJSON_GetObjectItemCaseSensitive(config, "background") : NULL;
    if (cJSON_GetArraySize(bg) == 3) {
        br_gpu_set_clear_color(br_engine_gpu(e),
                               br_color_make((float)cJSON_GetArrayItem(bg, 0)->valuedouble,
                                             (float)cJSON_GetArrayItem(bg, 1)->valuedouble,
                                             (float)cJSON_GetArrayItem(bg, 2)->valuedouble, 1.0f));
    }

    br_asset_info infos[64];
    uint32_t count = br_assets_list(e, infos, BR_ARRAY_COUNT(infos));
    for (uint32_t i = 0; i < count && i < BR_ARRAY_COUNT(infos); i++) {
        br_log_info("asset %-28s %-12s refs %u  %zu bytes", infos[i].path, infos[i].type ? infos[i].type : "-",
                    infos[i].ref_count, infos[i].byte_size);
    }
}

static void demo_update(br_engine *e, float dt, void *user)
{
    (void)dt;
    demo_state *s = user;
    if (!s->built) {
        br_sprite_set_size(e, s->bar_fill, 600.0f * s->progress, 24.0f);
        if (s->loaded) {
            br_node_set_visible(e, s->loading, false);
            build_scene(e, s);
            s->built = true;
        }
        return;
    }
    if (s->walk_count > 0) {
        uint32_t frame = (uint32_t)(br_ticker_elapsed(e) * 6.0) % s->walk_count;
        br_sprite_set_texture(e, s->walker, s->walk_frames[frame]);
    }
}

static void demo_shutdown(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool demo_init(br_engine *e)
{
    demo_state *s = br_alloc(sizeof(demo_state));
    if (s == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, demo_shutdown, s);
    br_gpu_set_clear_color(br_engine_gpu(e), br_color_from_hex(0x101422, 1.0f));
    s->loading = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), s->loading);
    add_rect(e, s->loading, 180.0f, 258.0f, 600.0f, 24.0f, 0x303850);
    s->bar_fill = add_rect(e, s->loading, 180.0f, 258.0f, 0.0f, 24.0f, 0x60c0ff);
    start_loading(e, s);
    br_system_add(e, "assets_demo.update", BR_PHASE_UPDATE, 0, demo_update, s);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - assets";
    desc.width = 960;
    desc.height = 540;
    // ponytail: assets are read in place from the source tree (build/<preset>/ -> examples/assets);
    // a packaged build would copy them next to the executable and use "assets".
    desc.asset_root = BRATY_EXAMPLES_DIR "/assets";
    desc.init = demo_init;
    return desc;
}
