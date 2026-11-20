# Install-time half of braty_install_game (root CMakeLists.txt): packs the game's data/assets
# into assets.brpack and writes game.json next to the executable, adding
# "packs": ["assets.brpack"] when the game's own game.json lists no packs.
# Set by the install(CODE) line before this script: BR_PYTHON, BR_TOOLS, BR_DATA, BR_DEST.
set(dest "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${BR_DEST}")
if(NOT BR_PYTHON)
    message(FATAL_ERROR "braty_install_game: Python 3 is needed to build assets.brpack (tools/pack_assets.py)")
endif()
execute_process(
    COMMAND ${BR_PYTHON} ${BR_TOOLS}/pack_assets.py ${BR_DATA}/assets ${dest}/assets.brpack
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "braty_install_game: pack_assets.py failed (${result})")
endif()
file(READ ${BR_DATA}/game.json json)
if(NOT json MATCHES "\"packs\"")
    # after the opening brace: the rest of the file is kept byte for byte
    string(FIND "${json}" "{" brace)
    if(brace LESS 0)
        message(FATAL_ERROR "braty_install_game: ${BR_DATA}/game.json is not a JSON object")
    endif()
    math(EXPR after "${brace} + 1")
    string(SUBSTRING "${json}" 0 ${after} head)
    string(SUBSTRING "${json}" ${after} -1 tail)
    set(json "${head}\n  \"packs\": [\"assets.brpack\"],${tail}")
endif()
file(WRITE ${dest}/game.json "${json}")
message(STATUS "Installing: ${dest}/game.json and ${dest}/assets.brpack")
