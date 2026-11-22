# net: TCP client and server, UDP and addresses on SDL3_net (docs/agents/net.md).
# SDL3_net: an installed package first, else built from source (static, no samples). It is a
# private dependency of braty_net; static linking still passes it on, so a game or add-on that
# links braty::net gets SDL3_net without naming it.
find_package(SDL3_net CONFIG QUIET)
if(SDL3_net_FOUND)
    message(STATUS "braty: net: using installed SDL3_net ${SDL3_net_VERSION}")
else()
    message(STATUS "braty: net: SDL3_net not found, fetching release-3.2.0")
    include(FetchContent)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)    # the variables below win over SDL_net's options
    set(BUILD_SHARED_LIBS OFF)               # normal variable: this directory and SDL_net only
    set(SDLNET_SAMPLES OFF)
    set(SDLNET_INSTALL ${BRATY_INSTALL})     # SDL3_net goes into the SDK next to the engine
    FetchContent_Declare(SDL3_net
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git
        GIT_TAG release-3.2.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3_net)
endif()

braty_addon(net SOURCES src/br_net.c LINK SDL3_net::SDL3_net)
