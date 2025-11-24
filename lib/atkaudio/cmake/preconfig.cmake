# Prefer system pkg-config over Linuxbrew on Linux (native builds only)
# Only applies when Linuxbrew is installed and not cross-compiling
if(UNIX AND NOT APPLE AND NOT CMAKE_CROSSCOMPILING)
    if(EXISTS "$ENV{HOME}/.linuxbrew" OR EXISTS "/home/linuxbrew/.linuxbrew")
        if(EXISTS "/usr/bin/pkg-config")
            # Force CMake to use system pkg-config executable
            set(PKG_CONFIG_EXECUTABLE "/usr/bin/pkg-config" CACHE FILEPATH "pkg-config executable" FORCE)
            # Override PKG_CONFIG_PATH to prioritize system libraries
            set(ENV{PKG_CONFIG_PATH} "/usr/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:/usr/local/lib/pkgconfig")
            message(STATUS "Using system pkg-config to avoid Linuxbrew library conflicts")
        endif()
    endif()
endif()

find_package(Git REQUIRED)
find_program(JQ_EXECUTABLE jq REQUIRED)

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(${GIT_BRANCH} MATCHES "release/")
    string(REPLACE "release/" "" GIT_BRANCH_STRIPPED "${GIT_BRANCH}")

    file(READ "${CMAKE_SOURCE_DIR}/buildspec.json" DATA)
    string(JSON DATA SET ${DATA} version "\"${GIT_BRANCH_STRIPPED}\"")
    file(WRITE "${CMAKE_SOURCE_DIR}/buildspec.json" "${DATA}")

    execute_process(
        COMMAND jq . "${CMAKE_SOURCE_DIR}/buildspec.json"
        OUTPUT_VARIABLE FORMATTED_JSON
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    file(WRITE "${CMAKE_SOURCE_DIR}/buildspec.json" "${FORMATTED_JSON}")
endif()