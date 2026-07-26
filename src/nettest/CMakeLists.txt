add_executable(cybermp_nettest
    main.cpp
)

target_link_libraries(cybermp_nettest PRIVATE cybermp_common)

if(MSVC)
    target_compile_options(cybermp_nettest PRIVATE /W4 /permissive-)
endif()
