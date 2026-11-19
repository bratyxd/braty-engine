# Star Courier - builds two ways:
#   standalone:  cmake -S sample_projects/star_courier -B build/star_courier -G Ninja \
#                      -DCMAKE_PREFIX_PATH=<SDL3 prefix>   (pulls the engine in with add_subdirectory)
#   from the engine's root project (BRATY_BUILD_SAMPLES, default ON).
# data/ (game.json + assets/) is copied next to the executable after every build, so the game
# finds its files from any working directory (game.json and asset_root resolve against the
# executable's directory).
cmake_minimum_required(VERSION 3.24)
project(star_courier LANGUAGES C)

if(NOT TARGET braty)
    # standalone: build the engine (library only) from the repository root
    set(BRATY_BUILD_TESTS OFF)
    set(BRATY_BUILD_EXAMPLES OFF)
    set(BRATY_BUILD_SAMPLES OFF)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/braty)
endif()

file(GLOB STAR_COURIER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_executable(star_courier ${STAR_COURIER_SOURCES})
set_target_properties(star_courier PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(star_courier PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wpedantic>)
target_link_libraries(star_courier PRIVATE braty)
add_custom_command(TARGET star_courier POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:star_courier>
    COMMENT "star_courier: copying data/ next to the executable")

# The committed play-test (one round: score, lives, state checked with "expect"). Headless, so it
# needs no GPU: ctest -L playtest. playtest/soak.json is the 60-second soak (run it by hand).
add_test(NAME star_courier_round
    COMMAND star_courier --headless --playtest ${CMAKE_CURRENT_SOURCE_DIR}/playtest/round.json)
set_tests_properties(star_courier_round PROPERTIES LABELS playtest)
