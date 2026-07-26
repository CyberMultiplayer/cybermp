# Shared between the game dll and the server. No RED4ext dependency on purpose,
# so the server side stays buildable without the game sdk.
add_library(cybermp_common STATIC
    Net.cpp
    Net.hpp
)

target_include_directories(cybermp_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(cybermp_common PUBLIC ws2_32)

target_compile_definitions(cybermp_common PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(cybermp_common PRIVATE /W4 /permissive-)
endif()
