# Crate Rush - builds two ways:
#   standalone:  cmake -S sample_projects/crate_rush -B build/crate_rush -G Ninja \
#                      -DCMAKE_PREFIX_PATH=<SDL3 prefix>   (pulls the engine in with add_subdirectory)
#   from the engine's root project (BRATY_BUILD_SAMPLES, default ON).
# data/ (game.json + assets/) is copied next to the executable after every build, so the game
# finds its files from any working directory. art/ and tools/ are sources for data/ (Kenney packs
# and the scripts that turn them into the atlases and level files); the game never reads them.
cmake_minimum_required(VERSION 3.24)
project(crate_rush LANGUAGES C)

if(NOT TARGET braty)
    # standalone: build the engine (library only) from the repository root
    set(BRATY_BUILD_TESTS OFF)
    set(BRATY_BUILD_EXAMPLES OFF)
    set(BRATY_BUILD_SAMPLES OFF)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/braty)
endif()

file(GLOB CRATE_RUSH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_executable(crate_rush ${CRATE_RUSH_SOURCES})
set_target_properties(crate_rush PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(crate_rush PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wpedantic>)
target_link_libraries(crate_rush PRIVATE braty)
add_custom_command(TARGET crate_rush POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:crate_rush>
    COMMENT "crate_rush: copying data/ next to the executable")

# Shippable folder <folder>/crate_rush/ (exe + game.json + assets.brpack + SDL runtime if shared):
#   cmake --install <build dir> --prefix <folder>
braty_install_game(crate_rush ${CMAKE_CURRENT_SOURCE_DIR}/data)

# The committed play-test: menu -> level 1 -> crates -> results -> menu, checked with "expect".
# Headless, so it needs no GPU: ctest -L playtest. playtest/soak.json cycles every level several
# times and checks that counts return to their baseline (run it by hand; it also profiles).
add_test(NAME crate_rush_round
    COMMAND crate_rush --headless --playtest ${CMAKE_CURRENT_SOURCE_DIR}/playtest/round.json)
set_tests_properties(crate_rush_round PROPERTIES LABELS playtest)
