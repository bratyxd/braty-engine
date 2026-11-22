# third_party

Vendored single-file libraries, unmodified, each with its licence file.
Each library is compiled by exactly one `.c` file in its folder (CMake target `braty_third_party`).
Include them as `#include "stb/stb_image.h"`, `"cjson/cJSON.h"`, `"cgltf/cgltf.h"`, `"microui/microui.h"`.

| Library | Upstream | Commit | Licence |
|---|---|---|---|
| stb_image, stb_image_write, stb_truetype, stb_rect_pack, stb_vorbis | github.com/nothings/stb | 6b5799bae8a1 | MIT or public domain |
| cgltf | github.com/jkuhlmann/cgltf | 85cd62382dfe | MIT |
| cJSON | github.com/DaveGamble/cJSON | 6d9f2443ab07 | MIT |
| microui | github.com/rxi/microui | 0850aba86095 | MIT |

Compile definitions set for everyone (see CMakeLists.txt): `STBI_NO_STDIO`, `STBI_WRITE_NO_STDIO`,
`STB_VORBIS_NO_STDIO`, so the engine reads files only through SDL (`br_file_*`).
To use stb_vorbis declarations: `#define STB_VORBIS_HEADER_ONLY` then `#include "stb/stb_vorbis.c"`.

## Fetched by add-ons

Not in this folder: an add-on's `CMakeLists.txt` uses an installed package when CMake finds one,
else fetches the source with FetchContent and builds it static (`addons/<name>/CMakeLists.txt`).

| Library | Add-on | Upstream | Version | Licence |
|---|---|---|---|---|
| SDL3_net | `net` | github.com/libsdl-org/SDL_net | release-3.2.0 | zlib |
