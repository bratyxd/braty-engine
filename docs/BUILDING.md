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
ctest --preset windows-gcc          # test_core, test_math
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
| `android` | Configured (needs `ANDROID_NDK`), untested; tests off. A real Android build also needs SDL's Java glue (`android-project`). |

## GPU drivers

Every app accepts `--gpu-driver <name>` (passed to `SDL_CreateGPUDevice`):

```sh
hello_quad --gpu-driver direct3d12 --frames 3 --screenshot d3d12.png
hello_quad --gpu-driver vulkan     --frames 3 --screenshot vulkan.png
```

Verified on the dev machine (RTX 3060 Ti): `direct3d12` and `vulkan` render identical screenshots.
`metal` is not testable here.

Debug builds create the device with SDL's debug mode on (D3D12 debug layer / Vulkan validation
if installed). Release builds (`-DCMAKE_BUILD_TYPE=Release`) turn it off.

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
- Before each commit: zero warnings in engine code (`-Wall -Wextra -Wpedantic`), `ctest` green.
