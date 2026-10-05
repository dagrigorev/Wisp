# ── wisp_core ─────────────────────────────────────────────────────────────────
# Single Responsibility: memory, strings, logging, arena allocation.
# Other modules depend on this; it depends on nothing but the Windows API.
add_library(wisp_core STATIC
    arena.c
    str_util.c
    unicode.c
    log.c
    path_util.c
    session.c
)

target_include_directories(wisp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(wisp_core PRIVATE WISP_CORE_BUILD)
