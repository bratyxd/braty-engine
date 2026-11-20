/* text_field_demo - editable text boxes (br_text_field).
 *
 *   text_field_demo [--frames N --screenshot out.png] [--gpu-driver vulkan|direct3d12]
 *
 * Left: a name field (focused, "Lovelace" selected, caret at the end of the selection), a
 * password field (bullets), an empty search field showing its placeholder, and a multi-line
 * notes field whose text wraps and has scrolled to keep the caret in view.
 * Right: the same kind of field on a tilted canvas3d (texture mode: the field clips with a
 * scissor mask). Click any field to type; Enter on the name field logs a submit.
 * The start state is set from code, so every run (and the golden frame) looks the same: the
 * caret is on for the first 0.5 s after it last moved.
 */
#include "braty/braty_main.h"

static br_text_style style(float size, uint32_t rgb)
{
    br_text_style st = br_text_style_default();
    st.font_size = size;
    st.fill = br_color_from_hex(rgb, 1.0f);
    return st;
}

static void label(br_engine *e, br_node parent, const char *text, float x, float y)
{
    br_text_style st = style(16.0f, 0xaab4c8);
    br_node n = br_text_create(e, text, &st);
    br_node_set_position(e, n, br_vec2_make(x, y));
    br_node_add_child(e, parent, n);
}

static br_node field(br_engine *e, br_node parent, const char *name, float x, float y, float w, float h)
{
    br_text_style st = style(22.0f, 0x1d2230);
    br_node f = br_text_field_create(e, br_vec2_make(w, h), &st);
    br_node_set_label(e, f, name);
    br_node_set_position(e, f, br_vec2_make(x, y));
    br_node_add_child(e, parent, f);
    return f;
}

static void on_name(br_engine *e, br_node f, br_text_field_event event, void *user)
{
    (void)user;
    if (event == BR_TEXT_FIELD_SUBMITTED) {
        br_log_info("text_field_demo: submitted '%s'", br_text_field_text(e, f));
    }
}

static bool demo_init(br_engine *e)
{
    br_node root = br_stage_root(e);
    br_node bg = br_sprite_create(e, br_texture_white(e));
    br_sprite_set_size(e, bg, 1280.0f, 720.0f);
    br_node_set_tint(e, bg, br_color_from_hex(0x1c2030, 1.0f));
    br_node_add_child(e, root, bg);

    br_text_style title = style(30.0f, 0xffffff);
    br_node t = br_text_create(e, "Text fields", &title);
    br_node_set_position(e, t, br_vec2_make(60.0f, 30.0f));
    br_node_add_child(e, root, t);

    label(e, root, "Name (Enter submits)", 60.0f, 96.0f);
    br_node name = field(e, root, "name", 60.0f, 120.0f, 420.0f, 46.0f);
    br_text_field_set_text(e, name, "Ada Lovelace");
    br_text_field_set_max_length(e, name, 24);
    br_text_field_set_listener(e, name, on_name, NULL);

    label(e, root, "Password", 60.0f, 186.0f);
    br_node password = field(e, root, "password", 60.0f, 210.0f, 420.0f, 46.0f);
    br_text_field_set_password(e, password, true);
    br_text_field_set_text(e, password, "correct horse");

    label(e, root, "Search (placeholder)", 60.0f, 276.0f);
    br_node search = field(e, root, "search", 60.0f, 300.0f, 420.0f, 46.0f);
    br_text_field_set_placeholder(e, search, "Type to search\xE2\x80\xA6");

    label(e, root, "Notes (multi-line, scrolled to the caret)", 60.0f, 366.0f);
    br_node notes = field(e, root, "notes", 60.0f, 390.0f, 420.0f, 150.0f);
    br_text_field_set_multiline(e, notes, true);
    br_text_field_set_max_length(e, notes, 2000);
    br_text_field_set_text(e, notes,
                           "Multi-line fields wrap at the box width.\n"
                           "Enter starts a new line, Ctrl+Enter submits.\n\n"
                           "Caf\xC3\xA9, \xCE\xA9\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1 and other UTF-8 text are edited one code point "
                           "at a time; the caret never splits a character. This last line pushed the view down.");

    /* a field on a tilted canvas in a 3D view */
    br_node view = br_view3d_create(e, 640.0f, 560.0f);
    br_node_set_position(e, view, br_vec2_make(600.0f, 100.0f));
    br_node_add_child(e, root, view);
    br_view3d_set_background(e, view, br_color_to_linear(br_color_from_hex(0x2a3148, 1.0f)));
    br_node3d cam = br_view3d_camera(e, view);
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 0.4f, 5.0f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_canvas3d_desc d = br_canvas3d_default(360.0f, 120.0f);
    d.mode = BR_CANVAS3D_TEXTURE;
    br_node3d canvas = br_canvas3d_create(e, &d);
    br_node3d_set_rotation(e, canvas, br_quat_from_euler(-0.25f, 0.45f, 0.0f));
    br_node3d_add_child(e, br_view3d_root(e, view), canvas);
    br_node panel = br_sprite_create(e, br_texture_white(e));
    br_sprite_set_size(e, panel, 360.0f, 120.0f);
    br_node_set_tint(e, panel, br_color_from_hex(0x3b4560, 1.0f));
    br_node_add_child(e, br_canvas3d_root(e, canvas), panel);
    label(e, br_canvas3d_root(e, canvas), "In 3D (canvas3d)", 20.0f, 14.0f);
    br_node in3d = field(e, br_canvas3d_root(e, canvas), "in3d", 20.0f, 44.0f, 320.0f, 50.0f);
    br_text_field_set_text(e, in3d, "Click me in the world");

    /* the name field has focus with its last word selected (shift + ctrl + left) */
    br_text_field_focus(e, name);
    br_text_field_set_selection(e, name, 4, 12);
    br_text_field_set_selection(e, notes, 2000, 2000);   /* clamped to the end: scrolled down */
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Braty - text fields";
    desc.width = 1280;
    desc.height = 720;
    desc.init = demo_init;
    return desc;
}
