# ── wisp_platform ──────────────────────────────────────────────────────────────
# Single Responsibility: Win32-specific platform services.
# Config loading, PTY management, keyboard translation.
add_library(wisp_platform STATIC
    config.c
    pty.c
    input.c
)

target_include_directories(wisp_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wisp_platform PUBLIC wisp_core)
