# Server logic lives in a lib so the tests can reach it without spawning the exe.
add_library(cybermp_serverlib STATIC
    Session.cpp
    Session.hpp
    Script.hpp
    ScriptHost.cpp
    ScriptHost.hpp
    LuaBackend.cpp
    LuaBackend.hpp
)

target_include_directories(cybermp_serverlib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# sol2 is PRIVATE: it is an implementation detail of the lua backend and must not
# leak into anything that links the server lib.
target_link_libraries(cybermp_serverlib PUBLIC cybermp_common PRIVATE sol2)

add_executable(cybermp_server
    main.cpp
)

target_link_libraries(cybermp_server PRIVATE cybermp_serverlib)

if(MSVC)
    target_compile_options(cybermp_serverlib PRIVATE /W4 /permissive-)
    target_compile_options(cybermp_server PRIVATE /W4 /permissive-)
endif()
