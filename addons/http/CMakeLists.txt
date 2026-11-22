# http: HTTP/1.1 client over tls (addons/http/README.md).
# picohttpparser (vendored, third_party/picohttpparser/, MIT) parses the status line, the headers
# and chunked bodies. It is compiled into braty_http without this project's warning flags:
# third-party code, its warnings are not ours to fail on.
set(pico ${CMAKE_CURRENT_SOURCE_DIR}/third_party/picohttpparser)
set_source_files_properties(${pico}/picohttpparser.c PROPERTIES
    COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")

braty_addon(http SOURCES src/br_http.c src/br_http_parse.c ${pico}/picohttpparser.c)
target_include_directories(braty_http PRIVATE ${pico})

# test_http runs the loopback TLS server of the tls tests for its https case.
if(TARGET test_http)
    target_link_libraries(test_http PRIVATE braty_tls_test_server)
endif()
