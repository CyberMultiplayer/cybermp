add_executable(cybermp_tests
    main.cpp
)

target_link_libraries(cybermp_tests PRIVATE cybermp_common)

if(MSVC)
    target_compile_options(cybermp_tests PRIVATE /W4 /permissive-)
endif()

add_test(NAME protocol COMMAND cybermp_tests)
