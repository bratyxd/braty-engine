# Building Braty

## Requirements

- CMake >= 3.24 and Ninja
- A C11 compiler: GCC or Clang (MSVC preset exists but is untested)
- SDL 3.4.20, either prebuilt (fast) or fetched and built by CMake (slow, automatic)
- Only to change shaders: Python 3 and the Vulkan SDK (`dxc`, `spirv-cross`)

## Quick start (Windows, MinGW-w64 GCC) - tested

```sh
cmake --preset windows-gcc          # configure into build/windows-gcc
cmake --build --preset windows-gcc  # engine, tests, examples
ctest --preset windows-gcc          # unit tests, headless examples, play-tests, golden images
build/windows-gcc/hello_quad.exe --frames 3 --screenshot out.png
```

Without presets:

```sh
cmake -S . -B build/windows-gcc -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_PREFIX_PATH=D:/personal/engines/b1/.deps/sdl3
cmake --build build/windows-gcc
ctest --test-dir build/windows-gcc --output-on-failure
```

## SDL3

`CMakeLists.txt` calls `find_package(SDL3 CONFIG)` first. If that fails it uses `FetchContent` to
clone tag `release-3.4.20` and builds SDL as part of the project (a few minutes).

The presets point `CMAKE_PREFIX_PATH` at `.deps/sdl3` (gitignored). To create that prebuilt copy:

```sh
git clone --depth 1 --branch release-3.4.20 https://github.com/libsdl-org/SDL.git .deps/src/SDL
cmake -S .deps/src/SDL -B .deps/src/build-sdl -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
      -DCMAKE_INSTALL_PREFIX=.deps/sdl3
cmake --build .deps/src/build-sdl && cmake --install .deps/src/build-sdl
```

Gotcha (CMake 4.x): do not name SDL's build directory `<sdl-source-dir>-build` (for example
`SDL` and `SDL-build`). SDL's `source_group` call treats a string prefix as a path prefix and
configuration fails. `build-sdl` works.

## Presets

| Preset | Status |
|---|---|
| `windows-gcc` | Tested: Windows 11, GCC 15.2 (MinGW-w64), Ninja, Direct3D 12 and Vulkan |
| `windows-msvc` | Configured, untested (no MSVC on the dev machine). Run from a VS developer prompt. |
| `linux` | Configured, untested |
| `macos` | Configured, untested |
| `ios` | Configured (Xcode generator), untested; tests off |
| `android` | Configured (needs `ANDROID_NDK`), untested; tests off. A real Android build also needs SDL's Java glue (`android-project`) and the game as a shared library (`docs/PLATFORMS.md`). |
| `<preset>-release` | One per preset above (`windows-gcc-release` tested): see "Release builds" |

## Release builds

```sh
cmake --preset windows-gcc-release            # build/windows-gcc-release
cmake --build --preset windows-gcc-release
ctest --preset windows-gcc-release            # all tests pass here too
```

A release preset is its platform preset plus:

| Setting | Debug presets | Release presets | Effect |
|---|---|---|---|
| `CMAKE_BUILD_TYPE` | Debug | Release (`-O3`, `NDEBUG`) | `NDEBUG` turns off GPU validation (SDL GPU debug mode) and scene / data hot-reload polling |
| `BRATY_PROFILER` | ON | OFF | profiler scopes and counters compile to nothing; `--profiler-hud`, F2 and `--profile` show nothing |
| `BRATY_PLAYTEST_STDIN` | ON (desktop) | OFF | no `--playtest-stdin` (scripts with `--playtest` still work) |

What a release build does on its own: packs win over loose files (`br_file_set_loose_first`), the
log is mirrored into `log.txt` in user storage for interactive runs (written at shutdown and on a
failed start), and a failed start-up shows an SDL message box with the last error (never in
`--headless`, `--frames`, `--deterministic` or play-test runs). What it does **not** do yet:
executables are console-subsystem programs on Windows (a console window opens next to the game;
add `WIN32_EXECUTABLE` to your game target), and the debug overlay (F1 / F2) is still compiled in
(game.json `"debug": {"keys": false}` or `br_debug_set_keys_enabled` switches its keys off). For
profiling numbers use a Release build with `-DBRATY_PROFILER=ON`.

## Shipping a game folder

`braty_install_game(<target> <data dir>)` in the game's CMakeLists (both samples call it) adds an
install rule; `cmake --install` then writes one folder per game:

```sh
cmake --install build/windows-gcc-release --prefix dist
# dist/crate_rush/: crate_rush.exe, game.json, assets.brpack (+ SDL3.dll when SDL is shared)
dist/crate_rush/crate_rush.exe --headless --playtest sample_projects/crate_rush/playtest/round.json
```

The pack is `<data dir>/assets` packed by `tools/pack_assets.py` (Python 3 at install time; no
compression, paths case-sensitive and at most 255 bytes); game.json is copied with
`"packs": ["assets.brpack"]` added when it lists none. During development a listed pack that is
missing is skipped with an info line, so the build folder's loose files keep working.
`--pack FILE` mounts a pack by hand (before game.json's). Format and rules:
`docs/DATA_FORMATS.md` section 8.

## GPU drivers

Every app accepts `--gpu-driver <name>` (passed to `SDL_CreateGPUDevice`):

```sh
hello_quad --gpu-driver direct3d12 --frames 3 --screenshot d3d12.png
hello_quad --gpu-driver vulkan     --frames 3 --screenshot vulkan.png
```

Verified on the dev machine (RTX 3060 Ti): both drivers run every example and both games, but
the screenshots are not identical: Vulkan differs from the Direct3D 12 goldens by a few edge
pixels in 11 of 29 golden cases (`docs/TESTING.md`, "Driver"). `metal` is not testable here.

Debug builds create the device with SDL's debug mode on (D3D12 debug layer / Vulkan validation
if installed). Release builds (`NDEBUG`, the release presets) turn it off.

## Shaders

Shaders are HLSL in `engine/shaders/<name>.<vert|frag>.hlsl`. They are compiled offline and the
results are committed as `engine/shaders/compiled/<name>.<stage>.h`, so a normal build needs no
shader tools. After editing or adding a shader:

```sh
python tools/compile_shaders.py            # all shaders
python tools/compile_shaders.py engine/shaders/textured.frag.hlsl
```

The tool finds `dxc` and `spirv-cross` on `PATH`, under `$VULKAN_SDK/Bin`, or in the newest
`C:/VulkanSDK/*/Bin`. Each header holds SPIR-V (Vulkan), DXIL (Direct3D 12) and MSL (Metal) plus
the resource counts SDL needs. Tested with the Vulkan SDK 1.4.363.0 (`dxc` 1.9.0.5512): its DXIL is
accepted by Direct3D 12 without a separate `dxil.dll`.

**MSL is untested.** It is produced by `spirv-cross --msl --msl-version 20100
--msl-decoration-binding` (entry point `main0`), which follows SDL's documented Metal binding
order for simple shaders, but nothing has run it on a Mac yet.

Resource rules (from `SDL_CreateGPUShader` in `SDL3/SDL_gpu.h`):

| Stage | Textures + samplers | Uniform buffers |
|---|---|---|
| vertex | `register(t[n], space0)` / `register(s[n], space0)` | `register(b[n], space1)` |
| fragment | `register(t[n], space2)` / `register(s[n], space2)` | `register(b[n], space3)` |

Mark each texture/sampler pair `[[vk::combinedImageSampler]]` (SPIR-V wants combined samplers).
Vertex inputs are `TEXCOORD0..N`. The fragment input struct must be identical to the vertex output
struct, `SV_Position` included, or Direct3D 12 rejects the pipeline.

## Verification conventions

- `--frames N` runs N frames with a fixed 1/60 s step (reproducible), then exits 0.
- `--screenshot out.png` writes the last frame as PNG (window size in pixels).
- `--deterministic` makes any run reproducible (constant steps, seeded RNG, in-order asset
  loads); `--playtest` scripts and `tools/golden.py` use it. `docs/TESTING.md`, `docs/PLAYTESTING.md`.
- Before each commit: zero warnings in engine code (`-Wall -Wextra -Wpedantic`), `ctest` green
  (it includes the golden images: they need a GPU and open windows; `ctest -LE golden` skips them).

## CMake options

| Option | Default | What |
|---|---|---|
| `BRATY_BUILD_TESTS` | ON | `tests/test_*.c`, a `--headless --frames 5` run of every example, the play-test scripts and the golden-image test (needs Python 3) |
| `BRATY_BUILD_EXAMPLES` | ON | `examples/*.c` (they read their data from the source tree: `BRATY_EXAMPLES_DIR`) |
| `BRATY_BUILD_SAMPLES` | ON | `sample_projects/*` |
| `BRATY_PLAYTEST_STDIN` | ON on desktop, OFF on Android / iOS / Emscripten and in release presets | `--playtest-stdin`: the one engine file that uses C stdio (`engine/src/playtest/br_playtest_stdin.c`) |
| `BRATY_PROFILER` | ON (OFF in release presets) | profiler scopes and counters (`docs/PROFILING.md`) |
| `BRATY_WERROR` | OFF (ON in CI) | warnings in engine code are errors (`-Werror` / `/WX`) |
| `BRATY_ADDONS` | empty | add-ons to build: names separated by `;`, or `all` ("Add-ons" below) |
| `BRATY_INSTALL` | OFF | install rules for the SDK ("The SDK" below) |

Tests read their data from the source tree (`BRATY_SOURCE_DIR`, set by CMake), so a build
directory can be anywhere: `cmake --preset windows-gcc -B build/any/depth` works with `ctest`.

## Add-ons

Optional libraries in `addons/` (`net`, `tls`, `http`, `websocket`; `docs/agents/addons.md`).
Nothing there is configured, fetched or compiled unless `BRATY_ADDONS` names it:

```sh
cmake --preset windows-gcc -DBRATY_ADDONS=all            # every add-on (what CI does)
cmake --preset windows-gcc -DBRATY_ADDONS=websocket      # websocket + http, tls, net (added for you)
cmake --preset windows-gcc -DBRATY_ADDONS=               # none (the default)
```

A game that embeds the engine writes `set(BRATY_ADDONS http)` before
`add_subdirectory(<braty>)` and links `braty::http`. An unknown name or a dependency cycle stops
the configure with an error naming it.

What is fetched, and from where (everything else is in the repository: picohttpparser and wslay
under `addons/<name>/third_party/`, the CA bundle `addons/tls/certs/cacert.pem`):

| Add-on | Fetched | From |
|---|---|---|
| `net` | SDL3_net `release-3.2.0` (only when no installed `SDL3_net` package is found) | `git clone --depth 1` of https://github.com/libsdl-org/SDL_net.git |
| `tls` | Mbed TLS 3.6.8 (always) | https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.8/mbedtls-3.6.8.tar.bz2, checked against the SHA-256 in `addons/tls/CMakeLists.txt` |

**Build time:** all four add a little over 100 s of CPU time to a build (Mbed TLS about 90 s,
SDL3_net about 5 s, the add-ons themselves about 10 s; for comparison SDL about 300 s and the
engine about 230 s; measured with MinGW GCC, Release).

**Offline builds:** FetchContent downloads once into `<build>/_deps/`; later configures of the
same build directory reuse it. For a new build directory without network, point CMake at sources
you already have: `-DFETCHCONTENT_SOURCE_DIR_SDL3_NET=<SDL_net checkout>`,
`-DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<extracted mbedtls-3.6.8>` (and
`-DFETCHCONTENT_SOURCE_DIR_SDL3=<SDL checkout>` when SDL is fetched too), or an installed SDL3_net
package on `CMAKE_PREFIX_PATH`. `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` makes a configure fail
instead of trying the network.

## The SDK (a game without the engine's sources)

Every release on https://github.com/bratyxd/braty-engine/releases has one archive per toolchain
(`windows-msvc`, `windows-mingw`, `linux-x64`, `macos-arm64`): the engine and SDL3 as static
libraries, the headers, a CMake package, `tools/` and `docs/`. A game links it and never compiles
the engine or SDL:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_game LANGUAGES C)
find_package(braty CONFIG REQUIRED)            # cmake -DCMAKE_PREFIX_PATH=<unzipped SDK folder> ...
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.c)
add_executable(my_game ${SOURCES})
target_link_libraries(my_game PRIVATE braty::braty)
add_custom_command(TARGET my_game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:my_game>)
braty_install_game(my_game ${CMAKE_CURRENT_SOURCE_DIR}/data)   # cmake --install: exe + game.json + assets.brpack
```

The tools are in `<SDK>/tools` (`playtest.py`, `pack_assets.py`, `golden.py`). Both sample games
use the SDK when `CMAKE_PREFIX_PATH` points at one and build the engine from source otherwise.

**Add-ons in the SDK.** The release archives are built with `-DBRATY_ADDONS=all`, so the package
also defines `braty::net`, `braty::tls`, `braty::http` and `braty::websocket`. Link the one you
use; its dependencies (the add-ons under it, SDL3_net, Mbed TLS, and system libraries such as
`ws2_32`, `iphlpapi` and `bcrypt` on Windows) come with it:

```cmake
target_link_libraries(my_game PRIVATE braty::braty braty::websocket)   # + http, tls, net, SDL3_net, Mbed TLS
```

and call `br_websocket_register(e)` in init. A game that links only `braty::braty` has no add-on
library on its link line (the release workflow checks Crate Rush's). In the SDK: the add-on
libraries (`lib/libbraty_<name>.a` or `braty_<name>.lib`), SDL3_net and Mbed TLS (`mbedtls`,
`mbedx509`, `mbedcrypto`, `everest`, `p256m`) with their CMake packages in `lib/`, the headers in
`include/braty/`, each add-on's guide in `addons/<name>/README.md`, and the licences of SDL3_net,
Mbed TLS, picohttpparser, wslay and the CA bundle in `licenses/<library>/`. `BRATY_SDK_ADDONS`
lists the add-ons of the SDK you found. `tests/sdk_consumer/` is a small project that builds the
`http_get` and `websocket_echo` examples against an installed SDK only.

Building an SDK yourself: `-DBRATY_INSTALL=ON -DBRATY_ADDONS=all` (as `release.yml`), then
`cmake --install <build> --prefix <folder>`. SDL3 and SDL3_net go into it only when they were
fetched and built in that build: a configure that found them as installed packages (the presets'
`.deps/sdl3`, a system SDL3_net) leaves them out, and a game then needs those prefixes on
`CMAKE_PREFIX_PATH` too. The release workflow fetches both, so its archives are complete.

- One flavour: optimised, profiler and `--playtest-stdin` compiled in. For a stripped shipping
  build, or a toolchain with no archive (consoles, Intel Macs), build from source
  (`add_subdirectory(<braty>)` and the `<preset>-release` presets).
- MSVC: built with the release runtime; build the game as Release or RelWithDebInfo.
- Linux: built on Ubuntu 22.04 (its glibc or newer).
- Making a release: `git tag v0.2.0 && git push origin v0.2.0` (bump `project(braty VERSION ...)`
  first). `.github/workflows/release.yml` builds each archive, builds Crate Rush against it,
  plays its round, checks its link line has no add-on, builds `tests/sdk_consumer` against it,
  and publishes. `gh workflow run release.yml` does the same without publishing.
