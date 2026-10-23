cmake_minimum_required(VERSION 3.28)

if(DEFINED CPACK_GENERATOR AND NOT CPACK_GENERATOR STREQUAL "NSIS")
    return()
endif()

get_filename_component(ATK_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(READ "${ATK_PROJECT_ROOT}/buildspec.json" buildspec)
string(JSON ATK_NAME GET "${buildspec}" name)
string(JSON ATK_VERSION GET "${buildspec}" version)
string(JSON ATK_DISPLAY_NAME GET "${buildspec}" displayName)
string(JSON ATK_VENDOR GET "${buildspec}" author)
string(JSON ATK_WEBSITE GET "${buildspec}" website)

set(CPACK_PACKAGE_NAME "${ATK_DISPLAY_NAME}")
set(CPACK_PACKAGE_VERSION "${ATK_VERSION}")
if(NOT DEFINED CPACK_PACKAGE_DIRECTORY)
    set(CPACK_PACKAGE_DIRECTORY "${ATK_PROJECT_ROOT}/packages")
endif()
if(NOT DEFINED CPACK_GENERATOR AND NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    set(CPACK_GENERATOR NSIS)
    set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_FILE}")
    return()
endif()

if(NOT DEFINED ATK_CONFIGURATION)
    set(ATK_CONFIGURATION "${CPACK_BUILD_CONFIG}")
endif()
if(NOT ATK_CONFIGURATION)
    set(ATK_CONFIGURATION RelWithDebInfo)
endif()
if(NOT DEFINED ATK_X64_PAYLOAD)
    set(ATK_X64_PAYLOAD "${ATK_PROJECT_ROOT}/build_x64/installer-payload")
endif()
if(NOT DEFINED ATK_ARM64_PAYLOAD)
    set(ATK_ARM64_PAYLOAD "${ATK_PROJECT_ROOT}/build_arm64/installer-payload")
endif()

foreach(architecture IN ITEMS x64 arm64)
    string(TOUPPER "${architecture}" architecture_upper)
    set(payload "${ATK_${architecture_upper}_PAYLOAD}")
    file(READ "${payload}/payload.json" descriptor)
    foreach(
        field
        IN
        ITEMS
            architecture
            name
            version
            configuration
            revision
    )
        string(JSON value GET "${descriptor}" "${field}")
        if(field STREQUAL "revision")
            if(NOT value OR (DEFINED revision AND NOT value STREQUAL revision))
                message(FATAL_ERROR "Installer payload source revisions differ or are missing")
            endif()
            set(revision "${value}")
        else()
            if(field STREQUAL "architecture")
                set(expected "${architecture}")
            elseif(field STREQUAL "configuration")
                set(expected "${ATK_CONFIGURATION}")
            else()
                string(TOUPPER "${field}" field_upper)
                set(expected "${ATK_${field_upper}}")
            endif()
            if(NOT value STREQUAL expected)
                message(FATAL_ERROR "${architecture} payload ${field}: expected ${expected}, got ${value}")
            endif()
        endif()
    endforeach()
    foreach(binary IN ITEMS "${ATK_NAME}.dll" "${ATK_NAME}_scanner.exe")
        if(NOT EXISTS "${payload}/files/${binary}")
            message(FATAL_ERROR "Missing ${architecture} installer binary: ${binary}")
        endif()
        file(READ "${payload}/files/${binary}" signature OFFSET 0 LIMIT 2 HEX)
        file(READ "${payload}/files/${binary}" offset_bytes OFFSET 60 LIMIT 4 HEX)
        if(
            NOT signature
                STREQUAL
                "4d5a"
            OR NOT offset_bytes
                MATCHES
                "^([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])$"
        )
            message(FATAL_ERROR "Invalid PE binary: ${binary}")
        endif()
        math(EXPR pe_offset "0x${CMAKE_MATCH_4}${CMAKE_MATCH_3}${CMAKE_MATCH_2}${CMAKE_MATCH_1}")
        file(READ "${payload}/files/${binary}" pe_header OFFSET ${pe_offset} LIMIT 6 HEX)
        if(architecture STREQUAL "x64")
            set(expected_header "504500006486")
        else()
            set(expected_header "5045000064aa")
        endif()
        if(NOT pe_header STREQUAL expected_header)
            message(FATAL_ERROR "${binary} does not match ${architecture} PE architecture")
        endif()
    endforeach()
    file(READ "${payload}/files/data/manifest.json" manifest)
    string(JSON manifest_version GET "${manifest}" version)
    if(NOT manifest_version STREQUAL ATK_VERSION)
        message(FATAL_ERROR "${architecture} manifest version does not match ${ATK_VERSION}")
    endif()
endforeach()

set(ATK_DATA_DELETE_COMMANDS "")
set(data_files "")
set(data_directories "")
foreach(payload IN ITEMS "${ATK_X64_PAYLOAD}" "${ATK_ARM64_PAYLOAD}")
    file(GLOB_RECURSE entries LIST_DIRECTORIES TRUE RELATIVE "${payload}/files" "${payload}/files/data/*")
    foreach(entry IN LISTS entries)
        if(IS_DIRECTORY "${payload}/files/${entry}")
            list(APPEND data_directories "${entry}")
        else()
            list(APPEND data_files "${entry}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES data_files)
list(REMOVE_DUPLICATES data_directories)
list(SORT data_directories ORDER DESCENDING)
foreach(entry IN LISTS data_files)
    string(
        REPLACE "/"
        "\\"
        entry
        "${entry}"
    )
    string(APPEND ATK_DATA_DELETE_COMMANDS "  Delete \"$INSTDIR\\${entry}\"\n")
endforeach()
foreach(entry IN LISTS data_directories)
    string(
        REPLACE "/"
        "\\"
        entry
        "${entry}"
    )
    string(APPEND ATK_DATA_DELETE_COMMANDS "  RMDir \"$INSTDIR\\${entry}\"\n")
endforeach()

set(CPACK_GENERATOR NSIS)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/resources")
set(CPACK_PACKAGE_NAME "${ATK_DISPLAY_NAME}")
set(CPACK_PACKAGE_VENDOR "${ATK_VENDOR}")
set(CPACK_PACKAGE_VERSION "${ATK_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION "${ATK_DISPLAY_NAME} for OBS Studio")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME "${ATK_NAME}-${ATK_VERSION}-Windows")
if(NOT DEFINED ENV{CI})
    string(APPEND CPACK_PACKAGE_FILE_NAME "-${ATK_CONFIGURATION}")
endif()
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${ATK_NAME}")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${ATK_NAME}")
set(CPACK_RESOURCE_FILE_LICENSE "${ATK_PROJECT_ROOT}/LICENSE")
file(
    TO_NATIVE_PATH
    "${ATK_PROJECT_ROOT}/assets/icon.ico"
    CPACK_NSIS_MUI_ICON
)
set(CPACK_NSIS_MUI_UNIICON "${CPACK_NSIS_MUI_ICON}")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_COMPONENTS_ALL "")
set(CPACK_INSTALLED_DIRECTORIES "${ATK_X64_PAYLOAD}/files;x64;${ATK_ARM64_PAYLOAD}/files;arm64")
