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
    src/pd_native_probe.cpp
    src/pd_native_gpu.cpp
    src/pd_native_shaders.cpp
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
    # Help & Options > Check for Updates asks GitHub over HTTPS (src/pd_update.h).
    target_link_libraries(perfect_dark PRIVATE winhttp.lib)
endif()

# The Plume renderer (gpu_native, src/pd_native_gpu.cpp; the default since
# 0.9.0a): Plume (MIT) for Direct3D 12, and XenosRecomp's shader recompiler
# (MIT) with Microsoft's DXC (dxcompiler.dll + dxil.dll, redistributable) to
# translate the game's shaders on the player's PC. In the public repository
# they are the thirdparty/plume and thirdparty/XenosRecomp submodules (upstream
# commits) plus patches/plume.patch and patches/xenosrecomp.patch, applied at
# configure time (cmake/NativePatches.cmake); in the development tree they are
# the already patched checkouts in ..\native\refs.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume/plume_d3d12.cpp")
    set(_pd_native_refs "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
else()
    set(_pd_native_refs "${CMAKE_CURRENT_SOURCE_DIR}/../native/refs")
endif()
set(PD_PLUME_DIR "${_pd_native_refs}/plume" CACHE PATH "Plume checkout")
set(PD_XENOSRECOMP_DIR "${_pd_native_refs}/XenosRecomp" CACHE PATH "XenosRecomp checkout")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NativePatches.cmake")
    include(cmake/NativePatches.cmake)
endif()
if(WIN32)
    # Direct3D 12 and Vulkan backends (gpu_native_api; Vulkan since 0.9.1).
    add_library(pd_plume STATIC
        "${PD_PLUME_DIR}/plume_d3d12.cpp"
        "${PD_PLUME_DIR}/plume_vulkan.cpp"
        "${PD_PLUME_DIR}/contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
    target_include_directories(pd_plume PUBLIC
        "${PD_PLUME_DIR}"
        "${PD_PLUME_DIR}/contrib/D3D12MemoryAllocator/include")
    # Plume's Vulkan headers (volk, its Vulkan-Headers, VMA) only where Plume's
    # Vulkan types are used: the SDK's Vulkan code (pd_native_vk_out.cpp, which
    # hands frames to the SDK's Vulkan presenter) keeps its own.
    set(_pd_plume_vk_includes
        "${PD_PLUME_DIR}/contrib/volk"
        "${PD_PLUME_DIR}/contrib/Vulkan-Headers/include"
        "${PD_PLUME_DIR}/contrib/VulkanMemoryAllocator/include")
    target_include_directories(pd_plume PRIVATE ${_pd_plume_vk_includes})
    set_property(SOURCE src/pd_native_gpu.cpp APPEND PROPERTY INCLUDE_DIRECTORIES ${_pd_plume_vk_includes})
    # The Vulkan presenter hand-off (src/pd_native_vk_out.cpp) uses the SDK's
    # Vulkan headers: built on its own with the SDK's include paths (as
    # GoldenEye's port does), plus renderdoc_app.h, which those headers include:
    # the installed SDK's include/renderdoc, or the SDK source's thirdparty/renderdoc.
    add_library(pd_vk_out OBJECT src/pd_native_vk_out.cpp)
    target_compile_features(pd_vk_out PRIVATE cxx_std_23)
    target_link_libraries(pd_vk_out PRIVATE rex::runtime)
    if(rexglue_DIR AND EXISTS "${rexglue_DIR}/../../../include/renderdoc/renderdoc_app.h")
        target_include_directories(pd_vk_out SYSTEM PRIVATE "${rexglue_DIR}/../../../include/renderdoc")
    elseif(REXSDK_DIR AND EXISTS "${REXSDK_DIR}/thirdparty/renderdoc/renderdoc_app.h")
        target_include_directories(pd_vk_out SYSTEM PRIVATE "${REXSDK_DIR}/thirdparty/renderdoc")
    endif()
    target_link_libraries(perfect_dark PRIVATE pd_vk_out)
    target_compile_definitions(pd_plume PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pd_plume PRIVATE -w)
    target_link_libraries(pd_plume PUBLIC d3d12 dxgi dxguid d3dcompiler)
    target_link_libraries(perfect_dark PRIVATE pd_plume)
    # The renderer reads guest memory inside __try blocks that must catch a bad
    # page: without asynchronous exceptions clang only catches faults raised in
    # a called function.
    set_property(SOURCE src/pd_native_gpu.cpp APPEND PROPERTY COMPILE_OPTIONS -fasync-exceptions)
    set_property(SOURCE src/pd_native_gpu.cpp src/pd_native_shaders.cpp APPEND PROPERTY COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS)

    set(_pd_xr "${PD_XENOSRECOMP_DIR}/XenosRecomp")
    set(_pd_dxc "${PD_XENOSRECOMP_DIR}/thirdparty/dxc-bin")
    add_library(pd_xenosrecomp STATIC "${_pd_xr}/shader_recompiler.cpp")
    target_include_directories(pd_xenosrecomp PUBLIC
        "${_pd_xr}"
        "${_pd_dxc}/inc"
        "${PD_XENOSRECOMP_DIR}/thirdparty/smol-v/source"
        "${PD_XENOSRECOMP_DIR}/thirdparty/zstd/lib"
        "${PD_XENOSRECOMP_DIR}/thirdparty/xxHash")
    target_precompile_headers(pd_xenosrecomp PRIVATE "${_pd_xr}/pch.h")
    target_compile_definitions(pd_xenosrecomp PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(pd_xenosrecomp PRIVATE -w -fms-extensions)
    target_link_libraries(pd_xenosrecomp PUBLIC fmt::fmt)
    target_link_libraries(perfect_dark PRIVATE pd_xenosrecomp)
    # XenosRecomp's shared definitions, prepended to every translated shader:
    # embedded so the game needs no loose file for it.
    file(READ "${_pd_xr}/shader_common.h" _pd_shader_common)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_include/pd_shader_common.inc"
        CONTENT "// Generated from XenosRecomp/shader_common.h by CMakeLists.txt.\nR\"PDSC(@_pd_shader_common@)PDSC\"\n"
        @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pd_xr}/shader_common.h")
    add_custom_command(TARGET perfect_dark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_pd_dxc}/bin/x64/dxcompiler.dll" "${_pd_dxc}/bin/x64/dxil.dll" "$<TARGET_FILE_DIR:perfect_dark>")
    # The pipelines every level draws (shader hashes and state, no game data),
    # recorded on the developer's PC: created at boot so a first play has no
    # first-compile hitches (pd_native_gpu.cpp, warm_pipelines).
    add_custom_command(TARGET perfect_dark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/native_pipelines.bin" "$<TARGET_FILE_DIR:perfect_dark>")
endif()
