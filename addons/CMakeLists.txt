# Add-ons named by BRATY_ADDONS (names separated by ';', or all) and their dependencies, each
# added once, dependencies first (cmake/BratyAddons.cmake). Empty (the default) builds none.
if(BRATY_ADDONS STREQUAL "")
    return()
endif()
braty_addons_resolve(${CMAKE_CURRENT_SOURCE_DIR} "${BRATY_ADDONS}" BR_ADDON_ORDER)
message(STATUS "braty: add-ons: ${BR_ADDON_ORDER}")
foreach(name IN LISTS BR_ADDON_ORDER)
    add_subdirectory(${name})
endforeach()
