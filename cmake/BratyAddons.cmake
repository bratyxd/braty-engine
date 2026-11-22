# Add-ons: optional libraries in addons/<name>/, built only when BRATY_ADDONS names them (or
# their dependants). Each add-on folder has addon.cmake (set(BRATY_ADDON_DEPENDS <names>), nothing
# else) and a CMakeLists.txt that calls braty_addon(). docs/agents/addons.md.
#
# The resolver creates no targets, so `cmake -P` scripts can include this file
# (tests/addon_resolve.cmake).

get_filename_component(BRATY_ENGINE_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)  # the engine sources

# Depth-first visit of one add-on: dependencies are appended to _braty_order before the add-on.
# _braty_order (the result so far) and _braty_available come from the caller's scope; stack is
# the chain of add-ons being visited, for the cycle message.
function(_braty_addon_visit root name stack)
    if(name IN_LIST _braty_order)
        return()
    endif()
    if(name IN_LIST stack)
        list(FIND stack ${name} first)
        list(SUBLIST stack ${first} -1 cycle)
        list(APPEND cycle ${name})
        list(JOIN cycle " -> " text)
        message(FATAL_ERROR "add-on dependency cycle: ${text}")
    endif()
    if(NOT name IN_LIST _braty_available)
        list(JOIN _braty_available ", " names)
        if(names STREQUAL "")
            set(names "none")
        endif()
        message(FATAL_ERROR "unknown add-on '${name}' (available: ${names})")
    endif()
    set(BRATY_ADDON_DEPENDS "")
    include(${root}/${name}/addon.cmake)
    set(deps ${BRATY_ADDON_DEPENDS})
    list(APPEND stack ${name})
    foreach(dep IN LISTS deps)
        _braty_addon_visit(${root} ${dep} "${stack}")
    endforeach()
    list(APPEND _braty_order ${name})
    set(_braty_order ${_braty_order} PARENT_SCOPE)
endfunction()

# braty_addons_resolve(<addons_root> "<requested_list>" <out_var>): the requested add-ons (names,
# or "all" = every folder with an addon.cmake) plus their dependencies, each once, dependencies
# before dependants. Quote the list. Stops with FATAL_ERROR on a cycle or an unknown name.
function(braty_addons_resolve root requested out_var)
    file(GLOB found RELATIVE ${root} ${root}/*/addon.cmake)
    set(_braty_available "")
    foreach(file IN LISTS found)
        get_filename_component(dir ${file} DIRECTORY)
        list(APPEND _braty_available ${dir})
    endforeach()
    list(SORT _braty_available)
    if(requested STREQUAL "all")
        set(requested ${_braty_available})
    endif()
    set(_braty_order "")
    foreach(name IN LISTS requested)
        _braty_addon_visit(${root} ${name} "")
    endforeach()
    set(${out_var} ${_braty_order} PARENT_SCOPE)
endfunction()

# braty_addon(<name> SOURCES <files> [LINK <private libs>] [DEFINITIONS <private defs>]), called
# from addons/<name>/CMakeLists.txt: static library braty_<name> (alias braty::<name>) on
# braty::braty and the add-ons its addon.cmake names, include/ public, src/ private; tests from
# tests/test_*.c (label `addons`), examples from examples/*.c (built, not run).
function(braty_addon name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LINK;DEFINITIONS")
    set(dir ${CMAKE_CURRENT_SOURCE_DIR})
    set(BRATY_ADDON_DEPENDS "")
    include(${dir}/addon.cmake)
    set(deps "")
    foreach(dep IN LISTS BRATY_ADDON_DEPENDS)
        list(APPEND deps braty::${dep})
    endforeach()
    set(warnings "$<IF:$<C_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra;-Wpedantic>")
    set(werror "$<$<BOOL:${BRATY_WERROR}>:$<IF:$<C_COMPILER_ID:MSVC>,/WX,-Werror>>")

    add_library(braty_${name} STATIC ${ARG_SOURCES})
    add_library(braty::${name} ALIAS braty_${name})
    target_include_directories(braty_${name}
        PUBLIC $<BUILD_INTERFACE:${dir}/include> $<INSTALL_INTERFACE:include>
        PRIVATE ${dir}/src)
    target_link_libraries(braty_${name} PUBLIC braty::braty ${deps} PRIVATE ${ARG_LINK})
    target_compile_definitions(braty_${name} PRIVATE ${ARG_DEFINITIONS})
    target_compile_options(braty_${name} PRIVATE ${warnings} ${werror})
    # Install rules (BRATY_INSTALL) come with the SDK work: plan 2026-11-22-addons Task 6.

    if(BRATY_BUILD_TESTS)
        file(GLOB tests CONFIGURE_DEPENDS ${dir}/tests/test_*.c)
        foreach(src IN LISTS tests)
            get_filename_component(test ${src} NAME_WE)
            add_executable(${test} ${src})
            target_compile_options(${test} PRIVATE ${warnings} ${werror})
            target_link_libraries(${test} PRIVATE braty_${name})
            # tests/br_test.h, engine-internal and add-on-internal headers
            target_include_directories(${test} PRIVATE
                ${BRATY_ENGINE_DIR}/tests ${BRATY_ENGINE_DIR}/engine/src ${dir}/src)
            target_compile_definitions(${test} PRIVATE BRATY_SOURCE_DIR="${BRATY_ENGINE_DIR}")
            add_test(NAME ${test} COMMAND ${test} WORKING_DIRECTORY ${dir}/tests)
            set_tests_properties(${test} PROPERTIES LABELS addons TIMEOUT 120)
        endforeach()
    endif()
    if(BRATY_BUILD_EXAMPLES)
        file(GLOB examples CONFIGURE_DEPENDS ${dir}/examples/*.c)
        foreach(src IN LISTS examples)
            get_filename_component(example ${src} NAME_WE)
            add_executable(${example} ${src})
            target_compile_options(${example} PRIVATE ${warnings} ${werror})
            target_link_libraries(${example} PRIVATE braty_${name})
        endforeach()
    endif()
endfunction()
