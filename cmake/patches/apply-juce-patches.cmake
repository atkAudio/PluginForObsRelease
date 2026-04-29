# Apply JUCE patches - skips if already applied
cmake_minimum_required(VERSION 3.16)

find_package(Git REQUIRED)

# Get the directory containing the patches (this script's directory)
get_filename_component(PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

# Find all juce*.patch files in this directory
file(GLOB PATCHES "${PATCH_DIR}/juce*.patch")

foreach(PATCH_FILE ${PATCHES})
    get_filename_component(PATCH_NAME "${PATCH_FILE}" NAME)

    if(NOT EXISTS "${PATCH_FILE}")
        message(WARNING "Patch file not found: ${PATCH_FILE}")
        continue()
    endif()

    # Check if patch is already applied (reverse check succeeds if applied)
    execute_process(
        COMMAND
            ${GIT_EXECUTABLE} apply --reverse --check "${PATCH_FILE}"
        RESULT_VARIABLE REVERSE_CHECK_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if(REVERSE_CHECK_RESULT EQUAL 0)
        message(STATUS "Patch already applied: ${PATCH_NAME}")
    else()
        # Check if patch can be applied
        execute_process(
            COMMAND
                ${GIT_EXECUTABLE} apply --check "${PATCH_FILE}"
            RESULT_VARIABLE CHECK_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
        )

        if(CHECK_RESULT EQUAL 0)
            # Apply the patch
            execute_process(
                COMMAND
                    ${GIT_EXECUTABLE} apply "${PATCH_FILE}"
                RESULT_VARIABLE APPLY_RESULT
            )

            if(APPLY_RESULT EQUAL 0)
                message(STATUS "Successfully applied patch: ${PATCH_NAME}")
            else()
                message(WARNING "Failed to apply patch: ${PATCH_NAME}")
            endif()
        else()
            message(STATUS "Patch cannot be applied (may be partially applied or conflict): ${PATCH_NAME}")
        endif()
    endif()
endforeach()
