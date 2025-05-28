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