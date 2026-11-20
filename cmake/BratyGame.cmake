# A shippable folder for a game: braty_install_game(<target> <data dir>) in the game's
# CMakeLists, then `cmake --install <build dir> --prefix <folder>` gives <folder>/<target>/ with
# the executable, the SDL3 runtime (when SDL is a shared library), assets.brpack (<data dir>/assets
# packed by tools/pack_assets.py) and game.json (with "packs": ["assets.brpack"] added if it lists
# none). One folder per game, so the engine's root project installs both samples side by side.
# BRATY_TOOLS_DIR (the engine's tools/ folder) is set by whoever includes this file: the engine's
# CMakeLists.txt or the SDK's bratyConfig.cmake.
function(braty_install_game target data_dir)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    install(TARGETS ${target} RUNTIME DESTINATION ${target})
    install(FILES $<TARGET_RUNTIME_DLLS:${target}> DESTINATION ${target})
    install(CODE "set(BR_PYTHON \"${Python3_EXECUTABLE}\")
set(BR_TOOLS \"${BRATY_TOOLS_DIR}\")
set(BR_DATA \"${data_dir}\")
set(BR_DEST \"${target}\")")
    install(SCRIPT ${BRATY_TOOLS_DIR}/install_game.cmake)
endfunction()
