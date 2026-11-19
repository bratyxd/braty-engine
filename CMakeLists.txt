cmake_minimum_required(VERSION 3.24)
project(braty LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BRATY_BUILD_TESTS "Build tests/test_*.c" ON)
option(BRATY_BUILD_EXAMPLES "Build examples/*.c" ON)

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
target_link_libraries(braty PUBLIC SDL3::SDL3 braty_third_party)

# --- tests: every tests/test_*.c is one CTest executable
if(BRATY_BUILD_TESTS)
    enable_testing()
    file(GLOB BR_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c)
    foreach(src ${BR_TEST_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_compile_options(${name} PRIVATE ${BR_WARNINGS})
        target_link_libraries(${name} PRIVATE braty)
        add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    endforeach()
endif()

# --- examples: every examples/*.c is one executable
if(BRATY_BUILD_EXAMPLES)
    file(GLOB BR_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c)
    foreach(src ${BR_EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_compile_options(${name} PRIVATE ${BR_WARNINGS})
        target_link_libraries(${name} PRIVATE braty)
    endforeach()
endif()
