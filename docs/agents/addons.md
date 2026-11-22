# Add-ons

An add-on is an optional library that ships with the engine (in `addons/<name>/` and in the SDK)
but is **not compiled or linked unless a build asks for it**. The core (`braty`, `engine/src/`)
never includes an add-on header and never links an add-on; with `BRATY_ADDONS` empty (the
default) nothing under `addons/` is built. Add-ons may depend on each other, never in a cycle,
and may bring third-party code (the one place where the "SDL3 and the C standard library only"
rule is relaxed).

| Add-on | What it does | Depends on | Third-party | Guide |
|---|---|---|---|---|
| `net` | TCP client / server, UDP, addresses | - | SDL3_net 3.2.0 (zlib, fetched) | `addons/net/README.md` |
| `tls` | byte streams, plain or TLS 1.2 / 1.3 (verified, SNI); random, SHA-1, base64 | `net` | Mbed TLS 3.6.8 (Apache-2.0, fetched), Mozilla CA bundle (MPL-2.0) | `addons/tls/README.md` |
| `http` | HTTP/1.1 client, `http://` and `https://` | `tls` | picohttpparser (MIT, vendored) | `addons/http/README.md` |
| `websocket` | RFC 6455 client, `ws://` and `wss://` | `http` | wslay 1.1.1 (MIT, vendored, patched) | `addons/websocket/README.md` |

Code: `cmake/BratyAddons.cmake` (resolver + `braty_addon()`), `addons/CMakeLists.txt`, the
registry in `engine/include/braty/br_app.h` ("add-on registry"), `engine/src/app/br_engine.c`.
Tests: `tests/test_addon.c`, `tests/addon_resolve.cmake` with `tests/addon_fixtures/`.

## Selecting add-ons: `BRATY_ADDONS`

A cache variable of the engine's `CMakeLists.txt`: names separated by `;`, or `all` (every
folder of `addons/` with an `addon.cmake`). Each name's dependencies are added for you.

```sh
cmake --preset windows-gcc -DBRATY_ADDONS=all            # every add-on (CI does this)
cmake --preset windows-gcc "-DBRATY_ADDONS=http;net"     # http (+ tls, net)
cmake --preset windows-gcc -DBRATY_ADDONS=               # none: the default
```

A game that embeds the engine sets it before `add_subdirectory`:

```cmake
set(BRATY_ADDONS websocket)                    # builds websocket, http, tls, net
add_subdirectory(path/to/braty ${CMAKE_BINARY_DIR}/braty)
target_link_libraries(my_game PRIVATE braty::braty braty::websocket)
```

The prebuilt SDK has every add-on (it is built with `all`): `find_package(braty CONFIG REQUIRED)`
then `target_link_libraries(my_game PRIVATE braty::braty braty::http)`. A game that links only
`braty::braty` gets nothing from add-ons on its link line. `BRATY_SDK_ADDONS` (set by the SDK's
package) lists what the SDK has.

## The resolver

`braty_addons_resolve(<addons_root> "<requested>" <out_var>)` (in `cmake/BratyAddons.cmake`;
creates no targets, so `cmake -P` scripts can use it) returns the requested add-ons plus their
dependencies, each once, dependencies before dependants: requesting `websocket` gives
`net;tls;http;websocket`. `addons/CMakeLists.txt` then calls `add_subdirectory` in that order. It
stops the configure with `FATAL_ERROR` on:

- a cycle: `add-on dependency cycle: a -> b -> a`;
- an unknown name (requested or declared): `unknown add-on 'zz' (available: a, b, c)`.

CTest checks it against fake trees in `tests/addon_fixtures/` (`addon_resolve_ok`, `_all`,
`_cycle`, `_unknown`); these run with add-ons off.

## Folder layout

```
addons/<name>/addon.cmake                 set(BRATY_ADDON_DEPENDS <names>) - nothing else
addons/<name>/CMakeLists.txt              third-party fetch (if any) + one braty_addon(...) call
addons/<name>/README.md                   the guide (installed with the SDK as addons/<name>/README.md)
addons/<name>/include/braty/br_<name>.h   public API
addons/<name>/src/*.c, *_internal.h       sources, private headers
addons/<name>/tests/test_*.c              CTest programs (label `addons`)
addons/<name>/examples/*.c                built, not run by CTest (they may need the internet)
addons/<name>/third_party/<lib>/          vendored third-party code with its licence
```

`addon.cmake` is read by the resolver before any target exists, so it holds only the
dependency list: `set(BRATY_ADDON_DEPENDS tls)` or `set(BRATY_ADDON_DEPENDS)`.

## `braty_addon()`

Called once from `addons/<name>/CMakeLists.txt`:

```cmake
braty_addon(<name> SOURCES <files> [LINK <private libs>] [DEFINITIONS <private defs>]
            [PACKAGES <CMake packages>])
```

- a static library `braty_<name>`, alias `braty::<name>` (the same name in the SDK);
- `include/` public, `src/` private; PUBLIC link to `braty::braty` and `braty::<dep>` for every
  name in `addon.cmake`; `LINK` libraries private (static linking still passes them on, so a
  game never names SDL3_net or Mbed TLS); `DEFINITIONS` private;
- the engine's warning flags (`-Wall -Wextra -Wpedantic` / `/W4`) and `BRATY_WERROR`;
- with `BRATY_BUILD_TESTS`: one CTest program per `tests/test_*.c` (CTest name = file name, label
  `addons`, timeout 120 s, `BRATY_SOURCE_DIR` defined, `tests/br_test.h`, `engine/src` and the
  add-on's `src/` on the include path, working directory `tests/`);
- with `BRATY_BUILD_EXAMPLES`: one executable per `examples/*.c`;
- with `BRATY_INSTALL` (the SDK): the library in `lib/` (export set `bratyAddonTargets`), the
  headers in `include/braty/`, `README.md` as `addons/<name>/README.md`. `PACKAGES` names the CMake
  packages the `LINK` targets come from (`net`: `SDL3_net`, `tls`: `MbedTLS`); the SDK's
  `bratyConfig.cmake` finds them again before it defines `braty::<name>`. Third-party licences:
  the add-on's `CMakeLists.txt` installs them itself to `licenses/<library>/`.

## The core registry and the register convention

The engine keeps one small table of add-on states (`br_app.h`, `BR_ADDON_MAX` 16):

```c
bool        br_addon_add(br_engine *e, const char *name, void *state);  /* false (logged): taken or full */
void       *br_addon_get(br_engine *e, const char *name);               /* NULL: not registered */
uint32_t    br_addon_count(br_engine *e);
const char *br_addon_name_at(br_engine *e, uint32_t index);             /* NULL out of range */
```

`name` is a string literal (compared by pointer first, then by text). Every add-on has
`bool br_<name>_register(br_engine *e)`, which a game calls once in its init:

1. **Idempotent:** `br_addon_get(e, "<name>") != NULL` -> return true.
2. **Registers its dependencies first** (`br_http_register` calls `br_tls_register`, which calls
   `br_net_register`), so a game calls only the one it uses.
3. Allocates its state, `br_addon_add(e, "<name>", state)`, adds its systems
   (`br_system_add(e, "<name>.update", BR_PHASE_INPUT, BR_<NAME>_UPDATE_PRIORITY, ...)`) and frees
   the state in a `br_engine_on_shutdown` callback (shutdown runs in reverse order, so dependants
   go before their dependencies).
4. Every other public function finds its state with `br_addon_get` and is a logged no-op (zero
   handle, false, NULL) when the add-on was not registered.

The four networking add-ons follow the engine's handle-and-poll style: start something, keep a
handle `{index, generation}` (zero = none), poll its state from a system. No callbacks, no
threads of their own; stale and zero handles are safe everywhere; fixed tables sized by
`BR_<NAME>_MAX_*`.

## Rules for add-on code

- Everything in `AGENTS.md` "Code rules" applies (C11, naming, zero warnings, no platform
  `#ifdef`s, errors return + log, a doc comment on every public function, no heap allocation per
  frame, `// ponytail:` on shortcuts).
- **Dependencies go one way.** An add-on includes only its own headers, the engine's and those of
  add-ons it declares in `addon.cmake` (directly or through them). The core never includes an
  add-on header. The resolver refuses a cycle.
- **Third-party code** is allowed in an add-on: fetched by its `CMakeLists.txt` (FetchContent with
  a pinned tag or a tarball with `URL_HASH`, an installed package first where one exists) or
  vendored under `addons/<name>/third_party/<lib>/` with its licence; compiled without the
  engine's warning flags (`COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>"` on its sources);
  listed in `third_party/README.md` (version, licence, how to update); its licence installed to
  `licenses/<library>/`. Add-on sources still use no platform headers.
- **Tests run without the internet:** loopback (127.0.0.1) inside the test process, a distinct
  fixed port per test program (47801 and up: `net` 47801-47804, `tls` 47811-47816, `http`
  47821-47823, `websocket` 47831-47833; a new add-on takes the next free block of ten). Parsers
  and codecs are pure functions with their own tests that open no socket.
- Data from outside is untrusted: every length checked against a limit before use; over the limit
  fails that connection with a logged reason. Limits go in the add-on's README and the README
  "Limits" table.

## Writing a new add-on, step by step

A complete add-on `stopwatch` (counts frames in the INPUT phase) with one test. Copy the shape.

1. `addons/stopwatch/addon.cmake`:

   ```cmake
   # stopwatch: frames since register (addons/stopwatch/README.md).
   set(BRATY_ADDON_DEPENDS)
   ```

2. `addons/stopwatch/CMakeLists.txt`:

   ```cmake
   braty_addon(stopwatch SOURCES src/br_stopwatch.c)
   ```

3. `addons/stopwatch/include/braty/br_stopwatch.h`:

   ```c
   /* br_stopwatch.h - the `stopwatch` add-on: counts frames. */
   #ifndef BR_STOPWATCH_H
   #define BR_STOPWATCH_H

   #include "braty/br_app.h"

   /* Adds the `stopwatch.update` system. Idempotent; false (logged) on failure. */
   bool br_stopwatch_register(br_engine *e);
   /* Frames stepped since register; 0 when not registered. */
   uint64_t br_stopwatch_frames(br_engine *e);

   #endif
   ```

4. `addons/stopwatch/src/br_stopwatch.c`:

   ```c
   #include "braty/br_stopwatch.h"
   #include "braty/br_core.h"

   #include <SDL3/SDL.h>

   typedef struct br_stopwatch {
       uint64_t frames;
   } br_stopwatch;

   static void br_stopwatch_update(br_engine *e, float dt, void *user)
   {
       br_stopwatch *s = user;
       (void)e;
       (void)dt;
       s->frames++;
   }

   static void br_stopwatch_shutdown(br_engine *e, void *user)
   {
       (void)e;
       br_free(user);
   }

   bool br_stopwatch_register(br_engine *e)
   {
       if (e == NULL) {
           br_log_error("br_stopwatch_register: no engine");
           return false;
       }
       if (br_addon_get(e, "stopwatch") != NULL) {
           return true;
       }
       /* a dependant registers its dependencies here first: if (!br_net_register(e)) return false; */
       br_stopwatch *s = br_alloc(sizeof(br_stopwatch));
       if (s == NULL) {
           return false;
       }
       SDL_zerop(s);
       if (!br_addon_add(e, "stopwatch", s)) {
           br_free(s);
           return false;
       }
       br_system_add(e, "stopwatch.update", BR_PHASE_INPUT, 0, br_stopwatch_update, s);
       br_engine_on_shutdown(e, br_stopwatch_shutdown, s);
       return true;
   }

   uint64_t br_stopwatch_frames(br_engine *e)
   {
       br_stopwatch *s = e != NULL ? br_addon_get(e, "stopwatch") : NULL;
       return s != NULL ? s->frames : 0;
   }
   ```

5. `addons/stopwatch/tests/test_stopwatch.c`:

   ```c
   #include "braty/braty.h"
   #include "braty/br_stopwatch.h"
   #include "br_test.h"

   int main(int argc, char **argv)
   {
       (void)argc;
       (void)argv;
       br_engine *e = br_engine_create("test_stopwatch", NULL);
       BR_CHECK(e != NULL);
       BR_CHECK(br_stopwatch_frames(e) == 0);           /* not registered: 0, no crash */
       BR_CHECK(br_stopwatch_register(e));
       BR_CHECK(br_stopwatch_register(e));              /* idempotent */
       for (int i = 0; i < 3; i++) {
           br_engine_step(e, 1.0f / 60.0f);
       }
       BR_CHECK(br_stopwatch_frames(e) == 3);
       br_engine_destroy(e);
       return br_test_finish();
   }
   ```

6. Build and run only it: `cmake --preset windows-gcc -DBRATY_ADDONS=stopwatch`,
   `cmake --build --preset windows-gcc --target test_stopwatch`,
   `ctest --preset windows-gcc -R test_stopwatch`.
7. Write `addons/stopwatch/README.md` in the shape of the other four (what it is and depends on;
   enable it in both setups; register; functions table; a snippet inside a system; rules; limits
   with every `BR_*` constant; gotchas; not implemented), add a row to the table above and to
   `AGENTS.md` "Add-ons", a README feature row and its limits, and third-party rows if any.
8. Push: CI builds every add-on (`-DBRATY_ADDONS=all`) on all four toolchains with warnings as
   errors, and checks that the core still builds without them.
