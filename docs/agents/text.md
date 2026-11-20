# text - fonts, text, bitmap fonts, rich text, split text

Headers: `engine/include/braty/br_text.h` (styles, text node, measure, UTF-8), `br_font.h`
(TTF fonts, families), `br_bitmap_font.h` (AngelCode / installed fonts, bitmap text),
`br_rich_text.h` (tags, split text). Source: `engine/src/text/`, shader
`engine/shaders/text_sdf.frag.hlsl`. Test: `tests/test_text.c`. Examples: `examples/text_demo.c`,
`examples/sdf_fonts_demo.c`.

## What it does

- Text works with no files: the default font is Noto Sans Regular embedded as a C array
  (`engine/src/text/br_font_default_data.c`, made by `engine/assets/fonts/embed_font.py`,
  licence `engine/assets/fonts/OFL.txt`). Latin, Greek and Cyrillic.
- A text node lays out UTF-8 with a `br_text_style` and draws quads: shadow, stroke, fill,
  then underline/strike bars. Layout and quads are cached per node (below).
- TTF glyphs are rasterised on demand into one atlas per font, keyed by glyph, pixel size and
  effect (fill / grown outline for strokes and fake bold / softened for shadows). The atlas
  grows 256 -> 4096; an old atlas texture is kept alive until the next frame.
- `br_font_options.sdf = true` makes a font store distance fields at one base size; it is drawn
  with `text_sdf.frag` through `br_render_push_custom` and stays sharp at any scale (use it for
  zoomed or 3D text). AngelCode fonts with `distanceField` (`sdf`, `msdf`, `mtsdf`) use the same
  shader (and `canvas3d_sdf.frag` on a canvas3d); pages are premultiplied on upload, so the
  multi-channel mode divides by alpha before the median (`examples/sdf_fonts_demo.c`, golden
  `sdf_fonts_demo`, draws one glyph of each kind inside a reference ring).

## Per-node quad cache

Glyph lookup and quad generation run only when something that moves a quad changes: the text,
the style, the anchor, the resolution (device scale), a font being freed, a fallback chain, or
any TTF atlas being recreated (it grew: glyphs moved, new texture; `atlas_generation`). The cache
holds local-space vertices with their colour before tint; world vertices are recomputed only
when the node's matrix, world tint or alpha changes. An unchanged node then costs one
`br_render_push_quad` per quad (one `br_render_push_custom` per SDF run). `br_text_quad_builds`
counts rebuilds (`test_text` `test_quad_cache` checks every invalidation).

Measured with `examples/text_bench.c` (2,000 static labels, a quarter stroked, a quarter with a
drop shadow, ~40,000 glyphs), D3D12, RTX 3060 Ti, `--vsync off --frames 600 --profile FILE`,
five runs each (other builds were running on the machine, so the ranges are wide):

| Build | Before (every frame: lookups + quads) | After (cache) |
|---|---|---|
| release (`windows-gcc-release`), frame ms avg | 6.7 - 13.0 | 1.1 - 1.4 |
| debug (`windows-gcc`), `render.collect` self ms | 29.5 - 38.3 | 1.8 - 2.1 |

The release preset has the profiler scopes off, so the release row is the whole frame (GPU about
1.2 ms either way); the debug row is the `render.collect` scope from `FILE.txt`.

## Functions you actually need

| Function | What for |
|---|---|
| `br_text_style_default()`, `br_text_style_set_vertical_gradient` | Start a style (Pixi defaults: 26 px, black fill). |
| `br_text_create(e, utf8, &style)`, `br_text_set_text`, `br_text_set_style`, `br_text_set_anchor` | Text node (JSON type `"text"`). |
| `br_text_measure(e, utf8, &style)`, `br_text_size`, `br_text_get_line` | Size and line breaks. |
| `br_text_prepare(e, n)`, `br_text_quad_builds(e, n)` | Warm glyphs and quads at load time; count cache rebuilds. |
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
br_node_set_position(e, title, br_vec2_make(16.0f, 296.0f));
br_node_add_child(e, br_stage_root(e), title);
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

Type names `"text"`, `"bitmap_text"`, `"rich_text"`:
`{ "type": "text", "text": "Hello", "style": { "font_size": 32, "fill": "white" } }`,
`{ "type": "bitmap_text", "text": "SCORE 0", "font": "fonts/pixel.fnt", "size": 24 }`,
`{ "type": "rich_text", "text": "<b>Bold</b> <warn>red</warn>", "tag_styles": { "warn": { "color": "red" } } }`.
All style keys: `docs/DATA_FORMATS.md` ("text, bitmap_text, rich_text") and
`engine/src/data/br_node_json_text.c`.

## Limits (bad data must not freeze the game)

Checked inside the module whatever the caller passes (`br_text_create`, `br_text_set_style`,
`br_text_measure`, split text, `br_bitmap_font_install`):

- Non-finite style numbers fall back to 0 (`font_size` 0 = the default 26 px / native size);
  `font_size`, `stroke_width`, `shadow_blur` and rich `<size=>` are capped at 4096; spacing,
  leading, padding, wrap width and shadow distance at +-1e6.
- Coverage glyphs are rasterised at an em size of at most 1024 pixels; bigger text is scaled up
  from that (use an SDF font for huge titles).
- Stroke and shadow growth / blur are capped at 64 raster pixels (their distance field costs grow
  with the square of the size; logged once). Wider effects: an SDF font or a filter.
- A glyph that does not fit the atlas (full, or larger than 4096) is remembered as empty: drawn
  as nothing, rasterised once. SDF fonts: `sdf_size` <= 256, `sdf_padding` <= 64.
- `br_text_prepare(e, node)` runs the rasterise pass of a draw and builds the quad cache now
  (warm the atlas at load time).

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
  `style.font_family` find it. Installing again under the same name frees the old font.
- Fonts can be freed while text uses them (`br_font_destroy`, asset unload, uninstall,
  reinstall): every text node holding the font in `style.font` drops it and every node whose
  layout used it lays out again, falling back to its family or the default font. Pointers
  kept elsewhere (your own structs) are yours to clear.
- Gradients are over the whole text box (`start`/`end` as fractions). Vertical and horizontal
  gradients are exact; diagonal ones are sampled at glyph corners. Split pieces get their own box.
- Rich-text line info offsets (`br_text_get_line`) refer to `br_rich_text_plain`, not the markup.
- Fake bold/italic is used when a family has no such face (outline growth / 0.2 shear).
- Asset loaders: `ttf,otf` -> `"font"`, `fnt,xml` -> `"bitmap_font"` (an `.xml` is claimed only
  when it contains `<font`, so other XML goes to other loaders). A loaded `.fnt` holds one
  asset reference per page texture and releases them when it is freed. Numbers in `.fnt` files
  follow the SVG number grammar; ids out of range drop the char, `distanceRange <= 0` becomes 4.
- Not done: complex shaping (Arabic, Indic, ligatures; stb_truetype has no shaper), bidi,
  colour emoji, and atlas eviction (a full 4096 atlas stops caching new glyphs: they are
  remembered as empty and not drawn; logged once).
