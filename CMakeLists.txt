cmake_minimum_required(VERSION 3.24)
project(braty LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BRATY_BUILD_TESTS "Build tests/test_*.c" ON)
option(BRATY_BUILD_EXAMPLES "Build examples/*.c" ON)
option(BRATY_BUILD_SAMPLES "Build sample_projects/* (each has its own CMakeLists.txt)" ON)
# --playtest-stdin reads the process's stdin with C stdio (engine/src/playtest/br_playtest_stdin.c,
# the one exception to "SDL only"); desktop only.
if(ANDROID OR IOS OR EMSCRIPTEN)
    option(BRATY_PLAYTEST_STDIN "Interactive play-testing over stdin/stdout" OFF)
else()
    option(BRATY_PLAYTEST_STDIN "Interactive play-testing over stdin/stdout" ON)
endif()

# --- SDL3: prebuilt package first (pass -DCMAKE_PREFIX_PATH=.deps/sdl3), source fallback second.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    message(STATUS "braty: using installed SDL3 ${SDL3_VERSION}")
else()
    message(STATUS "braty: SDL3 not found, fetching release-3.4.20 (slow, builds SDL from source)")
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.20
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# --- third-party single-file libraries (no warnings flags: not our code)
file(GLOB BR_THIRD_PARTY_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/*/*.c)
add_library(braty_third_party STATIC ${BR_THIRD_PARTY_SOURCES})
target_include_directories(braty_third_party PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(braty_third_party PUBLIC STBI_NO_STDIO STBI_WRITE_NO_STDIO STB_VORBIS_NO_STDIO)
if(NOT MSVC)
    target_link_libraries(braty_third_party PUBLIC m)
endif()

# --- the engine: every .c under engine/src, no list to maintain
set(BR_WARNINGS "$<IF:$<C_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wpedantic>")
file(GLOB_RECURSE BR_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/engine/src/*.c)
add_library(braty STATIC ${BR_ENGINE_SOURCES})
target_include_directories(braty
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine/include ${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/compiled
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine/src)
target_compile_options(braty PRIVATE ${BR_WARNINGS})
option(BRATY_WERROR "Treat warnings in engine code as errors (CI turns it on)" OFF)
if(BRATY_WERROR)
    target_compile_options(braty PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()
target_link_libraries(braty PUBLIC SDL3::SDL3 braty_third_party)
option(BRATY_PROFILER "Profiler scopes and counters (turn OFF for release builds)" ON)
if(NOT BRATY_PROFILER)
    target_compile_definitions(braty PUBLIC BRATY_PROFILER_OFF)
endif()

if(BRATY_PLAYTEST_STDIN)
    target_compile_definitions(braty PRIVATE BRATY_PLAYTEST_STDIN)
endif()
add_library(braty::braty ALIAS braty)   # games: add_subdirectory(<braty> ...) then link braty::braty

# --- tests: every tests/test_*.c is one CTest executable
if(BRATY_BUILD_TESTS)
    enable_testing()
    file(GLOB BR_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c)
    foreach(src ${BR_TEST_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_compile_options(${name} PRIVATE ${BR_WARNINGS})
        target_link_libraries(${name} PRIVATE braty)
        # tests read tests/data, examples/ and docs/ from the source tree, so any build directory works
        target_compile_definitions(${name} PRIVATE BRATY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        if(BRATY_PLAYTEST_STDIN)
            target_compile_definitions(${name} PRIVATE BRATY_PLAYTEST_STDIN)
        endif()
        add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    endforeach()
    # golden images (tools/golden.py): every example + the sample game vs tests/golden/*.png.
    # Opens windows; reported as skipped (exit 77) when no GPU device can be created.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    # The goldens are Direct3D 12 images; other drivers differ by a few pixels (docs/TESTING.md),
    # so Windows names the driver instead of taking whatever SDL picks.
    set(BR_GOLDEN_DRIVER "")
    if(WIN32)
        set(BR_GOLDEN_DRIVER --gpu-driver direct3d12)
    endif()
    if(Python3_FOUND AND BRATY_BUILD_EXAMPLES AND BRATY_BUILD_SAMPLES)
        add_test(NAME golden
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/golden.py run
                    --build-dir ${CMAKE_BINARY_DIR} --out ${CMAKE_BINARY_DIR}/golden ${BR_GOLDEN_DRIVER})
        set_tests_properties(golden PROPERTIES LABELS golden SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()
    if(Python3_FOUND)
        add_test(NAME test_playtest_tool COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_playtest_tool.py)
    endif()
endif()

# --- examples: every examples/*.c is one executable
if(BRATY_BUILD_EXAMPLES)
    file(GLOB BR_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c)
    foreach(src ${BR_EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_compile_options(${name} PRIVATE ${BR_WARNINGS})
        target_link_libraries(${name} PRIVATE braty)
        # examples read their data from the source tree, so any build directory works
        target_compile_definitions(${name} PRIVATE BRATY_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
        if(BRATY_BUILD_TESTS)
            # every example must survive a run without a GPU (br_engine_gpu returns NULL)
            add_test(NAME headless_${name} COMMAND ${name} --headless --frames 5)
            set_tests_properties(headless_${name} PROPERTIES LABELS headless TIMEOUT 60)
        endif()
    endforeach()
endif()

# --- sample games: each builds standalone too (cmake -S sample_projects/<name>)
if(BRATY_BUILD_SAMPLES)
    add_subdirectory(sample_projects/star_courier)
    add_subdirectory(sample_projects/crate_rush)
endif()
