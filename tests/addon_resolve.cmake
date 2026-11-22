# Resolver check: cmake -DCASE=<ok|cycle|unknown> -DREQUEST=<names or all> -P addon_resolve.cmake
# resolves REQUEST against tests/addon_fixtures/<CASE> and prints "resolved: [a, b, c]"
# (", " because a ';' in a CTest regex property would split it into several regexes).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/BratyAddons.cmake)
braty_addons_resolve(${CMAKE_CURRENT_LIST_DIR}/addon_fixtures/${CASE} "${REQUEST}" order)
list(JOIN order ", " text)
message(STATUS "resolved: [${text}]")
