# Optional `deploy` target -- the project must still build on a machine without the game.

if(NOT CYBERMP_GAME_DIR)
    message(STATUS "cybermp: CYBERMP_GAME_DIR not set -> 'deploy' target unavailable.")
    return()
endif()

if(NOT EXISTS "${CYBERMP_GAME_DIR}/bin/x64/Cyberpunk2077.exe")
    message(WARNING "cybermp: '${CYBERMP_GAME_DIR}' doesn't look like a Cyberpunk 2077 install.")
endif()

# Dependent dlls go here too, never in bin/x64.
set(CYBERMP_PLUGIN_DIR "${CYBERMP_GAME_DIR}/red4ext/plugins/cybermp")

add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CYBERMP_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:cybermp>" "${CYBERMP_PLUGIN_DIR}/"
    DEPENDS cybermp
    COMMENT "Deploying -> ${CYBERMP_PLUGIN_DIR}"
    VERBATIM
)

message(STATUS "cybermp: 'deploy' target -> ${CYBERMP_PLUGIN_DIR}")
