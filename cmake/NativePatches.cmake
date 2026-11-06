# The Plume renderer builds two libraries from upstream commits (the
# thirdparty/plume and thirdparty/XenosRecomp submodules) plus this project's
# changes in patches/plume.patch and patches/xenosrecomp.patch: Direct3D 12
# depth formats in Plume, and Perfect Dark's shader translation fixes in
# XenosRecomp. As with cmake/RexgluePatch.cmake, each patch is applied to its
# submodule's working tree at configure time, so the submodule commits stay
# the exact upstream ones and nobody edits them by hand.
#
# A copy of each applied patch is kept in the submodule's git directory. When
# a patch changes, the old copy is reversed and the new one applied, so
# pulling an updated patch needs nothing more than a reconfigure.
#
# To return a submodule to pristine upstream:
#   git -C thirdparty/plume checkout -- .  &&  git -C thirdparty/plume clean -fd

find_package(Git REQUIRED)

function(_pd_native_git dir out_result out_output)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out_result} ${_result} PARENT_SCOPE)
    set(${out_output} "${_output}" PARENT_SCOPE)
endfunction()

function(pd_apply_native_patch name dir commit patch)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    _pd_native_git("${dir}" _r _head rev-parse HEAD)
    if(NOT _r EQUAL 0)
        message(FATAL_ERROR
            "${name} not found at '${dir}'. Fetch it with:\n"
            "  git -c core.longpaths=true submodule update --init --recursive")
    endif()
    if(NOT _head STREQUAL commit)
        message(FATAL_ERROR
            "${dir} is at ${_head}, but Perfect Dark is built against ${name} ${commit}. Run:\n"
            "  git submodule update --init --recursive")
    endif()

    _pd_native_git("${dir}" _r _git_dir rev-parse --absolute-git-dir)
    set(_applied "${_git_dir}/perfect-dark-applied.patch")
    file(SHA256 "${patch}" _want)

    if(EXISTS "${_applied}")
        file(SHA256 "${_applied}" _have)
        if(_have STREQUAL _want)
            _pd_native_git("${dir}" _r _out apply --check --reverse "${_applied}")
            if(_r EQUAL 0)
                return()  # applied and unchanged
            endif()
            file(REMOVE "${_applied}")  # the tree was reset since; apply again
        else()
            # An older version of the patch is applied: take it back out first.
            _pd_native_git("${dir}" _r _out apply --reverse "${_applied}")
            if(NOT _r EQUAL 0)
                _pd_native_git("${dir}" _r2 _out2 apply --check "${_applied}")
                if(_r2 EQUAL 0)
                    set(_r 0)  # the tree was reset to upstream already
                endif()
            endif()
            if(NOT _r EQUAL 0)
                message(FATAL_ERROR
                    "Could not remove the previously applied ${name} patch:\n${_out}\n"
                    "Reset the submodule and configure again:\n"
                    "  git -C ${dir} checkout -- .  &&  git -C ${dir} clean -fd")
            endif()
            file(REMOVE "${_applied}")
            message(STATUS "Removed the previous Perfect Dark ${name} patch")
        endif()
    endif()

    _pd_native_git("${dir}" _r _out apply --check --reverse "${patch}")
    if(_r EQUAL 0)
        message(STATUS "Perfect Dark ${name} patch already present")
    else()
        _pd_native_git("${dir}" _r _out apply --whitespace=nowarn "${patch}")
        if(NOT _r EQUAL 0)
            message(FATAL_ERROR
                "The ${name} patch does not apply to ${dir}:\n${_out}\n"
                "The submodule has local edits. Reset it to upstream and configure again:\n"
                "  git -C ${dir} checkout -- .  &&  git -C ${dir} clean -fd")
        endif()
        message(STATUS "Applied the Perfect Dark ${name} patch to ${dir}")
    endif()
    file(COPY_FILE "${patch}" "${_applied}")
endfunction()

set(_pd_root "${CMAKE_CURRENT_LIST_DIR}/..")
cmake_path(NORMAL_PATH _pd_root)
pd_apply_native_patch(Plume "${_pd_root}thirdparty/plume"
    e0c8871b930dc1a6544bf457f2348c456b9d7e13 "${_pd_root}patches/plume.patch")
pd_apply_native_patch(XenosRecomp "${_pd_root}thirdparty/XenosRecomp"
    990d03b28a27b50277ee5d8d942e1c5f873869d1 "${_pd_root}patches/xenosrecomp.patch")
if(NOT EXISTS "${_pd_root}thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxcompiler.dll" OR
   NOT EXISTS "${_pd_root}thirdparty/plume/contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
    message(FATAL_ERROR
        "The Plume renderer's third-party libraries are missing. Fetch them with:\n"
        "  git -c core.longpaths=true submodule update --init --recursive")
endif()
