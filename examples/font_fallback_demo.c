/* font_fallback_demo - fallback chains: per character, the first font that has the glyph.
 *
 *   font_fallback_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Left: what each row uses; right: the result. The pixel font (examples/fonts/pixel5x7) has
 * only upper case, digits and a little punctuation:
 *   1. pixel5x7.fnt with Noto Sans (coverage) as fallback: lower case, accents, Greek from Noto.
 *   2. pixel5x7.xml with an SDF copy of Noto Sans as fallback.
 *   3. the one-glyph SDF circle font ("O"), no fallback: other letters draw the missing box.
 *   4. Noto Sans, no font has CJK: its own .notdef boxes.
 *   5. the "default" family with a fallback whose only glyph is U+E000 (private use) = a circle.
 */
#include "braty/braty_main.h"

/* The circle distance field (fonts/gen_circle_sdf.py) as the glyph of U+E000. */
static const char *k_circle_pua =
    "common lineHeight=64 base=52 scaleW=64 scaleH=64 pages=1\n"
    "page id=0 file=\"circle_field.png\"\n"
    "distanceField fieldType=sdf distanceRange=8\n"
    "char id=57344 x=0 y=0 width=64 height=64 xoffset=0 yoffset=0 xadvance=64 page=0\n";

static void add_row(br_engine *e, int row, const char *what, const char *text, br_text_style *st)
{
    float y = 40.0f + 128.0f * (float)row;
    br_text_style label = br_text_style_default();
    label.font_size = 20.0f;
    label.fill = br_color_from_hex(0xA8DADC, 1.0f);
    br_node l = br_text_create(e, what, &label);
    br_node_set_position(e, l, br_vec2_make(32.0f, y + 16.0f));
    br_node_add_child(e, br_stage_root(e), l);

    st->fill = br_color_from_hex(0xF1FAEE, 1.0f);
    br_node t = br_text_create(e, text, st);
    br_node_set_position(e, t, br_vec2_make(520.0f, y));
    br_node_add_child(e, br_stage_root(e), t);
}

static bool demo_init(br_engine *e)
{
    br_gpu *gpu = br_engine_gpu(e);
    if (gpu != NULL) {
        br_gpu_set_clear_color(gpu, br_color_from_hex(0x1D3557, 1.0f));
    }
    br_font *pixel_fnt = br_bitmap_font_load(e, "fonts/pixel5x7.fnt");
    br_font *pixel_xml = br_bitmap_font_load(e, "fonts/pixel5x7.xml");
    br_font *circle = br_bitmap_font_load(e, "fonts/circle_sdf.fnt");
    size_t size = 0;
    const void *data = br_font_default_data(&size);
    br_font_options opts;
    SDL_zero(opts);
    opts.sdf = true;
    br_font *noto_sdf = br_font_create(e, data, size, &opts);  /* the engine frees both at shutdown */
    br_font *circle_pua = br_bitmap_font_parse(e, k_circle_pua, SDL_strlen(k_circle_pua), "fonts", true);
    if (pixel_fnt == NULL || pixel_xml == NULL || circle == NULL || noto_sdf == NULL || circle_pua == NULL) {
        return false;
    }
    br_font *noto = br_font_default(e);
    br_font_set_fallbacks(e, pixel_fnt, &noto, 1);
    br_font_set_fallbacks(e, pixel_xml, &noto_sdf, 1);
    br_font_family_set_fallbacks(e, "default", &circle_pua, 1);

    br_text_style st = br_text_style_default();
    st.font = pixel_fnt;
    st.font_size = 40.0f;
    add_row(e, 0, "pixel5x7.fnt -> Noto Sans", "SCORE 100: Caf\xC3\xA9 \xCE\xBB!", &st);
    st.font = pixel_xml;
    add_row(e, 1, "pixel5x7.xml -> Noto Sans SDF", "LEVEL 3: na\xC3\xAFve \xD0\x96", &st);
    st.font = circle;
    st.font_size = 64.0f;
    add_row(e, 2, "circle (SDF), no fallback", "OXO", &st);
    st.font = NULL;
    st.font_size = 48.0f;
    add_row(e, 3, "Noto Sans, CJK in no font", "Tofu \xE6\x97\xA5\xE6\x9C\xAC", &st);
    add_row(e, 4, "family default -> U+E000", "Coins \xEE\x80\x80 x 3", &st);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - font fallback demo";
    desc.width = 1280;
    desc.height = 720;
    desc.asset_root = BRATY_EXAMPLES_DIR; /* the source tree (CMake), any build dir works */
    desc.init = demo_init;
    return desc;
}
