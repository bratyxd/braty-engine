# text - fonts, text, bitmap fonts, rich text, split text

Headers: `engine/include/braty/br_text.h` (styles, text node, measure, UTF-8), `br_font.h`
(TTF fonts, families), `br_bitmap_font.h` (AngelCode / installed fonts, bitmap text),
`br_rich_text.h` (tags, split text). Source: `engine/src/text/`, shader
`engine/shaders/text_sdf.frag.hlsl`. Test: `tests/test_text.c`. Example: `examples/text_demo.c`.

## What it does

- Text works with no files: the default font is Noto Sans Regular embedded as a C array
  (`engine/src/text/br_font_default_data.c`, made by `engine/assets/fonts/embed_font.py`,
  licence `engine/assets/fonts/OFL.txt`). Latin, Greek and Cyrillic.
- A text node lays out UTF-8 with a `br_text_style` and draws quads: shadow, stroke, fill,
  then underline/strike bars. Layout is cached until text or style change.
- TTF glyphs are rasterised on demand into one atlas per font, keyed by glyph, pixel size and
  effect (fill / grown outline for strokes and fake bold / softened for shadows). The atlas
  grows 256 -> 4096; an old atlas texture is kept alive until the next frame.
- `br_font_options.sdf = true` makes a font store distance fields at one base size; it is drawn
  with `text_sdf.frag` through `br_render_push_custom` and stays sharp at any scale (use it for
  zoomed or 3D text). AngelCode fonts with `distanceField` (sdf / msdf) use the same shader.

## Functions you actually need

| Function | What for |
|---|---|
| `br_text_style_default()`, `br_text_style_set_vertical_gradient` | Start a style (Pixi defaults: 26 px, black fill). |
| `br_text_create(e, utf8, &style)`, `br_text_set_text`, `br_text_set_style`, `br_text_set_anchor` | Text node (JSON type `"text"`). |
| `br_text_measure(e, utf8, &style)`, `br_text_size`, `br_text_get_line` | Size and line breaks. |
| `br_font_load(e, "x.ttf")`, `br_font_create(e, bytes, size, &opts)`, `br_font_add_family` | More fonts; families give `font_family` + bold/italic. |
| `br_bitmap_font_load(e, "x.fnt")`, `br_bitmap_text_create(e, utf8, font, size)` | AngelCode fonts (text or XML, multi-page, kerning). |
| `br_bitmap_font_install(e, name, &style, &opts)` | Bake a TTF + style (gradient, stroke, shadow) into a bitmap font. |
| `br_rich_text_create(e, markup, &style)`, `br_rich_text_set_tag_style` | `<b> <i> <u> <s> <br> <color=> <size=> <font=>` + named tags. |
| `br_split_text_create(e, utf8, &style, BR_SPLIT_CHARS/WORDS/LINES)` | One child text node per piece, anchored at its centre. |

## Snippet (from examples/text_demo.c)

```c
br_text_style st = br_text_style_default();
st.font_size = 48.0f;
st.bold = true;
br_text_style_set_vertical_gradient(&st, br_color_from_hex(0xfff27a, 1.0f), br_color_from_hex(0xff3d7f, 1.0f));
st.gradient.stop_count = 3;
st.gradient.offsets[1] = 0.5f;
st.gradient.colors[1] = br_color_from_hex(0xff9a3d, 1.0f);
st.gradient.offsets[2] = 1.0f;
st.gradient.colors[2] = br_color_from_hex(0x9b3dff, 1.0f);
st.stroke_width = 3.0f;
st.stroke_color = br_color_from_hex(0x1a0a2a, 1.0f);
br_node title = br_text_create(e, "Gradient", &st);
br_node_set_position(e, title, 16.0f, 296.0f);
br_node_add_child(e, br_scene_root(e), title);
```

SDF text that scales cleanly:

```c
size_t size;
const void *data = br_font_default_data(&size);
br_font_options opts;
SDL_zero(opts);
opts.sdf = true;
s->sdf_font = br_font_create(e, data, size, &opts);   /* free with br_font_destroy */
br_text_style st = white_style(22.0f);
st.font = s->sdf_font;
```

## Scene JSON

Type names `"text"`, `"bitmap_text"`, `"rich_text"`; their `from_json` is written by Task 10.

## Gotchas

- Default fill is black (Pixi). On a dark clear colour set `style.fill`.
- Node origin = top-left of the text box; `br_text_set_anchor(e, n, 0.5f, 0.5f)` centres it.
  `style.baseline` moves which line of the box sits at y = 0.
- Coverage glyphs are rasterised at `font_size * resolution` (resolution = target pixel ratio
  unless `br_text_set_resolution`). Scaling the node up blurs them: use an SDF font.
- SDF fonts limit stroke + shadow growth to `sdf_padding` base pixels (default 12 at 64 px).
- Bitmap text: `font_size <= 0` means the native size. White AngelCode fonts are tinted by the
  style fill; installed fonts with colours baked in draw as baked (tint with the node).
  Stroke on a non-SDF bitmap font is ignored (bake it with `br_bitmap_font_install`).
- `br_bitmap_font_install` registers the name as a font family, so `<font=Name>` and
  `style.font_family` find it. Uninstalling or destroying a font that text still uses is a bug.
- Gradients are over the whole text box (`start`/`end` as fractions). Vertical and horizontal
  gradients are exact; diagonal ones are sampled at glyph corners. Split pieces get their own box.
- Rich-text line info offsets (`br_text_get_line`) refer to `br_rich_text_plain`, not the markup.
- Fake bold/italic is used when a family has no such face (outline growth / 0.2 shear).
- Asset loaders: `ttf,otf` -> `"font"`, `fnt,xml` -> `"bitmap_font"` (an XML file that is not an
  AngelCode font fails to load with an error).
- Not done: complex shaping (Arabic, Indic, ligatures; stb_truetype has no shaper), bidi,
  colour emoji, and atlas eviction (a full 4096 atlas stops caching new glyphs, logged once).
