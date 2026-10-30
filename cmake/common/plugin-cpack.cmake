file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _buildspec_json)
string(JSON AUTHOR GET "${_buildspec_json}" author)
string(JSON EMAIL GET "${_buildspec_json}" email)
string(JSON VERSION GET "${_buildspec_json}" version)
string(JSON WEBSITE GET "${_buildspec_json}" website)
string(JSON DISPLAYNAME GET "${_buildspec_json}" displayName)
string(JSON PATHNAME GET "${_buildspec_json}" name)

file(
    CREATE_LINK
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_BINARY_DIR}/README.txt"
    SYMBOLIC
)
set(CPACK_RESOURCE_FILE_README "${CMAKE_BINARY_DIR}/README.txt")

configure_file("${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

set(CPACK_PACKAGE_VENDOR "${AUTHOR}")
# Use PATHNAME for package name (no spaces), DISPLAYNAME for display purposes
if(UNIX AND NOT APPLE)
    set(CPACK_PACKAGE_NAME "${PATHNAME}")
else()
    set(CPACK_PACKAGE_NAME "${DISPLAYNAME}")
endif()

# Detect Windows target architecture once for all subsequent checks
if(WIN32)
    atk_get_windows_target_arch(_win_target_arch)
endif()

# Include architecture in package name only for Windows ARM64
# Include build type in filename for local builds (non-CI)
if(WIN32)
    if(_win_target_arch STREQUAL "ARM64")
        set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-arm64)
    else()
        # Windows x64 - no architecture suffix
        set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME})
    endif()
    # Add build type for local builds
    if(NOT DEFINED ENV{CI})
        set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_FILE_NAME}-${CMAKE_BUILD_TYPE})
    endif()
elseif(APPLE)
    # macOS - no architecture suffix
    set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-macos)
    # Add build type for local builds
    if(NOT DEFINED ENV{CI})
        set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_FILE_NAME}-${CMAKE_BUILD_TYPE})
    endif()
else()
    set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR})
    # Add build type for local builds
    if(NOT DEFINED ENV{CI})
        set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_FILE_NAME}-${CMAKE_BUILD_TYPE})
    endif()
endif()

set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")

# Global component configuration - used by all platforms
set(CPACK_COMPONENTS_ALL plugin)
set(CPACK_COMPONENT_PLUGIN_DISPLAY_NAME "${DISPLAYNAME}")
set(CPACK_COMPONENT_PLUGIN_DESCRIPTION "${DISPLAYNAME} for OBS Studio")
set(CPACK_COMPONENT_PLUGIN_REQUIRED TRUE)

# Custom install commands with proper components
# Get the target name from buildspec
string(JSON TARGET_NAME GET "${_buildspec_json}" name)

# Native plugin installation is owned here, not duplicated in platform helpers.
if(WIN32)
    # NSIS selects the plugin folder itself; normal CMake installs select its parent.
    foreach(component IN ITEMS plugin installer)
        if(component STREQUAL "installer")
            set(destination .)
            set(data_destination data)
            set(exclude_from_all EXCLUDE_FROM_ALL)
        else()
            set(destination "${TARGET_NAME}")
            set(data_destination "${TARGET_NAME}/data")
            set(exclude_from_all "")
        endif()
        install(
            TARGETS
                ${TARGET_NAME}
            RUNTIME
                DESTINATION "${destination}"
                COMPONENT ${component}
            ${exclude_from_all}
            LIBRARY
                DESTINATION "${destination}"
                COMPONENT ${component}
            ${exclude_from_all}
        )

        if(TARGET ${TARGET_NAME}_scanner)
            install(
                TARGETS
                    ${TARGET_NAME}_scanner
                RUNTIME
                    DESTINATION "${destination}"
                    COMPONENT ${component}
                ${exclude_from_all}
            )
        endif()

        if(component STREQUAL "installer" OR NOT _win_target_arch STREQUAL "ARM64")
            install(
                FILES
                    $<TARGET_PDB_FILE:${TARGET_NAME}>
                DESTINATION "${destination}"
                COMPONENT ${component}
                OPTIONAL
                ${exclude_from_all}
            )
        endif()

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
            install(
                DIRECTORY
                    "${CMAKE_CURRENT_SOURCE_DIR}/data/"
                DESTINATION "${data_destination}"
                USE_SOURCE_PERMISSIONS
                COMPONENT ${component}
                ${exclude_from_all}
            )
        endif()
        install(
            FILES
                "${ATK_PLUGIN_MANIFEST}"
            DESTINATION "${data_destination}"
            COMPONENT ${component}
            ${exclude_from_all}
        )
    endforeach()
elseif(APPLE)
    install(
        TARGETS
            ${TARGET_NAME}
        LIBRARY
            DESTINATION .
            COMPONENT plugin
    )

    # Resources and scanner are already included in the bundle.
else()
    install(
        TARGETS
            ${TARGET_NAME}
        LIBRARY
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-modules/plugins
            COMPONENT plugin
    )

    # Install scanner alongside the main plugin (Linux)
    if(TARGET ${TARGET_NAME}_scanner)
        install(
            TARGETS
                ${TARGET_NAME}_scanner
            RUNTIME
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-modules/plugins
                COMPONENT plugin
        )
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
        install(
            DIRECTORY
                "${CMAKE_CURRENT_SOURCE_DIR}/data/"
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-modules/plugins/${TARGET_NAME}
            USE_SOURCE_PERMISSIONS
            COMPONENT plugin
        )
    endif()
    install(
        FILES
            "${ATK_PLUGIN_MANIFEST}"
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-modules/plugins/${TARGET_NAME}
        COMPONENT plugin
    )

    # Extract debug symbols during CPack install (CI builds only)
    if(DEFINED ENV{CI})
        if(NOT CMAKE_OBJCOPY)
            find_program(CMAKE_OBJCOPY objcopy)
        endif()

        if(CMAKE_OBJCOPY)
            install(
                CODE
                    "
        if(DEFINED ENV{DESTDIR})
          set(SO_FILE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/obs-modules/plugins/${TARGET_NAME}.so\")
        else()
          set(SO_FILE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/obs-modules/plugins/${TARGET_NAME}.so\")
        endif()
        if(EXISTS \"\${SO_FILE}\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --only-keep-debug \"\${SO_FILE}\" \"${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.so.debug\" COMMAND_ERROR_IS_FATAL ANY)
          execute_process(COMMAND ${CMAKE_OBJCOPY} --strip-debug \"\${SO_FILE}\" COMMAND_ERROR_IS_FATAL ANY)
          execute_process(COMMAND ${CMAKE_OBJCOPY} \"--add-gnu-debuglink=${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.so.debug\" \"\${SO_FILE}\" COMMAND_ERROR_IS_FATAL ANY)
        endif()
      "
                COMPONENT plugin
            )
        endif()
    endif()
endif()

# ============================================================================
# DEBUG SYMBOLS COMPONENT
# ============================================================================

if(WIN32)
    install(FILES $<TARGET_PDB_FILE:${TARGET_NAME}> DESTINATION . COMPONENT debugsymbols EXCLUDE_FROM_ALL)
elseif(APPLE)
    install(
        DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TARGET_NAME}.plugin.dSYM"
        DESTINATION .
        COMPONENT debugsymbols
        EXCLUDE_FROM_ALL
    )
elseif(UNIX AND DEFINED ENV{CI})
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.so.debug"
        DESTINATION .
        COMPONENT debugsymbols
        OPTIONAL
        EXCLUDE_FROM_ALL
    )
endif()

# Manual ZIP payloads use the OBS 33 per-plugin layout (a bundle on macOS).
function(create_portable_installs target_name)
    message(STATUS "Creating portable install commands for target '${target_name}'")
    if(APPLE)
        set(portable_destination .)
    else()
        set(portable_destination "plugins/${target_name}")
    endif()

    install(
        TARGETS
            ${target_name}
        RUNTIME
            DESTINATION "${portable_destination}"
            COMPONENT portable
            EXCLUDE_FROM_ALL
        LIBRARY
            DESTINATION "${portable_destination}"
            COMPONENT portable
            EXCLUDE_FROM_ALL
    )

    # Install scanner for portable component
    if(NOT APPLE AND TARGET ${target_name}_scanner)
        install(
            TARGETS
                ${target_name}_scanner
            RUNTIME
                DESTINATION "${portable_destination}"
                COMPONENT portable
                EXCLUDE_FROM_ALL
        )
    endif()

    if(NOT APPLE)
        install(
            DIRECTORY
                "${CMAKE_SOURCE_DIR}/data/"
            DESTINATION "${portable_destination}/data"
            COMPONENT portable
            EXCLUDE_FROM_ALL
        )
        install(
            FILES
                "${ATK_PLUGIN_MANIFEST}"
            DESTINATION "${portable_destination}/data"
            COMPONENT portable
            EXCLUDE_FROM_ALL
        )
    endif()

    # Extract debug symbols for portable component (CI builds only)
    if(UNIX AND NOT APPLE AND DEFINED ENV{CI})
        if(NOT CMAKE_OBJCOPY)
            find_program(CMAKE_OBJCOPY objcopy)
        endif()

        if(CMAKE_OBJCOPY)
            install(
                CODE
                    "
        if(DEFINED ENV{DESTDIR})
                    set(SO_FILE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${portable_destination}/${target_name}.so\")
        else()
                    set(SO_FILE \"\${CMAKE_INSTALL_PREFIX}/${portable_destination}/${target_name}.so\")
        endif()
        if(EXISTS \"\${SO_FILE}\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --only-keep-debug \"\${SO_FILE}\" \"${CMAKE_CURRENT_BINARY_DIR}/${target_name}.so.debug\" COMMAND_ERROR_IS_FATAL ANY)
          execute_process(COMMAND ${CMAKE_OBJCOPY} --strip-debug \"\${SO_FILE}\" COMMAND_ERROR_IS_FATAL ANY)
          execute_process(COMMAND ${CMAKE_OBJCOPY} \"--add-gnu-debuglink=${CMAKE_CURRENT_BINARY_DIR}/${target_name}.so.debug\" \"\${SO_FILE}\" COMMAND_ERROR_IS_FATAL ANY)
        endif()
      "
                COMPONENT portable
                EXCLUDE_FROM_ALL
            )
        endif()
    endif()
endfunction()

# Create portable install commands
create_portable_installs(${TARGET_NAME})

set(CPACK_NSIS_CONTACT "${EMAIL}")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_DISPLAY_NAME "${DISPLAYNAME}")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall ${DISPLAYNAME}")
set(CPACK_NSIS_INSTALL_ROOT "$COMMONPROGRAMDATA\\obs-studio\\plugins")
set(CPACK_NSIS_BRANDING_TEXT " ")
set(CPACK_NSIS_COMPRESSOR "/SOLID lzma")
file(
    TO_NATIVE_PATH
    "${CPACK_NSIS_INSTALL_ROOT}"
    CPACK_NSIS_INSTALL_ROOT
)
string(
    REPLACE "\\"
    "\\\\"
    CPACK_NSIS_INSTALL_ROOT
    "${CPACK_NSIS_INSTALL_ROOT}"
)

set(ICON_PATH "${CMAKE_SOURCE_DIR}/assets/icon.ico")
file(
    TO_CMAKE_PATH
    "${ICON_PATH}"
    ICON_PATH
)
file(
    TO_NATIVE_PATH
    "${ICON_PATH}"
    ICON_PATH
)
string(
    REPLACE "\\"
    "\\\\"
    ICON_PATH
    "${ICON_PATH}"
)
set(CPACK_NSIS_MUI_ICON "${ICON_PATH}")
set(CPACK_NSIS_MUI_UNIICON "${ICON_PATH}")
set(CPACK_NSIS_INSTALLED_ICON_NAME "icon.ico")
set(CPACK_PACKAGE_ICON "${ICON_PATH}")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PATHNAME}")
    set(CPACK_COMPONENTS_ALL installer)
    set(CPACK_COMPONENT_INSTALLER_DISPLAY_NAME "${DISPLAYNAME}")
    set(CPACK_COMPONENT_INSTALLER_REQUIRED TRUE)
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};installer;/")
    set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${PROJECT_NAME}")
    set(CPACK_PACKAGE_EXTENSION "exe")
    set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/windows/CPackConfigUnified.cmake")
elseif(APPLE)
    set(CPACK_GENERATOR "productbuild")

    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET must be set for macOS packaging")
    endif()
    # Picked up by cmake/macos/CPack.distribution.dist.in's <allowed-os-versions>
    # check, which fails the installer at install time on older macOS versions.
    set(CPACK_ATK_MIN_MACOS_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")

    # Use DESTDIR approach to ensure proper staging directory structure
    set(CPACK_SET_DESTDIR ON)
    set(CPACK_INSTALL_PREFIX "Library/Application Support/obs-studio/plugins")

    # Configure domains - user home only, no system-wide installation
    set(CPACK_PRODUCTBUILD_DOMAINS ON)
    set(CPACK_PRODUCTBUILD_DOMAINS_ANYWHERE OFF)
    set(CPACK_PRODUCTBUILD_DOMAINS_USER ON)
    set(CPACK_PRODUCTBUILD_DOMAINS_ROOT OFF)

    message(STATUS "CPack productbuild will install plugin to: ${CMAKE_INSTALL_PREFIX}")
else()
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_EXTENSION "tar.gz")
    set(CPACK_PACKAGE_FILE_NAME "${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_SET_DESTDIR OFF)
    set(CPACK_INSTALL_PREFIX "")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
    set(CPACK_COMPONENTS_ALL portable)
    set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};portable;/")
    set(CPACK_INSTALLED_DIRECTORIES "")
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/cmake/linux/install.sh" DESTINATION . COMPONENT portable EXCLUDE_FROM_ALL)
endif()

# Set package output directory to packages/ in the source root
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/packages")
set(CPACK_PACKAGE_ABSOLUTE_PATH ${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.${CPACK_PACKAGE_EXTENSION})

message(STATUS "CPack configuration - Generator: ${CPACK_GENERATOR}")
message(STATUS "CPack configuration - Components: ${CPACK_COMPONENTS_ALL}")
message(STATUS "CPack configuration - Install prefix: ${CPACK_INSTALL_PREFIX}")
message(STATUS "CPack configuration - Set DESTDIR: ${CPACK_SET_DESTDIR}")
message(STATUS "CPack configuration - Package directory: ${CPACK_PACKAGE_DIRECTORY}")
message(STATUS "CPack configuration - Package file name: ${CPACK_PACKAGE_FILE_NAME}")
message(STATUS "CPack configuration - Install projects: ${CPACK_INSTALL_CMAKE_PROJECTS}")

# Include CMake's built-in CPack module explicitly to avoid local module shadowing.
set(_cmake_builtin_cpack "${CMAKE_ROOT}/Modules/CPack.cmake")
include("${_cmake_builtin_cpack}")

# ============================================================================
# DEBUG SYMBOLS INSTALLER CONFIGURATION
# ============================================================================
# Create separate ZIP installer for debug symbols component

# Determine debug symbols package name based on platform
if(WIN32)
    if(_win_target_arch STREQUAL "ARM64")
        set(DEBUG_ARCH "arm64")
    else()
        set(DEBUG_ARCH "x64")
    endif()
    set(DEBUG_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-windows-${DEBUG_ARCH}-debugsymbols")
elseif(APPLE)
    set(DEBUG_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-macos-universal-debugsymbols")
elseif(UNIX)
    set(DEBUG_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}-debugsymbols")
endif()

# Override settings for debug symbols package
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_EXTENSION "zip")
set(CPACK_PACKAGE_FILE_NAME "${DEBUG_PACKAGE_FILE_NAME}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/packages")
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};debugsymbols;/")
set(CPACK_COMPONENTS_ALL debugsymbols)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_SET_DESTDIR OFF)

# Explicitly disable directory-based packaging
set(CPACK_COMPONENTS_GROUPING IGNORE)
set(CPACK_INSTALLED_DIRECTORIES "")

set(CPACK_OUTPUT_CONFIG_FILE "${CMAKE_BINARY_DIR}/CPackConfigDebugSymbols.cmake")

message(STATUS "Debug symbols ZIP installer configuration:")
message(STATUS "  - Package name: ${CPACK_PACKAGE_FILE_NAME}")
message(STATUS "  - Component: debugsymbols ONLY")
message(STATUS "  - Install projects: ${CPACK_INSTALL_CMAKE_PROJECTS}")
message(STATUS "  - Config file: ${CPACK_OUTPUT_CONFIG_FILE}")

# Include CPack again for debug symbols config
set(CMAKE_MESSAGE_LOG_LEVEL_BACKUP ${CMAKE_MESSAGE_LOG_LEVEL})
set(CMAKE_MESSAGE_LOG_LEVEL ERROR)
include("${_cmake_builtin_cpack}")
set(CMAKE_MESSAGE_LOG_LEVEL ${CMAKE_MESSAGE_LOG_LEVEL_BACKUP})

# ============================================================================
# SOURCE ARCHIVE GENERATION
# ============================================================================
# Only generate on Linux x86_64 to avoid duplicate artifacts
if(UNIX AND NOT APPLE AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64"))
    set(SOURCE_ARCHIVE_NAME "${PATHNAME}-${PROJECT_VERSION}-source.tar.gz")
    set(SOURCE_ARCHIVE_OUTPUT "${CPACK_PACKAGE_DIRECTORY}/${SOURCE_ARCHIVE_NAME}")

    find_package(Git QUIET)

    if(GIT_FOUND)
        execute_process(
            COMMAND
                "${GIT_EXECUTABLE}" rev-parse --is-inside-work-tree
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE GIT_REPO_CHECK
            OUTPUT_QUIET
            ERROR_QUIET
        )

        if(GIT_REPO_CHECK EQUAL 0)
            message(STATUS "Source archive will be generated: ${SOURCE_ARCHIVE_NAME}")

            add_custom_command(
                OUTPUT
                    "${SOURCE_ARCHIVE_OUTPUT}"
                COMMAND
                    ${CMAKE_COMMAND} -E remove -f "${SOURCE_ARCHIVE_OUTPUT}"
                COMMAND
                    "${GIT_EXECUTABLE}" archive -o "${SOURCE_ARCHIVE_OUTPUT}" HEAD --worktree-attributes
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                COMMENT "Creating source archive ${SOURCE_ARCHIVE_NAME}"
                VERBATIM
            )

            add_custom_target(generate_source_archive ALL DEPENDS "${SOURCE_ARCHIVE_OUTPUT}")

            message(STATUS "Source archive location: ${SOURCE_ARCHIVE_OUTPUT}")
        else()
            message(STATUS "Not in a git repository - source archive will not be generated")
        endif()
    else()
        message(STATUS "Git not found - source archive will not be generated")
    endif()
else()
    message(STATUS "Source archive generation skipped (only Linux x86_64)")
endif()

# Configure portable installer CPack settings
set(CPACK_GENERATOR "ZIP")
set(PORTABLE_OS_NAME ${CMAKE_SYSTEM_NAME})
if(APPLE)
    set(PORTABLE_OS_NAME "macos")
endif()
set(CPACK_PACKAGE_FILE_NAME "portable-${PATHNAME}-${PROJECT_VERSION}-${PORTABLE_OS_NAME}")
if(WIN32 AND _win_target_arch STREQUAL "ARM64")
    set(CPACK_PACKAGE_FILE_NAME "portable-${PATHNAME}-${PROJECT_VERSION}-${PORTABLE_OS_NAME}-arm64")
endif()
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_EXTENSION "tar.gz")
    set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
    set(CPACK_PACKAGE_FILE_NAME "${PATHNAME}-${PROJECT_VERSION}-${PORTABLE_OS_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Use portable component only
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};portable;/")
set(CPACK_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL "portable")

# Ensure no directory-based packaging
set(CPACK_INSTALLED_DIRECTORIES "")

# Remove install prefix for portable (direct structure)
set(CPACK_INSTALL_PREFIX "")
set(CPACK_SET_DESTDIR OFF)

# Configure ZIP to not create package directory wrapper
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

# Set custom config file path for portable installer
set(CPACK_OUTPUT_CONFIG_FILE "${CMAKE_BINARY_DIR}/CPackConfigPortable.cmake")

message(STATUS "Portable ${CPACK_GENERATOR} installer configuration:")
message(STATUS "  - Package name: ${CPACK_PACKAGE_FILE_NAME}")
message(STATUS "  - Component: portable")
message(STATUS "  - No top-level directory: ${CPACK_INCLUDE_TOPLEVEL_DIRECTORY}")
message(STATUS "  - Config file: ${CPACK_OUTPUT_CONFIG_FILE}")

# Suppress warning about second CPack inclusion (needed for dual installer generation)
# Temporarily disable all message output for the second CPack include
set(CMAKE_MESSAGE_LOG_LEVEL_BACKUP ${CMAKE_MESSAGE_LOG_LEVEL})
set(CMAKE_MESSAGE_LOG_LEVEL ERROR)
include("${_cmake_builtin_cpack}")
set(CMAKE_MESSAGE_LOG_LEVEL ${CMAKE_MESSAGE_LOG_LEVEL_BACKUP})

# macOS: Copy scanner into bundle and re-sign after build
if(APPLE AND TARGET ${TARGET_NAME}_scanner)
    # Use proper identity for CI, ad-hoc for local development
    if(CODESIGN_IDENTITY)
        set(_sign_identity "${CODESIGN_IDENTITY}")
    else()
        set(_sign_identity "-")
    endif()

    add_custom_command(
        TARGET ${TARGET_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${TARGET_NAME}_scanner>
            "$<TARGET_BUNDLE_DIR:${TARGET_NAME}>/Contents/MacOS/"
        COMMAND
            codesign --force --sign "${_sign_identity}" --deep --timestamp -o runtime
            "$<TARGET_BUNDLE_DIR:${TARGET_NAME}>"
        VERBATIM
        COMMENT "Adding scanner to bundle and re-signing"
    )
endif()
