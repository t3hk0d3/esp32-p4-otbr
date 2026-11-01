# Sets PROJECT_VER to <UTC build timestamp YYYYMMDDTHHMMSSZ>-<7-hex git hash>[-dirty].
# Include from a project's top-level CMakeLists.txt before project().
# "dirty" means tracked files differ from HEAD; untracked files are ignored.
# Fails the configure step if the result is longer than 30 characters.
# A new commit (or checkout, reset) triggers a reconfigure, hence a fresh stamp: the files that move with HEAD are added to
# CMAKE_CONFIGURE_DEPENDS (HEAD, the loose ref of the current branch, packed-refs, the HEAD reflog; a detached HEAD is
# the HEAD file itself). "-dirty" cannot be tracked this way (it would need the work tree, or the index, which git
# rewrites on every status): it is as of the last configure.
set(_ver_repo_root "${CMAKE_CURRENT_LIST_DIR}/..")

string(TIMESTAMP _ver_stamp "%Y%m%dT%H%M%SZ" UTC)

execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${_ver_repo_root}"
    OUTPUT_VARIABLE _ver_hash
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _ver_hash_rc
    ERROR_QUIET)
if(NOT _ver_hash_rc EQUAL 0 OR _ver_hash STREQUAL "")
    message(FATAL_ERROR "version.cmake: 'git rev-parse --short=7 HEAD' failed")
endif()

# Files whose change means "HEAD moved". Only existing paths can be configure dependencies (CMake drops missing ones).
function(_ver_track_git_head repo_root)
    set(candidates "HEAD" "packed-refs" "logs/HEAD")
    set(dirs "")
    execute_process(
        COMMAND git symbolic-ref -q HEAD
        WORKING_DIRECTORY "${repo_root}"
        OUTPUT_VARIABLE head_ref
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE head_ref_rc
        ERROR_QUIET)
    if(head_ref_rc EQUAL 0 AND NOT head_ref STREQUAL "") # on a branch; a detached HEAD has no ref, HEAD itself changes
        list(APPEND candidates "${head_ref}")
        list(APPEND dirs "${head_ref}")
    endif()
    foreach(name IN LISTS candidates)
        execute_process(
            COMMAND git rev-parse --git-path "${name}" # resolves worktrees and gitfiles
            WORKING_DIRECTORY "${repo_root}"
            OUTPUT_VARIABLE path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE path_rc
            ERROR_QUIET)
        if(path_rc EQUAL 0 AND NOT path STREQUAL "")
            get_filename_component(path "${path}" ABSOLUTE BASE_DIR "${repo_root}")
            if(EXISTS "${path}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
            endif()
            # A packed branch has no ref file until its next commit creates one; CMake drops dependencies that do not exist,
            # but creating the file changes the mtime of its directory, which is tracked as well.
            list(FIND dirs "${name}" is_branch_ref)
            if(NOT is_branch_ref EQUAL -1)
                get_filename_component(ref_dir "${path}" DIRECTORY)
                if(IS_DIRECTORY "${ref_dir}")
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ref_dir}")
                endif()
            endif()
        endif()
    endforeach()
endfunction()
_ver_track_git_head("${_ver_repo_root}")

execute_process(
    COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${_ver_repo_root}"
    OUTPUT_VARIABLE _ver_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _ver_status_rc
    ERROR_QUIET)
if(NOT _ver_status_rc EQUAL 0)
    message(FATAL_ERROR "version.cmake: 'git status' failed")
endif()

set(PROJECT_VER "${_ver_stamp}-${_ver_hash}")
if(NOT _ver_status STREQUAL "")
    string(APPEND PROJECT_VER "-dirty")
endif()

string(LENGTH "${PROJECT_VER}" _ver_len)
if(_ver_len GREATER 30)
    message(FATAL_ERROR "version.cmake: PROJECT_VER '${PROJECT_VER}' is ${_ver_len} chars (max 30)")
endif()
