# Lua ships no CMakeLists, so here is a minimal one.
# lua.c and onelua.c each carry a main(), so they stay out of the library.

file(GLOB CYBERMP_LUA_SOURCES "${CMAKE_SOURCE_DIR}/vendor/lua/*.c")
list(FILTER CYBERMP_LUA_SOURCES EXCLUDE REGEX "/(lua|onelua)\\.c$")

if(NOT CYBERMP_LUA_SOURCES)
    message(FATAL_ERROR "vendor/lua is empty -- run the submodule checkout")
endif()

add_library(lua STATIC ${CYBERMP_LUA_SOURCES})

# SYSTEM so Lua's own warnings don't drown ours.
target_include_directories(lua SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/vendor/lua")

# Header-only. Safeties on: script input is untrusted by definition, and without
# them sol2 trades type checks for undefined behaviour.
add_library(sol2 INTERFACE)
target_include_directories(sol2 SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/vendor/sol2/include")
target_compile_definitions(sol2 INTERFACE SOL_ALL_SAFETIES_ON=1)
target_link_libraries(sol2 INTERFACE lua)
