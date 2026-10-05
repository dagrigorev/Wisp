# ── wisp_terminal ──────────────────────────────────────────────────────────────
# Single Responsibility: VT/ANSI emulation, screen buffer, rendering.
# Knows nothing about shell semantics or ConPTY.
add_library(wisp_terminal STATIC
    screen_view.c
    vt_pane.cpp
    layout.c
    renderer.cpp
    font.cpp
    guid_init.cpp
    style_table.cpp
    sys.cpp
)

target_include_directories(wisp_terminal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wisp_terminal PUBLIC wisp_core d2d1 dwrite bcrypt)
