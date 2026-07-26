# Server logic lives in a lib so the tests can reach it without spawning the exe.
add_library(cybermp_serverlib STATIC
    Session.cpp
    Session.hpp
)

target_include_directories(cybermp_serverlib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cybermp_serverlib PUBLIC cybermp_common)

add_executable(cybermp_server
    main.cpp
)

target_link_libraries(cybermp_server PRIVATE cybermp_serverlib)

if(MSVC)
    target_compile_options(cybermp_serverlib PRIVATE /W4 /permissive-)
    target_compile_options(cybermp_server PRIVATE /W4 /permissive-)
endif()
