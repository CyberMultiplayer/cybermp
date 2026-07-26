add_library(cybermp SHARED
    Main.cpp
    Plugin.cpp
    Plugin.hpp
    Log.hpp
    DebugApi.cpp
    DebugApi.hpp
    WorldSystem.cpp
    WorldSystem.hpp
)

target_link_libraries(cybermp PRIVATE RED4ext::SDK RedLib)

target_compile_definitions(cybermp PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# RED4ext loads <name>.dll, so no "lib" prefix.
set_target_properties(cybermp PROPERTIES
    OUTPUT_NAME "cybermp"
    PREFIX ""
)

if(MSVC)
    target_compile_options(cybermp PRIVATE /W4 /permissive-)

    # Keep symbols in Release too -- a crash inside the game is unreadable without a pdb.
    target_compile_options(cybermp PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(cybermp PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()
