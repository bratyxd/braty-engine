# Getting started: a new game from zero

This walks you through a tiny game, **Coin Rush** (move a ship with the keyboard or a gamepad,
collect coins, hear a sound, watch the score), in eleven steps; three more get it ready to ship
(steps 12-14: texts from a string table, a pack and an install folder, a text field); then a 3D
track (steps 15-18: a `.glb` level as the scene, components from glTF extras, a character with a
following camera, a world-space label). Every step builds and runs (`tools/check_getting_started.py` checks it); every
code block below is the complete file at that step, so you can copy it as it is. For finished
games built the same way, read `sample_projects/star_courier/` (2D) and
`sample_projects/crate_rush/` (3D).

You need the engine checkout (called `<braty>` below), CMake >= 3.24, Ninja, GCC or Clang and
SDL 3.4 (pass its install prefix as `CMAKE_PREFIX_PATH`, or let CMake fetch and build it).
The engine's ideas in one paragraph: a game is **data** (JSON files) plus **components** (plain
C structs whose fields the JSON can fill) plus **systems** (functions that run every frame and
loop over component arrays). Read `AGENTS.md` for the rules and `docs/DATA_FORMATS.md` for
every file format.

## 1. Project layout

```
my_game/
  CMakeLists.txt
  src/main.c
  data/game.json                   copied next to the executable after every build
  data/assets/                     asset_root: everything the game loads
    scenes/main.scene.json
    prefabs/coin.prefab.json
    manifest.json
    strings/strings.en.json        step 12: every text the player reads
    sprites/  sounds/              copied from the sample game in step 8
  playtest/name.json               step 14: a scripted play-test
```

Put `my_game/` anywhere; it finds the engine through `BRATY_DIR`.

## 2. CMake

<!-- file: CMakeLists.txt -->
```cmake
cmake_minimum_required(VERSION 3.24)
project(my_game LANGUAGES C)

set(BRATY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../braty" CACHE PATH "Braty engine checkout")
set(BRATY_BUILD_TESTS OFF)      # only the library, not the engine's tests,
set(BRATY_BUILD_EXAMPLES OFF)   # examples
set(BRATY_BUILD_SAMPLES OFF)    # and sample games
add_subdirectory(${BRATY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/braty)

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_executable(my_game ${GAME_SOURCES})
set_target_properties(my_game PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(my_game PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wpedantic>)
target_link_libraries(my_game PRIVATE braty::braty)

# data/ next to the executable: the game runs from any working directory
add_custom_command(TARGET my_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:my_game>)
```

**Why the copy:** `game.json` is read from the executable's directory and `asset_root` is
relative to it, so a game that ships with its data next to the binary finds it no matter where it
is started from. The examples in the engine repository instead point `asset_root` at
`../../examples` relative to the executable, so they only work from the default
`build/<preset>/` directory. The catch: hot reload (debug builds) watches the copies, so edit the files in
the build directory while iterating, or rebuild to copy your edits over.

## 3. `br_main`: a window

A game defines one function, `br_main`, in the one file that includes `braty/braty_main.h`. It
returns a `br_app_desc`; the engine opens the window and calls `init`.

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

static bool game_init(br_engine *e)
{
    br_log_info("my_game: assets come from %s", br_engine_asset_root(e));
    return true; /* false = could not start (log why first) */
}

br_app_desc br_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_app_desc desc;
    SDL_zero(desc);
    desc.title = "Coin Rush";
    desc.width = 960;
    desc.height = 540;
    desc.init = game_init;
    return desc;
}
```

Create the (still empty) `data/` folder too: the post-build step copies it.

```sh
cmake -S my_game -B my_game/build -G Ninja -DBRATY_DIR=<braty> -DCMAKE_PREFIX_PATH=<SDL3 prefix>
cmake --build my_game/build
my_game/build/my_game --frames 60 --screenshot shot.png    # runs 60 frames, saves the last
```

`--frames N` steps a fixed 1/60 s N times and exits; `--screenshot` writes the last frame. Use
them (and open the PNG) to check every step. `--gpu-driver vulkan` or `direct3d12` picks a backend.

## 4. game.json

Move the settings into data. `br_game_config_main` reads game.json in `br_main` (there is no
engine yet) and hands it to the engine, which applies it (window, vsync, clear colour, input,
audio, manifest, debug overlay) before `init` and loads its `start_scene` right after `init`.
`br_main` is one line.

<!-- file: data/game.json -->
```json
{
  "window": { "title": "Coin Rush", "width": 960, "height": 540, "resizable": false },
  "asset_root": "assets",
  "clear_color": "#14182b",
  "debug": { "overlay": false, "toggle_key": "F1" }
}
```

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

static bool game_init(br_engine *e)
{
    (void)e; /* game.json is already applied */
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    /* game.json next to the executable; --debug and --scene <path> from the command line */
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

A typo in game.json (`"widht"`) is logged as `game.json: window.widht: unknown key`. Your own
settings go under a `"game"` key and are read with `br_game_config_json(e)`.

## 5. A scene

A scene file is a tree of nodes. Every node has the Container fields (`position`, `scale`,
`rotation`, `alpha`, `tint`, ...) plus the fields of its `type`. Add `"start_scene"` to
game.json and the engine loads the scene under the stage right after `init`.

<!-- file: data/assets/scenes/main.scene.json -->
```json
{
  "type": "container",
  "label": "main",
  "children": [
    { "type": "text", "label": "score", "text": "COINS 0", "position": [20, 16],
      "style": { "font_size": 28, "bold": true, "fill": "#ffd166" } },
    { "type": "graphics", "label": "ship", "position": [480, 270],
      "commands": [["poly", [0, -18, 14, 14, 0, 6, -14, 14], true], ["fill", "#4fd1c5"],
                   ["stroke", { "width": 2, "color": "#0e3b46" }]] }
  ]
}
```

<!-- file: data/game.json -->
```json
{
  "window": { "title": "Coin Rush", "width": 960, "height": 540, "resizable": false },
  "asset_root": "assets",
  "start_scene": "scenes/main.scene.json",
  "clear_color": "#14182b",
  "debug": { "overlay": false, "toggle_key": "F1" }
}
```

No code changes: build, run with `--screenshot`, and you see the ship and the text. While the
game runs (debug build) you can edit the scene file in the build directory and it reloads.

## 6. A component and a system

Behaviour is a **system** over a **component**. A component is a struct plus a field list that
lets scene files fill it. Register components in `init`: the start scene that uses them is
loaded after `init` returns. Per-game state lives in one
struct handed to the systems as `user` and freed on shutdown.

<!-- file: data/assets/scenes/main.scene.json -->
```json
{
  "type": "container",
  "label": "main",
  "children": [
    { "type": "text", "label": "score", "text": "COINS 0", "position": [20, 16],
      "style": { "font_size": 28, "bold": true, "fill": "#ffd166" } },
    { "type": "graphics", "label": "ship", "position": [480, 270],
      "commands": [["poly", [0, -18, 14, 14, 0, 6, -14, 14], true], ["fill", "#4fd1c5"],
                   ["stroke", { "width": 2, "color": "#0e3b46" }]],
      "components": { "mover": { "velocity": [40, 0] } } }
  ]
}
```

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct mover {
    br_vec2 velocity; /* units per second */
} mover;

typedef struct game {
    uint32_t mover_id;
} game;

static void mover_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    for (uint32_t i = 0; i < br_component_count(e, g->mover_id); i++) {
        mover *m = br_component_at(e, g->mover_id, i);
        br_node n = br_component_owner_at(e, g->mover_id, i);
        br_vec2 p = br_node_position(e, n);
        br_node_set_position(e, n, br_vec2_make(p.x + m->velocity.x * dt, p.y + m->velocity.y * dt));
    }
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, game_free, g);

    static const br_field_desc mover_fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
    };
    br_component_desc desc = { "mover", sizeof(mover), mover_fields, BR_ARRAY_COUNT(mover_fields) };
    g->mover_id = br_component_register(e, &desc);
    br_system_add(e, "game.mover", BR_PHASE_UPDATE, 0, mover_system, g);

    return true; /* the start scene is loaded after init: its components exist by then */
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

The ship now drifts right. Systems iterate dense arrays (`br_component_count/at/owner_at`);
they never walk the node tree.

## 7. Input actions

Bind input in data: an action is a name plus keys, mouse buttons, gamepad buttons and gamepad
axis directions. Systems ask `br_input_action_value(e, "left")` (0..1) and do not care which
device it was.

<!-- file: data/game.json -->
```json
{
  "window": { "title": "Coin Rush", "width": 960, "height": 540, "resizable": false },
  "asset_root": "assets",
  "start_scene": "scenes/main.scene.json",
  "clear_color": "#14182b",
  "input": {
    "left":  { "keys": ["Left", "A"],  "buttons": ["dpleft"],  "axes": ["-leftx"] },
    "right": { "keys": ["Right", "D"], "buttons": ["dpright"], "axes": ["+leftx"] },
    "up":    { "keys": ["Up", "W"],    "buttons": ["dpup"],    "axes": ["-lefty"] },
    "down":  { "keys": ["Down", "S"],  "buttons": ["dpdown"],  "axes": ["+lefty"] }
  },
  "debug": { "overlay": false, "toggle_key": "F1" }
}
```

In the scene, give the ship a `pilot` component instead of a fixed velocity:

<!-- file: data/assets/scenes/main.scene.json -->
```json
{
  "type": "container",
  "label": "main",
  "children": [
    { "type": "text", "label": "score", "text": "COINS 0", "position": [20, 16],
      "style": { "font_size": 28, "bold": true, "fill": "#ffd166" } },
    { "type": "graphics", "label": "ship", "position": [480, 270],
      "commands": [["poly", [0, -18, 14, 14, 0, 6, -14, 14], true], ["fill", "#4fd1c5"],
                   ["stroke", { "width": 2, "color": "#0e3b46" }]],
      "components": { "mover": {}, "pilot": { "speed": 320 } } }
  ]
}
```

Add the `pilot` component and a system that turns actions into the mover's velocity:

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct mover {
    br_vec2 velocity;
} mover;

typedef struct pilot {
    float speed;
} pilot;

typedef struct game {
    uint32_t mover_id;
    uint32_t pilot_id;
} game;

static void pilot_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    br_vec2 dir = br_vec2_make(br_input_action_value(e, "right") - br_input_action_value(e, "left"),
                               br_input_action_value(e, "down") - br_input_action_value(e, "up"));
    for (uint32_t i = 0; i < br_component_count(e, g->pilot_id); i++) {
        pilot *p = br_component_at(e, g->pilot_id, i);
        mover *m = br_component_get(e, br_component_owner_at(e, g->pilot_id, i), g->mover_id);
        if (m != NULL) {
            m->velocity = br_vec2_scale(dir, p->speed);
        }
    }
}

static void mover_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    for (uint32_t i = 0; i < br_component_count(e, g->mover_id); i++) {
        mover *m = br_component_at(e, g->mover_id, i);
        br_node n = br_component_owner_at(e, g->mover_id, i);
        br_vec2 p = br_node_position(e, n);
        br_node_set_position(e, n, br_vec2_make(p.x + m->velocity.x * dt, p.y + m->velocity.y * dt));
    }
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, game_free, g);

    static const br_field_desc mover_fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
    };
    static const br_field_desc pilot_fields[] = {
        { "speed", BR_FIELD_FLOAT, offsetof(pilot, speed) },
    };
    br_component_desc mover_desc = { "mover", sizeof(mover), mover_fields, BR_ARRAY_COUNT(mover_fields) };
    br_component_desc pilot_desc = { "pilot", sizeof(pilot), pilot_fields, BR_ARRAY_COUNT(pilot_fields) };
    g->mover_id = br_component_register(e, &mover_desc);
    g->pilot_id = br_component_register(e, &pilot_desc);
    br_system_add(e, "game.pilot", BR_PHASE_UPDATE, 0, pilot_system, g);
    br_system_add(e, "game.mover", BR_PHASE_UPDATE, 10, mover_system, g); /* after pilot */

    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

Arrow keys, WASD, the d-pad and the left stick all steer. Scripts and tests can drive the same
path with `br_input_inject_key(e, SDL_SCANCODE_RIGHT, true)` (the play-test module does it for you: docs/PLAYTESTING.md).

## 8. Assets, a spritesheet, a prefab and a bundle

Borrow the sample game's spritesheet and sounds (they were generated by its
`tools/make_assets.py`):

```sh
cp -r <braty>/sample_projects/star_courier/data/assets/sprites my_game/data/assets/
cp -r <braty>/sample_projects/star_courier/data/assets/sounds my_game/data/assets/
```

A **manifest** names assets (aliases) and groups them into **bundles** that load in the
background; game.json's `"manifest"` registers it. A **prefab** is a scene file meant to be
instantiated many times. Texture references are `"sheet.json#frame"` (or an alias:
`"atlas#crate.png"`).

<!-- file: data/assets/manifest.json -->
```json
{ "bundles": [ { "name": "game", "assets": [
    { "alias": "atlas", "src": "sprites/atlas.json" },
    { "alias": "sfx", "src": "sounds/sfx.wav" } ] } ] }
```

<!-- file: data/assets/prefabs/coin.prefab.json -->
```json
{
  "type": "sprite",
  "label": "coin",
  "texture": "sprites/atlas.json#crate.png",
  "components": { "coin": { "radius": 18, "value": 1 } }
}
```

game.json gains the manifest and, for step 10, sound sprites (named time ranges of one file):

<!-- file: data/game.json -->
```json
{
  "window": { "title": "Coin Rush", "width": 960, "height": 540, "resizable": false },
  "asset_root": "assets",
  "manifest": "manifest.json",
  "start_scene": "scenes/main.scene.json",
  "clear_color": "#14182b",
  "input": {
    "left":  { "keys": ["Left", "A"],  "buttons": ["dpleft"],  "axes": ["-leftx"] },
    "right": { "keys": ["Right", "D"], "buttons": ["dpright"], "axes": ["+leftx"] },
    "up":    { "keys": ["Up", "W"],    "buttons": ["dpup"],    "axes": ["-lefty"] },
    "down":  { "keys": ["Down", "S"],  "buttons": ["dpdown"],  "axes": ["+lefty"] }
  },
  "audio": { "buses": { "sfx": 0.8 }, "sprites": { "sounds/sfx.wav": { "pickup": [0, 0.18] } } },
  "debug": { "overlay": false, "toggle_key": "F1" },
  "game": { "coins_on_screen": 3 }
}
```

The code for the bundle, the coins and the score follows in steps 9 and 10 (one file).

## 9. Text, and a game rule

Text nodes come from the scene (`"type": "text"`, styles in JSON); code only changes the
string. The rule system below spawns coins from the prefab once the bundle is loaded, tests
circles (ship vs coins) over the coin array, and updates the `score` text found by label.

## 10. Sound

`game.json` already gave `sounds/sfx.wav` a sprite named `pickup`; playing it is
`br_audio_play` with `play.sprite = "pickup"`. The complete `main.c` for steps 8-10:

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct mover {
    br_vec2 velocity;
} mover;

typedef struct pilot {
    float speed;
} pilot;

typedef struct coin {
    float   radius;
    int32_t value;
} coin;

typedef struct game {
    uint32_t  mover_id, pilot_id, coin_id;
    bool      ready;      /* the "game" bundle finished loading */
    br_sound *sfx;
    int32_t   score;
    uint32_t  rng;
} game;

static float random01(game *g)
{
    g->rng = g->rng * 1664525u + 1013904223u; /* deterministic: --frames runs repeat exactly */
    return (float)(g->rng >> 8) / 16777216.0f;
}

static void on_bundle(br_engine *e, const char *bundle, float progress, void *user)
{
    game *g = user;
    br_log_info("bundle %s: %.0f%%", bundle, (double)(progress * 100.0f));
    if (progress >= 1.0f) {
        g->sfx = br_sound_load(e, "sfx"); /* an alias from the manifest */
        g->ready = true;
    }
}

static void pilot_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    br_vec2 dir = br_vec2_make(br_input_action_value(e, "right") - br_input_action_value(e, "left"),
                               br_input_action_value(e, "down") - br_input_action_value(e, "up"));
    for (uint32_t i = 0; i < br_component_count(e, g->pilot_id); i++) {
        pilot *p = br_component_at(e, g->pilot_id, i);
        mover *m = br_component_get(e, br_component_owner_at(e, g->pilot_id, i), g->mover_id);
        if (m != NULL) {
            m->velocity = br_vec2_scale(dir, p->speed);
        }
    }
}

static void mover_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    for (uint32_t i = 0; i < br_component_count(e, g->mover_id); i++) {
        mover *m = br_component_at(e, g->mover_id, i);
        br_node n = br_component_owner_at(e, g->mover_id, i);
        br_vec2 p = br_node_position(e, n);
        br_node_set_position(e, n, br_vec2_make(p.x + m->velocity.x * dt, p.y + m->velocity.y * dt));
    }
}

static void coin_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    if (!g->ready) {
        return;
    }
    br_node ship = br_node_find_by_label(e, br_stage_root(e), "ship", true);
    br_vec2 at = br_node_position(e, ship);
    int alive = 0;
    for (uint32_t i = 0; i < br_component_count(e, g->coin_id); i++) {
        coin *c = br_component_at(e, g->coin_id, i);
        br_node n = br_component_owner_at(e, g->coin_id, i);
        if (!br_node_valid(e, n)) {
            continue; /* destroyed this frame: freed at the start of the next one */
        }
        if (br_vec2_length(br_vec2_sub(br_node_position(e, n), at)) < c->radius + 16.0f) {
            g->score += c->value;
            br_node_destroy(e, n);
            br_audio_play_desc play = br_audio_play_defaults();
            play.sprite = "pickup";
            br_audio_play(e, g->sfx, &play);
            char text[32];
            SDL_snprintf(text, sizeof(text), "COINS %d", (int)g->score);
            br_text_set_text(e, br_node_find_by_label(e, br_stage_root(e), "score", true), text);
        } else {
            alive++;
        }
    }
    const cJSON *wanted = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game"), "coins_on_screen");
    for (; cJSON_IsNumber(wanted) && alive < wanted->valueint; alive++) {
        br_node n = br_prefab_instantiate(e, "prefabs/coin.prefab.json", br_stage_root(e), NULL);
        br_node_set_position(e, n, br_vec2_make(60.0f + random01(g) * 840.0f, 80.0f + random01(g) * 420.0f));
    }
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    g->rng = 12345u;
    br_engine_on_shutdown(e, game_free, g);

    static const br_field_desc mover_fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
    };
    static const br_field_desc pilot_fields[] = {
        { "speed", BR_FIELD_FLOAT, offsetof(pilot, speed) },
    };
    static const br_field_desc coin_fields[] = {
        { "radius", BR_FIELD_FLOAT, offsetof(coin, radius) },
        { "value", BR_FIELD_INT, offsetof(coin, value) },
    };
    br_component_desc mover_desc = { "mover", sizeof(mover), mover_fields, BR_ARRAY_COUNT(mover_fields) };
    br_component_desc pilot_desc = { "pilot", sizeof(pilot), pilot_fields, BR_ARRAY_COUNT(pilot_fields) };
    br_component_desc coin_desc = { "coin", sizeof(coin), coin_fields, BR_ARRAY_COUNT(coin_fields) };
    g->mover_id = br_component_register(e, &mover_desc);
    g->pilot_id = br_component_register(e, &pilot_desc);
    g->coin_id = br_component_register(e, &coin_desc);
    br_system_add(e, "game.pilot", BR_PHASE_UPDATE, 0, pilot_system, g);
    br_system_add(e, "game.mover", BR_PHASE_UPDATE, 10, mover_system, g);
    br_system_add(e, "game.coins", BR_PHASE_UPDATE, 20, coin_system, g);

    br_assets_load_bundle(e, "game", on_bundle, g); /* game.json's manifest is registered already */
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

Coins appear a frame after the bundle finishes; fly over them to collect them.

## 11. The debug overlay

Run with `--debug` (or set `"debug": { "overlay": true }`), or press F1 in a running game:
FPS and frame-time graph, draw calls and batches, node / texture / asset counts, every system
with its last run time and an on/off checkbox, the scene tree with a live inspector, and the
log. `"toggle_key"` in game.json picks another key.

```sh
my_game/build/my_game --debug --frames 120 --screenshot debug.png
```

## 12. Localisation: texts from a string table

Player-visible text belongs in a **string table** per language, `strings/strings.<locale>.json`:
a flat object of keys; a value is a text with `{name}` placeholders or a plural entry (`zero` /
`one` / `other`). game.json's `"locale"` loads the default table before `init` (and picks the
system language when a table for it exists; `--frames`, `--deterministic` and headless runs always
use the default, so screenshots do not depend on the machine).

<!-- file: data/assets/strings/strings.en.json -->
```json
{
  "hud.title": "COIN RUSH",
  "hud.coins": { "zero": "NO COINS YET", "one": "{count} COIN", "other": "{count} COINS" },
  "hud.name_hint": "Your name, then Enter",
  "hud.hello": "Hello, {name}!"
}
```

<!-- file: data/assets/strings/strings.de.json -->
```json
{
  "hud.title": "COIN RUSH",
  "hud.coins": { "zero": "NOCH KEINE MÜNZEN", "one": "{count} MÜNZE", "other": "{count} MÜNZEN" },
  "hud.name_hint": "Dein Name, dann Enter",
  "hud.hello": "Hallo, {name}!"
}
```

<!-- file: data/game.json -->
```json
{
  "window": { "title": "Coin Rush", "width": 960, "height": 540, "resizable": false },
  "asset_root": "assets",
  "manifest": "manifest.json",
  "start_scene": "scenes/main.scene.json",
  "clear_color": "#14182b",
  "input": {
    "left":  { "keys": ["Left", "A"],  "buttons": ["dpleft"],  "axes": ["-leftx"] },
    "right": { "keys": ["Right", "D"], "buttons": ["dpright"], "axes": ["+leftx"] },
    "up":    { "keys": ["Up", "W"],    "buttons": ["dpup"],    "axes": ["-lefty"] },
    "down":  { "keys": ["Down", "S"],  "buttons": ["dpdown"],  "axes": ["+lefty"] }
  },
  "audio": { "buses": { "sfx": 0.8 }, "sprites": { "sounds/sfx.wav": { "pickup": [0, 0.18] } } },
  "locale": { "default": "en", "fallback": ["en"], "dir": "strings" },
  "debug": { "overlay": false, "toggle_key": "F1" },
  "game": { "coins_on_screen": 3 }
}
```

A scene text names its key with `"text_key"` instead of `"text"`: it follows the language when
it changes (`br_loc_set_locale(e, "de")`, or `set engine:locale "de"` in a play-test). Text made
by code goes through `br_loc_format(e, key, args, count)`; the argument `count` picks the plural
form. (Keep `"text_key"` for texts code does not change: a locale change sets a bound node back
to its key's text.)

<!-- file: data/assets/scenes/main.scene.json -->
```json
{
  "type": "container",
  "label": "main",
  "children": [
    { "type": "text", "label": "score", "text": "", "position": [20, 16],
      "style": { "font_size": 28, "bold": true, "fill": "#ffd166" } },
    { "type": "text", "text_key": "hud.title", "position": [940, 16], "anchor": [1, 0],
      "style": { "font_size": 20, "fill": "#8b93b8" } },
    { "type": "graphics", "label": "ship", "position": [480, 270],
      "commands": [["poly", [0, -18, 14, 14, 0, 6, -14, 14], true], ["fill", "#4fd1c5"],
                   ["stroke", { "width": 2, "color": "#0e3b46" }]],
      "components": { "mover": {}, "pilot": { "speed": 320 } } }
  ]
}
```

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct mover {
    br_vec2 velocity;
} mover;

typedef struct pilot {
    float speed;
} pilot;

typedef struct coin {
    float   radius;
    int32_t value;
} coin;

typedef struct game {
    uint32_t  mover_id, pilot_id, coin_id;
    bool      ready;      /* the "game" bundle finished loading */
    br_sound *sfx;
    int32_t   score;
    int32_t   shown;      /* the score the text shows (-1 = none yet) */
    uint32_t  rng;
} game;

static float random01(game *g)
{
    g->rng = g->rng * 1664525u + 1013904223u; /* deterministic: --frames runs repeat exactly */
    return (float)(g->rng >> 8) / 16777216.0f;
}

static void on_bundle(br_engine *e, const char *bundle, float progress, void *user)
{
    game *g = user;
    br_log_info("bundle %s: %.0f%%", bundle, (double)(progress * 100.0f));
    if (progress >= 1.0f) {
        g->sfx = br_sound_load(e, "sfx"); /* an alias from the manifest */
        g->ready = true;
    }
}

static void pilot_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    br_vec2 dir = br_vec2_make(br_input_action_value(e, "right") - br_input_action_value(e, "left"),
                               br_input_action_value(e, "down") - br_input_action_value(e, "up"));
    for (uint32_t i = 0; i < br_component_count(e, g->pilot_id); i++) {
        pilot *p = br_component_at(e, g->pilot_id, i);
        mover *m = br_component_get(e, br_component_owner_at(e, g->pilot_id, i), g->mover_id);
        if (m != NULL) {
            m->velocity = br_vec2_scale(dir, p->speed);
        }
    }
}

static void mover_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    for (uint32_t i = 0; i < br_component_count(e, g->mover_id); i++) {
        mover *m = br_component_at(e, g->mover_id, i);
        br_node n = br_component_owner_at(e, g->mover_id, i);
        br_vec2 p = br_node_position(e, n);
        br_node_set_position(e, n, br_vec2_make(p.x + m->velocity.x * dt, p.y + m->velocity.y * dt));
    }
}

static void coin_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    if (!g->ready) {
        return;
    }
    br_node ship = br_node_find_by_label(e, br_stage_root(e), "ship", true);
    br_vec2 at = br_node_position(e, ship);
    int alive = 0;
    for (uint32_t i = 0; i < br_component_count(e, g->coin_id); i++) {
        coin *c = br_component_at(e, g->coin_id, i);
        br_node n = br_component_owner_at(e, g->coin_id, i);
        if (!br_node_valid(e, n)) {
            continue; /* destroyed this frame: freed at the start of the next one */
        }
        if (br_vec2_length(br_vec2_sub(br_node_position(e, n), at)) < c->radius + 16.0f) {
            g->score += c->value;
            br_node_destroy(e, n);
            br_audio_play_desc play = br_audio_play_defaults();
            play.sprite = "pickup";
            br_audio_play(e, g->sfx, &play);
        } else {
            alive++;
        }
    }
    if (g->shown != g->score) {
        /* "hud.coins" is a plural entry: "NO COINS YET" / "1 COIN" / "3 COINS" */
        char count[16];
        SDL_snprintf(count, sizeof(count), "%d", (int)g->score);
        br_loc_arg arg = { "count", count };
        br_text_set_text(e, br_node_find_by_label(e, br_stage_root(e), "score", true), br_loc_format(e, "hud.coins", &arg, 1));
        g->shown = g->score;
    }
    const cJSON *wanted = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game"), "coins_on_screen");
    for (; cJSON_IsNumber(wanted) && alive < wanted->valueint; alive++) {
        br_node n = br_prefab_instantiate(e, "prefabs/coin.prefab.json", br_stage_root(e), NULL);
        br_node_set_position(e, n, br_vec2_make(60.0f + random01(g) * 840.0f, 80.0f + random01(g) * 420.0f));
    }
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    g->rng = 12345u;
    g->shown = -1;
    br_engine_on_shutdown(e, game_free, g);

    static const br_field_desc mover_fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
    };
    static const br_field_desc pilot_fields[] = {
        { "speed", BR_FIELD_FLOAT, offsetof(pilot, speed) },
    };
    static const br_field_desc coin_fields[] = {
        { "radius", BR_FIELD_FLOAT, offsetof(coin, radius) },
        { "value", BR_FIELD_INT, offsetof(coin, value) },
    };
    br_component_desc mover_desc = { "mover", sizeof(mover), mover_fields, BR_ARRAY_COUNT(mover_fields) };
    br_component_desc pilot_desc = { "pilot", sizeof(pilot), pilot_fields, BR_ARRAY_COUNT(pilot_fields) };
    br_component_desc coin_desc = { "coin", sizeof(coin), coin_fields, BR_ARRAY_COUNT(coin_fields) };
    g->mover_id = br_component_register(e, &mover_desc);
    g->pilot_id = br_component_register(e, &pilot_desc);
    g->coin_id = br_component_register(e, &coin_desc);
    br_system_add(e, "game.pilot", BR_PHASE_UPDATE, 0, pilot_system, g);
    br_system_add(e, "game.mover", BR_PHASE_UPDATE, 10, mover_system, g);
    br_system_add(e, "game.coins", BR_PHASE_UPDATE, 20, coin_system, g);

    br_assets_load_bundle(e, "game", on_bundle, g); /* game.json's manifest is registered already */
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

A missing key shows as the key itself and is logged once, so a typo is easy to spot. Limits:
plural forms are zero / one / other only, no right-to-left text and no shaping (README, Limits).

## 13. Shipping: a pack and an install folder

A released game is the executable, game.json and one `.brpack` holding everything under
`asset_root` (no compression; `docs/DATA_FORMATS.md` section 8). `braty_install_game` adds an
install rule that packs `data/assets` with `tools/pack_assets.py` and writes the folder:

<!-- file: CMakeLists.txt -->
```cmake
cmake_minimum_required(VERSION 3.24)
project(my_game LANGUAGES C)

set(BRATY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../braty" CACHE PATH "Braty engine checkout")
set(BRATY_BUILD_TESTS OFF)      # only the library, not the engine's tests,
set(BRATY_BUILD_EXAMPLES OFF)   # examples
set(BRATY_BUILD_SAMPLES OFF)    # and sample games
add_subdirectory(${BRATY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/braty)

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_executable(my_game ${GAME_SOURCES})
set_target_properties(my_game PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(my_game PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wpedantic>)
target_link_libraries(my_game PRIVATE braty::braty)

# data/ next to the executable: the game runs from any working directory
add_custom_command(TARGET my_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:my_game>)

# cmake --install <build> --prefix <folder>: <folder>/my_game/ = the executable, the SDL runtime
# (if shared), game.json and assets.brpack (data/assets packed by tools/pack_assets.py)
braty_install_game(my_game ${CMAKE_CURRENT_SOURCE_DIR}/data)
```

game.json lists the pack. A pack that is not there is skipped with an info line, so the
development build keeps reading the loose files copied next to it:

<!-- file: data/game.json -->
```json
{
  "window": { "title": "Coin Rush", "width": 960, "height": 540, "resizable": false },
  "asset_root": "assets",
  "packs": ["assets.brpack"],
  "manifest": "manifest.json",
  "start_scene": "scenes/main.scene.json",
  "clear_color": "#14182b",
  "input": {
    "left":  { "keys": ["Left", "A"],  "buttons": ["dpleft"],  "axes": ["-leftx"] },
    "right": { "keys": ["Right", "D"], "buttons": ["dpright"], "axes": ["+leftx"] },
    "up":    { "keys": ["Up", "W"],    "buttons": ["dpup"],    "axes": ["-lefty"] },
    "down":  { "keys": ["Down", "S"],  "buttons": ["dpdown"],  "axes": ["+lefty"] }
  },
  "audio": { "buses": { "sfx": 0.8 }, "sprites": { "sounds/sfx.wav": { "pickup": [0, 0.18] } } },
  "locale": { "default": "en", "fallback": ["en"], "dir": "strings" },
  "debug": { "overlay": false, "toggle_key": "F1" },
  "game": { "coins_on_screen": 3 }
}
```

```sh
cmake --install my_game/build --prefix dist       # dist/my_game/: my_game(.exe), game.json, assets.brpack
dist/my_game/my_game --frames 60 --screenshot shot.png
python <braty>/tools/pack_assets.py my_game/data/assets my.brpack   # a pack by hand
my_game/build/my_game --pack my.brpack            # mount it first (wins over loose files)
```

Pack paths are case-sensitive and at most 255 bytes. If the game cannot start (no game.json, a
broken pack, no GPU) a release build shows a message box and writes `log.txt` next to the
executable.

## 14. A text field

`br_text_field_create` makes an editable box (caret, selection, clipboard, undo, IME). While it
has the keyboard, game actions do not see the typed keys, so "wasd" in a name does not fly the
ship. The listener hears CHANGED, SUBMITTED (Enter), FOCUSED and BLURRED. Add a greeting text
to the scene:

<!-- file: data/assets/scenes/main.scene.json -->
```json
{
  "type": "container",
  "label": "main",
  "children": [
    { "type": "text", "label": "score", "text": "", "position": [20, 16],
      "style": { "font_size": 28, "bold": true, "fill": "#ffd166" } },
    { "type": "text", "text_key": "hud.title", "position": [940, 16], "anchor": [1, 0],
      "style": { "font_size": 20, "fill": "#8b93b8" } },
    { "type": "text", "label": "greeting", "text": "", "position": [300, 506], "anchor": [0, 0.5],
      "style": { "font_size": 22, "fill": "#ffffff" } },
    { "type": "graphics", "label": "ship", "position": [480, 270],
      "commands": [["poly", [0, -18, 14, 14, 0, 6, -14, 14], true], ["fill", "#4fd1c5"],
                   ["stroke", { "width": 2, "color": "#0e3b46" }]],
      "components": { "mover": {}, "pilot": { "speed": 320 } } }
  ]
}
```

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct mover {
    br_vec2 velocity;
} mover;

typedef struct pilot {
    float speed;
} pilot;

typedef struct coin {
    float   radius;
    int32_t value;
} coin;

typedef struct game {
    uint32_t  mover_id, pilot_id, coin_id;
    bool      ready;      /* the "game" bundle finished loading */
    br_sound *sfx;
    int32_t   score;
    int32_t   shown;      /* the score the text shows (-1 = none yet) */
    uint32_t  rng;
} game;

static float random01(game *g)
{
    g->rng = g->rng * 1664525u + 1013904223u; /* deterministic: --frames runs repeat exactly */
    return (float)(g->rng >> 8) / 16777216.0f;
}

static void on_bundle(br_engine *e, const char *bundle, float progress, void *user)
{
    game *g = user;
    br_log_info("bundle %s: %.0f%%", bundle, (double)(progress * 100.0f));
    if (progress >= 1.0f) {
        g->sfx = br_sound_load(e, "sfx"); /* an alias from the manifest */
        g->ready = true;
    }
}

static void pilot_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    br_vec2 dir = br_vec2_make(br_input_action_value(e, "right") - br_input_action_value(e, "left"),
                               br_input_action_value(e, "down") - br_input_action_value(e, "up"));
    for (uint32_t i = 0; i < br_component_count(e, g->pilot_id); i++) {
        pilot *p = br_component_at(e, g->pilot_id, i);
        mover *m = br_component_get(e, br_component_owner_at(e, g->pilot_id, i), g->mover_id);
        if (m != NULL) {
            m->velocity = br_vec2_scale(dir, p->speed);
        }
    }
}

static void mover_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    for (uint32_t i = 0; i < br_component_count(e, g->mover_id); i++) {
        mover *m = br_component_at(e, g->mover_id, i);
        br_node n = br_component_owner_at(e, g->mover_id, i);
        br_vec2 p = br_node_position(e, n);
        br_node_set_position(e, n, br_vec2_make(p.x + m->velocity.x * dt, p.y + m->velocity.y * dt));
    }
}

static void coin_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    if (!g->ready) {
        return;
    }
    br_node ship = br_node_find_by_label(e, br_stage_root(e), "ship", true);
    br_vec2 at = br_node_position(e, ship);
    int alive = 0;
    for (uint32_t i = 0; i < br_component_count(e, g->coin_id); i++) {
        coin *c = br_component_at(e, g->coin_id, i);
        br_node n = br_component_owner_at(e, g->coin_id, i);
        if (!br_node_valid(e, n)) {
            continue; /* destroyed this frame: freed at the start of the next one */
        }
        if (br_vec2_length(br_vec2_sub(br_node_position(e, n), at)) < c->radius + 16.0f) {
            g->score += c->value;
            br_node_destroy(e, n);
            br_audio_play_desc play = br_audio_play_defaults();
            play.sprite = "pickup";
            br_audio_play(e, g->sfx, &play);
        } else {
            alive++;
        }
    }
    if (g->shown != g->score) {
        /* "hud.coins" is a plural entry: "NO COINS YET" / "1 COIN" / "3 COINS" */
        char count[16];
        SDL_snprintf(count, sizeof(count), "%d", (int)g->score);
        br_loc_arg arg = { "count", count };
        br_text_set_text(e, br_node_find_by_label(e, br_stage_root(e), "score", true), br_loc_format(e, "hud.coins", &arg, 1));
        g->shown = g->score;
    }
    const cJSON *wanted = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game"), "coins_on_screen");
    for (; cJSON_IsNumber(wanted) && alive < wanted->valueint; alive++) {
        br_node n = br_prefab_instantiate(e, "prefabs/coin.prefab.json", br_stage_root(e), NULL);
        br_node_set_position(e, n, br_vec2_make(60.0f + random01(g) * 840.0f, 80.0f + random01(g) * 420.0f));
    }
}

/* Enter in the name field: greet the player (the field keeps the keyboard until then, so typing
 * "wasd" does not fly the ship). */
static void on_name(br_engine *e, br_node field, br_text_field_event event, void *user)
{
    (void)user;
    if (event != BR_TEXT_FIELD_SUBMITTED) {
        return;
    }
    br_loc_arg arg = { "name", br_text_field_text(e, field) };
    br_text_set_text(e, br_node_find_by_label(e, br_stage_root(e), "greeting", true), br_loc_format(e, "hud.hello", &arg, 1));
    br_node none = { 0, 0 };
    br_text_field_focus(e, none); /* give the keyboard back to the game */
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    g->rng = 12345u;
    g->shown = -1;
    br_engine_on_shutdown(e, game_free, g);

    static const br_field_desc mover_fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
    };
    static const br_field_desc pilot_fields[] = {
        { "speed", BR_FIELD_FLOAT, offsetof(pilot, speed) },
    };
    static const br_field_desc coin_fields[] = {
        { "radius", BR_FIELD_FLOAT, offsetof(coin, radius) },
        { "value", BR_FIELD_INT, offsetof(coin, value) },
    };
    br_component_desc mover_desc = { "mover", sizeof(mover), mover_fields, BR_ARRAY_COUNT(mover_fields) };
    br_component_desc pilot_desc = { "pilot", sizeof(pilot), pilot_fields, BR_ARRAY_COUNT(pilot_fields) };
    br_component_desc coin_desc = { "coin", sizeof(coin), coin_fields, BR_ARRAY_COUNT(coin_fields) };
    g->mover_id = br_component_register(e, &mover_desc);
    g->pilot_id = br_component_register(e, &pilot_desc);
    g->coin_id = br_component_register(e, &coin_desc);
    br_system_add(e, "game.pilot", BR_PHASE_UPDATE, 0, pilot_system, g);
    br_system_add(e, "game.mover", BR_PHASE_UPDATE, 10, mover_system, g);
    br_system_add(e, "game.coins", BR_PHASE_UPDATE, 20, coin_system, g);

    br_text_style style = br_text_style_default();
    style.font_size = 20;
    br_node name = br_text_field_create(e, br_vec2_make(260.0f, 36.0f), &style);
    br_node_set_label(e, name, "name");
    br_node_set_position(e, name, br_vec2_make(20.0f, 488.0f));
    br_text_field_set_max_length(e, name, 16);
    br_text_field_set_placeholder(e, name, br_loc_get(e, "hud.name_hint")); /* game.json "locale" is set up already */
    br_text_field_set_listener(e, name, on_name, g);
    br_node_add_child(e, br_stage_root(e), name);

    br_assets_load_bundle(e, "game", on_bundle, g); /* game.json's manifest is registered already */
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

Click the box (or let your menu call `br_text_field_focus` when its "confirm" lands on it),
type, press Enter. A play-test types for you: `type name "Ada"` focuses the field labelled `name`
and sends the text.

<!-- file: playtest/name.json -->
```json
[
  {"cmd": "wait", "frames": 5},
  {"cmd": "expect", "path": "score:text", "value": "NO COINS YET"},
  {"cmd": "type", "label": "name", "text": "Ada"},
  {"cmd": "expect", "path": "name:text", "value": "Ada"},
  {"cmd": "key", "key": "Return"},
  {"cmd": "wait", "frames": 2},
  {"cmd": "expect", "path": "greeting:text", "value": "Hello, Ada!"},
  {"cmd": "set", "path": "engine:locale", "value": "de"},
  {"cmd": "expect", "path": "loc:hud.hello", "value": "Hallo, {name}!"},
  {"cmd": "quit"}
]
```

```sh
my_game/build/my_game --headless --playtest my_game/playtest/name.json
```

## The 3D track

The same ideas in 3D, with glTF as the scene format. A level made in Blender (or, here, by Crate
Rush's `tools/build_assets.py`, which writes exactly what a Blender export contains) is a `.glb`:
meshes, a camera, a sun, and **node extras** (Blender custom properties) that become components.
Steps 15-18 replace `main.c` again; game.json keeps its input actions.

```sh
mkdir -p my_game/data/assets/levels my_game/data/assets/models
cp <braty>/sample_projects/crate_rush/data/assets/levels/level1.glb my_game/data/assets/levels/
cp -r <braty>/sample_projects/crate_rush/data/assets/models/character my_game/data/assets/models/
```

## 15. A .glb as the scene

A `.glb` path works wherever a scene file does: `start_scene`, `br_scene_load`, `br_scene_switch`
and `br_prefab_instantiate`. As a scene it becomes a full-window View3D with the file's nodes,
lights and first camera; the scene extras (`background`, `sky`, `ground`, `ambient`, `msaa`) set
up the view.

<!-- file: data/game.json -->
```json
{
  "window": { "title": "Coin Rush 3D", "width": 960, "height": 540, "resizable": false },
  "asset_root": "assets",
  "start_scene": "levels/level1.glb",
  "input": {
    "left":  { "keys": ["Left", "A"],  "buttons": ["dpleft"],  "axes": ["-leftx"] },
    "right": { "keys": ["Right", "D"], "buttons": ["dpright"], "axes": ["+leftx"] },
    "up":    { "keys": ["Up", "W"],    "buttons": ["dpup"],    "axes": ["-lefty"] },
    "down":  { "keys": ["Down", "S"],  "buttons": ["dpdown"],  "axes": ["+lefty"] }
  }
}
```

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

static bool game_init(br_engine *e)
{
    (void)e; /* the level loads after init: nothing to set up yet */
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

The screenshot shows the island through the file's camera, with shadows from its sun.

## 16. A component from glTF extras, and a system over 3D components

Every crate node in `level1.glb` has the custom property `"crate.value": 10` (in the file:
`"extras": { "crate.value": 10 }`). Register a `crate` component before the level loads and the
loader fills it on the crate's 3D node. 3D components have their own dense arrays:
`br_component3d_count / _at / _owner_at`.

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct crate {
    int32_t value;
} crate;

typedef struct game {
    uint32_t crate_id;
} game;

/* Every node that got a "crate" component from its extras turns slowly. */
static void crate_spin_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    br_quat turn = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), 1.5f * dt);
    for (uint32_t i = 0; i < br_component3d_count(e, g->crate_id); i++) {
        br_node3d n = br_component3d_owner_at(e, g->crate_id, i);
        if (br_node3d_valid(e, n)) {
            br_node3d_set_rotation(e, n, br_quat_mul(turn, br_node3d_rotation(e, n)));
        }
    }
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, game_free, g);
    static const br_field_desc crate_fields[] = {
        { "value", BR_FIELD_INT, offsetof(crate, value) },
    };
    br_component_desc desc = { "crate", sizeof(crate), crate_fields, BR_ARRAY_COUNT(crate_fields) };
    g->crate_id = br_component_register(e, &desc); /* before the level loads */
    br_system_add(e, "game.crate_spin", BR_PHASE_UPDATE, 0, crate_spin_system, g);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

Extras the game did not register (the level's `hazard`, `sign`, ...) are skipped silently.

## 17. A character and a following camera

The level's `Spawn` node carries `"spawn_point": { "yaw": 180 }`. The game puts the skinned
character there, moves it with the input actions and plays its `idle` / `sprint` clips. With
`br_gltf_options.camera_as_br_camera` set before the level loads, the `.glb`'s own camera (its
lens, sky and background from the scene extras) arrives as a Unity-style `br_camera` labelled
after its glTF node (`Camera`): give it a `camera.follow` component and it follows the character
and looks at it. (`br_camera_create_3d` makes one by hand when a file has no camera.)

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct crate {
    int32_t value;
} crate;

typedef struct spawn_point {
    float yaw;
} spawn_point;

typedef struct game {
    uint32_t    crate_id, spawn_id;
    br_node3d   player;
    br_animator anim;
    bool        running;
} game;

static void crate_spin_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    br_quat turn = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), 1.5f * dt);
    for (uint32_t i = 0; i < br_component3d_count(e, g->crate_id); i++) {
        br_node3d n = br_component3d_owner_at(e, g->crate_id, i);
        if (br_node3d_valid(e, n)) {
            br_node3d_set_rotation(e, n, br_quat_mul(turn, br_node3d_rotation(e, n)));
        }
    }
}

/* Once the level is there: the character at the spawn point, a camera following it. */
static void spawn(br_engine *e, game *g)
{
    br_scene level = br_scene_active(e);
    br_node3d root = br_scene_root_3d(e, level);
    br_vec3 at = br_vec3_make(0.0f, 1.0f, 0.0f);
    if (br_component3d_count(e, g->spawn_id) > 0) {
        at = br_node3d_world_position(e, br_component3d_owner_at(e, g->spawn_id, 0));
    }
    g->player = br_gltf_instantiate(e, "models/character/character-worker.glb", root, NULL);
    br_node3d_set_position(e, g->player, at);
    g->anim = br_animator_add(e, br_anim_target_3d(g->player)); /* plays the model's clips */
    br_animator_add_model_clips(e, g->anim, br_model3d_model(e, g->player));
    br_animator_play(e, g->anim, "idle");

    /* the file's camera is a br_camera (camera_as_br_camera in game_init), labelled after its
     * glTF node: it follows the character */
    br_node cam = br_node_find_by_label(e, br_scene_root_2d(e, level), "Camera", true);
    br_camera_follow *f = br_component_add(e, cam, br_camera_follow_component(e));
    f->target3d = g->player;
    f->offset3d = br_vec3_make(0.0f, 4.5f, 5.5f);
    f->damping = 0.15f;
    br_node3d_set_position(e, br_camera_node3d(e, cam), br_vec3_add(at, f->offset3d));
}

static void player_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    if (!br_node3d_valid(e, g->player)) {
        if (br_scene_is_loaded(e, br_scene_active(e))) {
            spawn(e, g);
        }
        return;
    }
    br_vec2 stick = br_input_action_vec2(e, "left", "right", "up", "down");
    bool running = br_vec2_length(stick) > 0.1f;
    if (running) {
        br_vec3 p = br_node3d_position(e, g->player);
        br_node3d_set_position(e, g->player, br_vec3_make(p.x + stick.x * 4.0f * dt, p.y, p.z + stick.y * 4.0f * dt));
        /* the model faces +Z; turn it toward the movement */
        br_node3d_set_rotation(e, g->player, br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f),
                                                                     SDL_atan2f(stick.x, stick.y)));
    }
    if (running != g->running) {
        br_animator_cross_fade(e, g->anim, running ? "sprint" : "idle", 0.15f);
        g->running = running;
    }
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, game_free, g);
    static const br_field_desc crate_fields[] = { { "value", BR_FIELD_INT, offsetof(crate, value) } };
    static const br_field_desc spawn_fields[] = { { "yaw", BR_FIELD_FLOAT, offsetof(spawn_point, yaw) } };
    br_component_desc crate_desc = { "crate", sizeof(crate), crate_fields, BR_ARRAY_COUNT(crate_fields) };
    br_component_desc spawn_desc = { "spawn_point", sizeof(spawn_point), spawn_fields, BR_ARRAY_COUNT(spawn_fields) };
    g->crate_id = br_component_register(e, &crate_desc);
    g->spawn_id = br_component_register(e, &spawn_desc);
    br_gltf_options options;
    SDL_zero(options);
    options.camera_as_br_camera = true; /* .glb scenes: the file's camera becomes a br_camera */
    br_gltf_set_load_options(e, &options);
    br_system_add(e, "game.crate_spin", BR_PHASE_UPDATE, 0, crate_spin_system, g);
    br_system_add(e, "game.player", BR_PHASE_UPDATE, 10, player_system, g);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

There is no collision here (the character walks over water): Crate Rush's `src/player.c` shows
a fixed-step version with gravity, jumping and box colliders read from the level's `-colonly`
nodes.

## 18. A world-space label

`br_canvas3d` puts 2D content (sprites, nine-slices, text) into the 3D world. `br_text3d_create`
is a canvas with one text node; with no font given it uses the default font as SDF glyphs, so the
label stays sharp at any distance. Billboarded, it always faces the camera. The label counts the
crates the character walks into (they are destroyed: their `crate` rows go with them).

<!-- file: src/main.c -->
```c
#include "braty/braty_main.h"

typedef struct crate {
    int32_t value;
} crate;

typedef struct spawn_point {
    float yaw;
} spawn_point;

typedef struct game {
    uint32_t    crate_id, spawn_id;
    br_node3d   player;
    br_animator anim;
    br_node3d   label;
    bool        running;
    int32_t     score;
} game;

static void crate_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    br_quat turn = br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f), 1.5f * dt);
    br_vec3 me = br_node3d_world_position(e, g->player);
    for (uint32_t i = 0; i < br_component3d_count(e, g->crate_id); i++) {
        const crate *c = br_component3d_at(e, g->crate_id, i);
        br_node3d n = br_component3d_owner_at(e, g->crate_id, i);
        if (!br_node3d_valid(e, n)) {
            continue; /* destroyed this frame */
        }
        br_node3d_set_rotation(e, n, br_quat_mul(turn, br_node3d_rotation(e, n)));
        br_vec3 d = br_vec3_sub(br_node3d_world_position(e, n), me);
        if (br_node3d_valid(e, g->player) && d.x * d.x + d.z * d.z < 0.5f && d.y > -0.5f && d.y < 0.5f) {
            g->score += c->value;
            br_node3d_destroy(e, n);
            char text[32];
            SDL_snprintf(text, sizeof(text), "SCORE %d", (int)g->score);
            br_text_set_text(e, br_canvas3d_content(e, g->label), text);
        }
    }
}

static void spawn(br_engine *e, game *g)
{
    br_scene level = br_scene_active(e);
    br_node3d root = br_scene_root_3d(e, level);
    br_vec3 at = br_vec3_make(0.0f, 1.0f, 0.0f);
    if (br_component3d_count(e, g->spawn_id) > 0) {
        at = br_node3d_world_position(e, br_component3d_owner_at(e, g->spawn_id, 0));
    }
    g->player = br_gltf_instantiate(e, "models/character/character-worker.glb", root, NULL);
    br_node3d_set_position(e, g->player, at);
    g->anim = br_animator_add(e, br_anim_target_3d(g->player)); /* plays the model's clips */
    br_animator_add_model_clips(e, g->anim, br_model3d_model(e, g->player));
    br_animator_play(e, g->anim, "idle");

    /* "SCORE 0" over the character's head: SDF text on a billboarded canvas */
    br_text_style style = br_text_style_default();
    style.font_size = 48.0f;
    style.fill = br_color_from_hex(0xffd34a, 1.0f);
    style.stroke_width = 8.0f;
    style.stroke_color = br_color_from_hex(0x2a1d10, 1.0f);
    g->label = br_text3d_create(e, "SCORE 0", &style, 150.0f);
    br_canvas3d_get(e, g->label)->billboard = BR_CANVAS3D_BILLBOARD_FULL;
    br_node3d_set_position(e, g->label, br_vec3_make(0.0f, 1.1f, 0.0f));
    br_node3d_add_child(e, g->player, g->label);

    br_node cam = br_node_find_by_label(e, br_scene_root_2d(e, level), "Camera", true);
    br_camera_follow *f = br_component_add(e, cam, br_camera_follow_component(e));
    f->target3d = g->player;
    f->offset3d = br_vec3_make(0.0f, 4.5f, 5.5f);
    f->damping = 0.15f;
    br_node3d_set_position(e, br_camera_node3d(e, cam), br_vec3_add(at, f->offset3d));
}

static void player_system(br_engine *e, float dt, void *user)
{
    game *g = user;
    if (!br_node3d_valid(e, g->player)) {
        if (br_scene_is_loaded(e, br_scene_active(e))) {
            spawn(e, g);
        }
        return;
    }
    br_vec2 stick = br_input_action_vec2(e, "left", "right", "up", "down");
    bool running = br_vec2_length(stick) > 0.1f;
    if (running) {
        br_vec3 p = br_node3d_position(e, g->player);
        br_node3d_set_position(e, g->player, br_vec3_make(p.x + stick.x * 4.0f * dt, p.y, p.z + stick.y * 4.0f * dt));
        br_node3d_set_rotation(e, g->player, br_quat_from_axis_angle(br_vec3_make(0.0f, 1.0f, 0.0f),
                                                                     SDL_atan2f(stick.x, stick.y)));
    }
    if (running != g->running) {
        br_animator_cross_fade(e, g->anim, running ? "sprint" : "idle", 0.15f);
        g->running = running;
    }
}

static void game_free(br_engine *e, void *user)
{
    (void)e;
    br_free(user);
}

static bool game_init(br_engine *e)
{
    game *g = br_alloc(sizeof(game));
    if (g == NULL) {
        return false;
    }
    br_engine_on_shutdown(e, game_free, g);
    static const br_field_desc crate_fields[] = { { "value", BR_FIELD_INT, offsetof(crate, value) } };
    static const br_field_desc spawn_fields[] = { { "yaw", BR_FIELD_FLOAT, offsetof(spawn_point, yaw) } };
    br_component_desc crate_desc = { "crate", sizeof(crate), crate_fields, BR_ARRAY_COUNT(crate_fields) };
    br_component_desc spawn_desc = { "spawn_point", sizeof(spawn_point), spawn_fields, BR_ARRAY_COUNT(spawn_fields) };
    g->crate_id = br_component_register(e, &crate_desc);
    g->spawn_id = br_component_register(e, &spawn_desc);
    br_gltf_options options;
    SDL_zero(options);
    options.camera_as_br_camera = true; /* .glb scenes: the file's camera becomes a br_camera */
    br_gltf_set_load_options(e, &options);
    br_system_add(e, "game.player", BR_PHASE_UPDATE, 0, player_system, g);
    br_system_add(e, "game.crates", BR_PHASE_UPDATE, 10, crate_system, g);
    return true;
}

br_app_desc br_main(int argc, char **argv)
{
    return br_game_config_main("game.json", argc, argv, game_init);
}
```

Walk into the crates (the one straight ahead is two units away): the label counts up. Next steps
from here are in Crate Rush: animation state machines from `*.animgraph.json`, tweens, a HUD
through a second (2D) camera, scenes with a loading screen, and play-test scripts.

## Checking this guide

`python tools/check_getting_started.py` builds every step of this file in a scratch project
(`build/getting_started/`), runs it with `--frames 60 --screenshot`, and writes
`build/getting_started/step_<N>.png` for you to open. A step whose CMakeLists has
`braty_install_game` is also installed (`cmake --install`) and the installed copy run from
another directory; a step that writes `playtest/*.json` runs each script headless.

## Where next

- `sample_projects/crate_rush/`: a finished 3D game built this way - three `.glb` levels with
  extras components, a skinned character with a state machine, fixed-step movement and
  collision, a follow camera with shake, world-space name plates and popups, a HUD and menus
  through a UI camera, settings with rebinding, play-test scripts and a soak.
- `sample_projects/star_courier/`: the same pattern at full size - a state machine switching
  scene files, ten components, particles, a filter flash, a tuning file, play-test scripts (`playtest/round.json`, docs/PLAYTESTING.md).
- `docs/DATA_FORMATS.md`: every key of every file. `docs/agents/<module>.md`: one page per
  module with the 5-10 functions you need. Headers in `engine/include/braty/` are the reference.
- `docs/API_REVIEW.md`: known rough edges, so you do not trip over them twice.
