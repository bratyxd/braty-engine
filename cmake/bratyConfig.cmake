# find_package(braty CONFIG REQUIRED) from the prebuilt SDK, then:
#   target_link_libraries(my_game PRIVATE braty::braty)
#   braty_install_game(my_game ${CMAKE_CURRENT_SOURCE_DIR}/data)   # exe + game.json + assets.brpack
# SDL3 ships in the same SDK folder, so one CMAKE_PREFIX_PATH entry finds both.
include(CMakeFindDependencyMacro)
get_filename_component(BRATY_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
find_dependency(SDL3 CONFIG HINTS ${BRATY_SDK_DIR})
include(${CMAKE_CURRENT_LIST_DIR}/bratyTargets.cmake)
set(BRATY_TOOLS_DIR ${BRATY_SDK_DIR}/tools CACHE INTERNAL "tools/ of the Braty engine")
include(${CMAKE_CURRENT_LIST_DIR}/BratyGame.cmake)
