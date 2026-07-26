add_executable(cybermp_server
    main.cpp
)

target_link_libraries(cybermp_server PRIVATE cybermp_common)

if(MSVC)
    target_compile_options(cybermp_server PRIVATE /W4 /permissive-)
endif()
