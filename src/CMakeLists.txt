# ── Wisp executable ───────────────────────────────────────────────────────────
add_executable(Wisp WIN32
    main.cpp
    window.cpp
    ${CMAKE_SOURCE_DIR}/res/Wisp.rc
)

# Wisp is a GUI app: Windows subsystem, no auto-manifest (RC file embeds one)
if(MSVC)
    set_target_properties(Wisp PROPERTIES
        LINK_FLAGS "/MANIFEST:NO")
endif()

target_include_directories(Wisp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/terminal
    ${CMAKE_SOURCE_DIR}/src/platform
)

target_link_libraries(Wisp PRIVATE
    wisp_core
    wisp_terminal
    wisp_platform
    kernel32 user32 gdi32 shell32
    advapi32 shlwapi ole32 oleaut32
    d2d1 dwrite
)

target_compile_definitions(Wisp PRIVATE
    WISP_VERSION="${CMAKE_PROJECT_VERSION}"
)

set_target_properties(Wisp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${WISP_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${WISP_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${WISP_DIST_DIR}"
)

add_custom_command(TARGET Wisp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/themes
            $<TARGET_FILE_DIR:Wisp>/themes
    COMMENT "Copying built-in themes"
)
