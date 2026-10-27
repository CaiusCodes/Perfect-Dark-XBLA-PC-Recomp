# perfect_dark - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(perfect_dark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared builds must not carry the builder's folder layout (and with it the
# Windows user name): source paths recorded by __FILE__ in log and assert
# messages are rewritten to short project-relative names. Set before the SDK
# is added so its targets inherit it too.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    cmake_path(CONVERT "${CMAKE_SOURCE_DIR}" TO_CMAKE_PATH_LIST _pd_source_root NORMALIZE)
    add_compile_options("-ffile-prefix-map=${_pd_source_root}=perfect-dark")
    if(REXSDK_DIR)
        cmake_path(CONVERT "${REXSDK_DIR}" TO_CMAKE_PATH_LIST _pd_sdk_root NORMALIZE)
        add_compile_options("-ffile-prefix-map=${_pd_sdk_root}=rexglue-sdk")
    endif()
endif()

# With REXSDK_DIR (the presets point it at the rexglue-sdk submodule, upstream
# v0.9.0), apply this project's SDK changes before the SDK is added; see
# cmake/RexgluePatch.cmake. Without it, an installed, already patched SDK is
# found through find_package.
if(REXSDK_DIR)
    include(cmake/RexgluePatch.cmake)
endif()

include(generated/rexglue.cmake)

# Sources
set(PERFECTDARK_SOURCES
    src/main.cpp
    src/perfect_dark_hooks.cpp
)

# The release version: VERSION is the single source of truth. The game shows
# and logs it (PD_VERSION), the exe's version resource carries it, and
# tools/Make-Release.ps1 and packaging/Build-Launcher.ps1 read the same file.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PD_VERSION)
string(STRIP "${PD_VERSION}" PD_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
string(REGEX MATCHALL "[0-9]+" _pd_version_parts "${PD_VERSION}")
list(APPEND _pd_version_parts 0 0 0 0)
list(SUBLIST _pd_version_parts 0 4 _pd_version_parts)
list(JOIN _pd_version_parts "," PD_VERSION_COMMA)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_include/pd_version_generated.h"
    CONTENT "// Generated from VERSION by CMakeLists.txt.\n#define PD_VERSION \"${PD_VERSION}\"\n#define PD_VERSION_COMMA ${PD_VERSION_COMMA}\n"
    @ONLY)

# The program's icon (window, taskbar, Explorer) and version information.
# Windows takes the lowest numbered icon in the executable for a window that
# sets none itself.
if(WIN32)
    list(APPEND PERFECTDARK_SOURCES src/perfect_dark.rc)
endif()

if(WIN32)
    add_executable(perfect_dark WIN32 ${PERFECTDARK_SOURCES})
else()
    add_executable(perfect_dark ${PERFECTDARK_SOURCES})
endif()

target_include_directories(perfect_dark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_include")

rexglue_setup_target(perfect_dark GPU_PLUGINS xenos)

# The PC menus draw with Dear ImGui, which lives inside rexruntime.dll. Built
# from the submodule, ReXGlue links it privately, so the game gets only its
# headers here: linking the library again would make a second ImGui with its
# own context. (An installed SDK exports the headers itself.)
if(TARGET imgui::imgui)
    target_include_directories(perfect_dark PRIVATE
        $<TARGET_PROPERTY:imgui::imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# The recompiled code gets the same CPU baseline as the runtime DLLs, which
# are already built for x86-64-v3 (AVX2, BMI2, MOVBE): the game's many
# byte-swapped guest loads and stores become single MOVBE instructions.
# fp-contract off keeps floating-point results exactly as the guest's separate
# multiply and add would round them (x86-64-v3 would otherwise allow FMA).
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(perfect_dark PRIVATE -march=x86-64-v3 -ffp-contract=off)
endif()

if(WIN32)
    # XInputGetState for the PC Settings panel's controller navigation.
    target_link_libraries(perfect_dark PRIVATE Xinput9_1_0.lib)
endif()
