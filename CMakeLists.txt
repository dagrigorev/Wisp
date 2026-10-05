cmake_minimum_required(VERSION 3.20)

project(Wisp
    VERSION 1.0.0
    DESCRIPTION "Wisp - GPU-accelerated terminal emulator for Windows"
    LANGUAGES C CXX
)

# ── C standard ─────────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build types ─────────────────────────────────────────────────────────────
set(WISP_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Runtime distribution directory")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Compiler / platform guards ──────────────────────────────────────────────
if(NOT WIN32)
    message(FATAL_ERROR "Wisp targets Windows only (ConPTY, Direct2D, DirectWrite).")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Wisp requires MSVC (cl.exe). Use the VS Developer Command Prompt.")
endif()

# ── Global MSVC flags ────────────────────────────────────────────────────────
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _DISABLE_VECTORIZED_ALGORITHMS
    NOMINMAX
    # Expose version to source code
    WISP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WISP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WISP_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Dynamic MSVC runtime: keeps one CRT heap shared by the EXE and any DLL it
# loads, so allocations can cross module boundaries safely.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_compile_options(
    /W4         # Warning level 4
    /WX-        # Warnings not errors (we audit manually)
    /utf-8      # Source as UTF-8
    /wd4005     # Macro redefinition (WIN32_LEAN_AND_MEAN set by both CMake and headers)
    /wd4100     # Unreferenced formal parameter (common in stubs)
    /wd4996     # Deprecated POSIX names (we use _snprintf deliberately)
    /wd4200     # Nonstandard extension: zero-sized array
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/GL>    # Whole-program optimisation
    $<$<CONFIG:Debug>:/Od>
    $<$<CONFIG:Debug>:/Zi>
)

add_link_options(
    $<$<CONFIG:Release>:/LTCG>
)

# ── Sub-libraries (each a static lib — SOLID: one reason to change) ──────────
add_subdirectory(src/core)
add_subdirectory(src/terminal)
add_subdirectory(src/platform)

# ── Main executable ──────────────────────────────────────────────────────────
add_subdirectory(src)

# ── Runtime resources ───────────────────────────────────────────────────────
add_custom_target(wisp_runtime_resources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WISP_DIST_DIR}/themes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/themes" "${WISP_DIST_DIR}/themes"
    COMMENT "Copying Wisp runtime resources to dist"
)

# ── Tests ────────────────────────────────────────────────────────────────────
option(WISP_BUILD_TESTS "Build unit tests" ON)
if(WISP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install rules ────────────────────────────────────────────────────────────
install(TARGETS Wisp RUNTIME DESTINATION bin)
install(DIRECTORY themes DESTINATION share/Wisp)

# ── CPack (ZIP distribution) ─────────────────────────────────────────────────
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "Wisp")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
include(CPack)

