# Configure local Git hooks for development workflows

include_guard(GLOBAL)

function(atk_setup_git_hooks)
    find_program(GIT_EXECUTABLE NAMES git)
    if(NOT GIT_EXECUTABLE)
        message(STATUS "Git hook setup skipped: git not found")
        return()
    endif()

    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" rev-parse --show-toplevel
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _git_root_result
        OUTPUT_VARIABLE _git_root_output
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(NOT _git_root_result EQUAL 0)
        message(STATUS "Git hook setup skipped: repository root not found")
        return()
    endif()

    set(_repo_root "${_git_root_output}")
    set(_hooks_dir "${_repo_root}/.git/hooks")
    set(_source_hook "${CMAKE_SOURCE_DIR}/build-aux/pre-commit")
    set(_dest_hook "${_hooks_dir}/pre-commit")

    if(NOT EXISTS "${_source_hook}")
        message(WARNING "Git hook setup skipped: source hook not found at ${_source_hook}")
        return()
    endif()

    if(NOT EXISTS "${_hooks_dir}")
        message(WARNING "Git hook setup skipped: hooks directory not found at ${_hooks_dir}")
        return()
    endif()

    if(NOT EXISTS "${_dest_hook}")
        execute_process(
            COMMAND
                "${CMAKE_COMMAND}" -E copy "${_source_hook}" "${_dest_hook}"
        )
        file(
            CHMOD
                "${_dest_hook}"
            FILE_PERMISSIONS
                OWNER_READ
                OWNER_WRITE
                OWNER_EXECUTE
                GROUP_READ
                GROUP_EXECUTE
                WORLD_READ
                WORLD_EXECUTE
        )
        message(STATUS "Installed Git pre-commit hook at ${_dest_hook}")
    else()
        message(STATUS "Git pre-commit hook already installed at ${_dest_hook}")
    endif()

    set(_missing_tools)

    find_program(
        CLANG_FORMAT_EXECUTABLE
        NAMES
            clang-format
            clang-format-19
            clang-format-18
    )
    if(NOT CLANG_FORMAT_EXECUTABLE)
        list(APPEND _missing_tools "clang-format")
    endif()

    find_program(GERSEMI_EXECUTABLE NAMES gersemi)
    if(NOT GERSEMI_EXECUTABLE)
        list(APPEND _missing_tools "gersemi")
    endif()

    if(_missing_tools)
        list(
            JOIN _missing_tools
            ", "
            _missing_tools_text
        )
        message(
            WARNING
            "Git pre-commit hook was installed, but required tools are missing: ${_missing_tools_text}. "
            "Install them to enable formatting checks."
        )
    else()
        message(STATUS "Git pre-commit hook tools available: clang-format, gersemi")
    endif()
endfunction()
