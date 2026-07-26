cmake_minimum_required(VERSION 3.21)

project(cybermp
    VERSION 0.1.0
    DESCRIPTION "Multiplayer foundation for Cyberpunk 2077"
    LANGUAGES CXX
)

# Fail early with a clear message instead of shipping a dll the game won't load.
if(NOT WIN32)
    message(FATAL_ERROR "cybermp targets Windows only.")
endif()

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "cybermp must be built as x64 -- reconfigure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only place in the project that knows where the game lives.
set(CYBERMP_GAME_DIR "" CACHE PATH "Cyberpunk 2077 install root (e.g. D:/Cyberpunk 2077)")

# Static lib by default, so the sdk compiles once instead of per translation unit.
add_subdirectory(vendor/RED4ext.SDK)

add_subdirectory(src/client)

include(cmake/Deploy.cmake)
