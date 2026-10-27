# Perfect Dark builds against upstream ReXGlue v0.9.0 (the rexglue-sdk
# submodule, pinned to the commit below) plus this project's changes in
# patches/rexglue.patch: LAN and Direct Connect play (Xbox LIVE Private Match
# over the local network), the frame pacing, clock and CPU fixes, Vulkan
# fixes, input and menu support, and the bug-report logging. The patch is
# applied to the submodule's working tree at configure time, so nobody edits
# ReXGlue by hand; the submodule's commit stays the exact upstream one.
#
# A copy of the applied patch is kept in the submodule's git directory. When
# patches/rexglue.patch changes, the old copy is reversed and the new one
# applied, so pulling an updated patch needs nothing more than a reconfigure.
#
# To return the submodule to pristine upstream:
#   git -C rexglue-sdk checkout -- .  &&  git -C rexglue-sdk clean -fd

set(PD_REXGLUE_COMMIT 3eb9b511b4140d2769e27be63eae57d41bfa2afa)  # v0.9.0
set(PD_REXGLUE_PATCH "${CMAKE_CURRENT_LIST_DIR}/../patches/rexglue.patch")
cmake_path(NORMAL_PATH PD_REXGLUE_PATCH)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PD_REXGLUE_PATCH}")

function(_pd_rexglue_git out_result out_output)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${REXSDK_DIR}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out_result} ${_result} PARENT_SCOPE)
    set(${out_output} "${_output}" PARENT_SCOPE)
endfunction()

function(pd_apply_rexglue_patch)
    if(NOT REXSDK_DIR OR NOT EXISTS "${REXSDK_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "ReXGlue SDK not found at '${REXSDK_DIR}'. Fetch it with:\n"
            "  git -c core.longpaths=true submodule update --init --recursive")
    endif()
    if(NOT EXISTS "${REXSDK_DIR}/thirdparty/fmt/CMakeLists.txt")
        message(FATAL_ERROR
            "ReXGlue's third-party libraries are missing. Fetch them with:\n"
            "  git -c core.longpaths=true submodule update --init --recursive")
    endif()
    find_package(Git REQUIRED)

    _pd_rexglue_git(_r _head rev-parse HEAD)
    if(NOT _r EQUAL 0)
        message(FATAL_ERROR "'${REXSDK_DIR}' is not a git checkout of ReXGlue: ${_head}")
    endif()
    if(NOT _head STREQUAL PD_REXGLUE_COMMIT)
        message(FATAL_ERROR
            "rexglue-sdk is at ${_head}, but Perfect Dark is built against ReXGlue v0.9.0 "
            "(${PD_REXGLUE_COMMIT}). Run:\n"
            "  git submodule update --init --recursive")
    endif()

    _pd_rexglue_git(_r _git_dir rev-parse --absolute-git-dir)
    set(_applied "${_git_dir}/perfect-dark-applied.patch")
    file(SHA256 "${PD_REXGLUE_PATCH}" _want)

    if(EXISTS "${_applied}")
        file(SHA256 "${_applied}" _have)
        if(_have STREQUAL _want)
            _pd_rexglue_git(_r _out apply --check --reverse "${_applied}")
            if(_r EQUAL 0)
                return()  # applied and unchanged
            endif()
            file(REMOVE "${_applied}")  # the tree was reset since; apply again
        else()
            # An older version of the patch is applied: take it back out first.
            _pd_rexglue_git(_r _out apply --reverse "${_applied}")
            if(NOT _r EQUAL 0)
                # Not removable: either the tree was reset to pristine upstream
                # (the old patch applies cleanly again) or it has local edits.
                _pd_rexglue_git(_r2 _out2 apply --check "${_applied}")
                if(_r2 EQUAL 0)
                    set(_r 0)
                endif()
            endif()
            if(NOT _r EQUAL 0)
                message(FATAL_ERROR
                    "Could not remove the previously applied ReXGlue patch:\n${_out}\n"
                    "Reset the SDK and configure again:\n"
                    "  git -C rexglue-sdk checkout -- .  &&  git -C rexglue-sdk clean -fd")
            endif()
            file(REMOVE "${_applied}")
            message(STATUS "Removed the previous Perfect Dark ReXGlue patch")
        endif()
    endif()

    _pd_rexglue_git(_r _out apply --check --reverse "${PD_REXGLUE_PATCH}")
    if(_r EQUAL 0)
        message(STATUS "Perfect Dark ReXGlue patch already present")
    else()
        _pd_rexglue_git(_r _out apply --whitespace=nowarn "${PD_REXGLUE_PATCH}")
        if(NOT _r EQUAL 0)
            message(FATAL_ERROR
                "patches/rexglue.patch does not apply to rexglue-sdk:\n${_out}\n"
                "The SDK has local edits. Reset it to upstream and configure again:\n"
                "  git -C rexglue-sdk checkout -- .  &&  git -C rexglue-sdk clean -fd")
        endif()
        message(STATUS "Applied the Perfect Dark ReXGlue patch to ${REXSDK_DIR}")
    endif()
    file(COPY_FILE "${PD_REXGLUE_PATCH}" "${_applied}")
endfunction()

# Git for Windows checks symbolic links out as small text files holding the
# link target unless core.symlinks is on (it needs Developer Mode). ReXGlue
# compiles libmspack through such links, which then fail with "expected
# identifier". Replace each placeholder with a copy of the file it points at.
function(pd_materialize_rexglue_symlinks)
    foreach(_sub IN ITEMS thirdparty/libmspack thirdparty/o1heap)
        set(_root "${REXSDK_DIR}/${_sub}")
        if(NOT EXISTS "${_root}")
            continue()
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_root}" ls-files -s
            OUTPUT_VARIABLE _entries
            RESULT_VARIABLE _r
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _r EQUAL 0)
            continue()
        endif()
        string(REPLACE "\n" ";" _entries "${_entries}")
        foreach(_entry IN LISTS _entries)
            if(NOT _entry MATCHES "^120000 [0-9a-f]+ [0-9]+\t(.+)$")
                continue()
            endif()
            set(_link "${_root}/${CMAKE_MATCH_1}")
            if(IS_SYMLINK "${_link}" OR IS_DIRECTORY "${_link}" OR NOT EXISTS "${_link}")
                continue()
            endif()
            file(SIZE "${_link}" _size)
            if(_size GREATER 1024)
                continue()  # already replaced with the real file
            endif()
            file(READ "${_link}" _target)
            string(STRIP "${_target}" _target)
            get_filename_component(_dir "${_link}" DIRECTORY)
            set(_source "${_dir}/${_target}")
            if(EXISTS "${_source}" AND NOT IS_DIRECTORY "${_source}")
                file(COPY_FILE "${_source}" "${_link}")
            endif()
        endforeach()
    endforeach()
endfunction()

pd_apply_rexglue_patch()
pd_materialize_rexglue_symlinks()
