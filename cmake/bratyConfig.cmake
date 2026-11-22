# find_package(braty CONFIG REQUIRED) from the prebuilt SDK, then:
#   target_link_libraries(my_game PRIVATE braty::braty)
#   braty_install_game(my_game ${CMAKE_CURRENT_SOURCE_DIR}/data)   # exe + game.json + assets.brpack
# An SDK built with add-ons also has braty::net, braty::tls, braty::http, braty::websocket:
#   target_link_libraries(my_game PRIVATE braty::braty braty::http)   # brings tls, net, SDL3_net, Mbed TLS
# (BRATY_SDK_ADDONS lists them). SDL3, SDL3_net and Mbed TLS ship in the same SDK folder, so one
# CMAKE_PREFIX_PATH entry finds all of them.
include(CMakeFindDependencyMacro)
get_filename_component(BRATY_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
find_dependency(SDL3 CONFIG HINTS ${BRATY_SDK_DIR})
include(${CMAKE_CURRENT_LIST_DIR}/bratyTargets.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/bratyAddons.cmake)   # BRATY_SDK_ADDONS, BRATY_SDK_ADDON_PACKAGES
foreach(_braty_package IN LISTS BRATY_SDK_ADDON_PACKAGES)
    find_dependency(${_braty_package} CONFIG HINTS ${BRATY_SDK_DIR})
endforeach()
if(BRATY_SDK_ADDONS)
    include(${CMAKE_CURRENT_LIST_DIR}/bratyAddonTargets.cmake)
endif()
set(BRATY_TOOLS_DIR ${BRATY_SDK_DIR}/tools CACHE INTERNAL "tools/ of the Braty engine")
include(${CMAKE_CURRENT_LIST_DIR}/BratyGame.cmake)
