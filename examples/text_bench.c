/* text_bench - 2,000 static text nodes, to measure what an unchanged text node costs per frame.
 *
 *   text_bench [--frames N --screenshot out.png] [--vsync off --profile build/trace.json]
 *
 * A grid of short labels (a quarter with a stroke, a quarter with a drop shadow) that never
 * change. Measure in a release build with `--vsync off --frames 600 --profile FILE` and read
 * `render.collect` in FILE.txt (docs/agents/text.md has the numbers).
 */
#include "braty/braty_main.h"

enum { BENCH_COUNT = 2000, BENCH_COLUMNS = 20 };

static bool bench_init(br_engine *e)
{
    br_gpu *gpu = br_engine_gpu(e);
    if (gpu != NULL) {
        br_gpu_set_clear_color(gpu, br_color_from_hex(0x101820, 1.0f));
    }
    for (int i = 0; i < BENCH_COUNT; i++) {
        br_text_style st = br_text_style_default();
        st.font_size = 12.0f;
        st.fill = br_color_from_hex(0xE8F0F8, 1.0f);
        if (i % 4 == 1) {
            st.stroke_width = 2.0f;
            st.stroke_color = br_color_from_hex(0x803020, 1.0f);
        } else if (i % 4 == 2) {
            st.drop_shadow = true;
            st.shadow_distance = 2.0f;
        }
        char label[32];
        SDL_snprintf(label, sizeof(label), "Item %04d: %d pts", i, (i * 37) % 10000);
        br_node t = br_text_create(e, label, &st);
        br_node_set_position(e, t, br_vec2_make(4.0f + 64.0f * (float)(i % BENCH_COLUMNS),
                                                4.0f + 7.0f * (float)(i / BENCH_COLUMNS)));
        br_node_add_child(e, br_stage_root(e), t);
    }
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - text bench";
    desc.width = 1280;
    desc.height = 720;
    desc.init = bench_init;
    return desc;
}
