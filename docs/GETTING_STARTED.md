# Getting started: a new game from zero

This walks you through a tiny game, **Coin Rush** (move a ship with the keyboard or a gamepad,
collect coins, hear a sound, watch the score), in eleven steps. Every step builds and runs; every
code block below is the complete file at that step, so you can copy it as it is. For a finished
game built the same way, read `sample_projects/star_courier/`.

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
    sprites/  sounds/              copied from the sample game in step 8
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
        br_node_set_position(e, n, p.x + m->velocity.x * dt, p.y + m->velocity.y * dt);
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
        br_node_set_position(e, n, p.x + m->velocity.x * dt, p.y + m->velocity.y * dt);
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
        br_node_set_position(e, n, p.x + m->velocity.x * dt, p.y + m->velocity.y * dt);
    }
}

static void coin_system(br_engine *e, float dt, void *user)
{
    (void)dt;
    game *g = user;
    if (!g->ready) {
        return;
    }
    br_node ship = br_node_find_by_label(e, br_scene_root(e), "ship", true);
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
            br_text_set_text(e, br_node_find_by_label(e, br_scene_root(e), "score", true), text);
        } else {
            alive++;
        }
    }
    const cJSON *wanted = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(br_game_config_json(e), "game"), "coins_on_screen");
    for (; cJSON_IsNumber(wanted) && alive < wanted->valueint; alive++) {
        br_node n = br_prefab_instantiate(e, "prefabs/coin.prefab.json", br_scene_root(e), NULL);
        br_node_set_position(e, n, 60.0f + random01(g) * 840.0f, 80.0f + random01(g) * 420.0f);
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

## Where next

- `sample_projects/star_courier/`: the same pattern at full size - a state machine switching
  scene files, ten components, particles, a filter flash, a tuning file, play-test scripts (`playtest/round.json`, docs/PLAYTESTING.md).
- `docs/DATA_FORMATS.md`: every key of every file. `docs/agents/<module>.md`: one page per
  module with the 5-10 functions you need. Headers in `engine/include/braty/` are the reference.
- `docs/API_REVIEW.md`: known rough edges, so you do not trip over them twice.
