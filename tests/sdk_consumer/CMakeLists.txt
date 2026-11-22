# Builds add-on programs against an installed SDK only (the release workflow's consumer check):
#   cmake -S tests/sdk_consumer -B build/sdk_consumer -DCMAKE_PREFIX_PATH=<SDK folder>
# Each program names one add-on; its dependencies (other add-ons, SDL3_net, Mbed TLS, system
# libraries) must come with it.
cmake_minimum_required(VERSION 3.24)
project(braty_sdk_consumer LANGUAGES C)
find_package(braty CONFIG REQUIRED)
set(addons ${CMAKE_CURRENT_SOURCE_DIR}/../../addons)
add_executable(websocket_echo ${addons}/websocket/examples/websocket_echo.c)
target_link_libraries(websocket_echo PRIVATE braty::braty braty::websocket)
add_executable(http_get ${addons}/http/examples/http_get.c)
target_link_libraries(http_get PRIVATE braty::braty braty::http)
add_executable(addons_registered addons_registered.c)   # runs offline: CI runs it
target_link_libraries(addons_registered PRIVATE braty::braty braty::websocket)
