file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _buildspec_json)
string(JSON AUTHOR GET "${_buildspec_json}" author)
string(JSON EMAIL GET "${_buildspec_json}" email)
string(JSON VERSION GET "${_buildspec_json}" version)
string(JSON WEBSITE GET "${_buildspec_json}" website)
string(JSON DISPLAYNAME GET "${_buildspec_json}" displayName)
string(JSON PATHNAME GET "${_buildspec_json}" name)

# Define ATK_CI_BUILD for CI builds
if(DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
  add_compile_definitions(ATK_CI_BUILD)
endif()

# Define ATK_DEBUG for Debug/RelWithDebInfo builds when not in CI
if((CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo") AND NOT DEFINED ENV{CI} AND NOT DEFINED ENV{GITHUB_ACTIONS})
  add_compile_definitions(ATK_DEBUG)
endif()

# Match JUCE's recommended config flags for Release and RelWithDebInfo
if(UNIX)
  # Fast math for all builds
  string(APPEND CMAKE_C_FLAGS " -ffast-math")
  string(APPEND CMAKE_CXX_FLAGS " -ffast-math")
  
  string(APPEND CMAKE_C_FLAGS_RELEASE " -g -O3")
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " -g -O3")
  string(APPEND CMAKE_C_FLAGS_RELWITHDEBINFO " -g -O3")
  string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -g -O3")
  
  if(APPLE)
    string(APPEND CMAKE_OBJC_FLAGS " -ffast-math")
    string(APPEND CMAKE_OBJCXX_FLAGS " -ffast-math")
    string(APPEND CMAKE_OBJC_FLAGS_RELEASE " -g -O3")
    string(APPEND CMAKE_OBJCXX_FLAGS_RELEASE " -g -O3")
    string(APPEND CMAKE_OBJC_FLAGS_RELWITHDEBINFO " -g -O3")
    string(APPEND CMAKE_OBJCXX_FLAGS_RELWITHDEBINFO " -g -O3")
  endif()
  
  # LTO flags (match juce_recommended_lto_flags) - only on CI for faster local builds
  if(DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
    add_compile_options($<$<CONFIG:Release>:-flto>)
    add_link_options($<$<CONFIG:Release>:-flto>)
    add_compile_options($<$<CONFIG:RelWithDebInfo>:-flto>)
    add_link_options($<$<CONFIG:RelWithDebInfo>:-flto>)
  endif()
  
elseif(WIN32)
  # Fast math for all builds
  add_compile_options(/fp:fast)
  
  # Match JUCE: /Ox for Release, /Zi for debug symbols
  add_compile_options(
    $<$<CONFIG:Release>:/Zi>
    $<$<CONFIG:Release>:/Ox>
    $<$<CONFIG:Release>:/MP>
    $<$<CONFIG:RelWithDebInfo>:/Zi>
    $<$<CONFIG:RelWithDebInfo>:/Ox>
    $<$<CONFIG:RelWithDebInfo>:/MP>
  )
  
  # LTO flags (match juce_recommended_lto_flags) - only on CI for faster local builds
  if(DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
      add_compile_options($<$<CONFIG:Release>:/GL>)
      add_link_options($<$<CONFIG:Release>:/LTCG>)
      add_compile_options($<$<CONFIG:RelWithDebInfo>:/GL>)
      add_link_options($<$<CONFIG:RelWithDebInfo>:/LTCG>)
    else()
      # Clang-cl
      add_compile_options($<$<CONFIG:Release>:-flto>)
      add_compile_options($<$<CONFIG:RelWithDebInfo>:-flto>)
    endif()
  endif()
endif()

file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_BINARY_DIR}/README.txt" SYMBOLIC)
set(CPACK_RESOURCE_FILE_README "${CMAKE_BINARY_DIR}/README.txt")

configure_file("${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

set(CPACK_PACKAGE_VENDOR "${AUTHOR}")
# Use PATHNAME for package name (no spaces), DISPLAYNAME for display purposes
if(UNIX AND NOT APPLE)
  # Linux DEB requires package name without spaces
  set(CPACK_PACKAGE_NAME "${PATHNAME}")
else()
  set(CPACK_PACKAGE_NAME "${DISPLAYNAME}")
endif()

# Detect Windows target architecture once for all subsequent checks
if(WIN32)
  if(CMAKE_GENERATOR_PLATFORM)
    set(_win_target_arch "${CMAKE_GENERATOR_PLATFORM}")
  elseif(CMAKE_VS_PLATFORM_NAME)
    set(_win_target_arch "${CMAKE_VS_PLATFORM_NAME}")
  else()
    set(_win_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
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

# Hardcoded install replication - copy exact patterns from helpers with custom components

# Plugin component - replicate helper install patterns exactly
if(WIN32)
  # Windows: copy cmake/windows/helpers.cmake patterns
  install(TARGETS ${TARGET_NAME} 
    RUNTIME DESTINATION "${TARGET_NAME}/bin/64bit" 
    LIBRARY DESTINATION "${TARGET_NAME}/bin/64bit"
    COMPONENT plugin)

  # Include PDB files for debugging (x64 only, not ARM64)
  if(NOT _win_target_arch STREQUAL "ARM64")
    install(FILES $<TARGET_PDB_FILE:${TARGET_NAME}> 
      DESTINATION "${TARGET_NAME}/bin/64bit" 
      COMPONENT plugin 
      OPTIONAL)
  endif()

  # Windows data install pattern
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" 
      DESTINATION "${TARGET_NAME}/data" 
      USE_SOURCE_PERMISSIONS 
      COMPONENT plugin)
  endif()

elseif(APPLE)
  # macOS: copy cmake/macos/helpers.cmake patterns
  install(TARGETS ${TARGET_NAME} 
    LIBRARY DESTINATION .
    COMPONENT plugin)

  # macOS data install pattern (same as Windows for packaging)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" 
      DESTINATION "${TARGET_NAME}/data" 
      USE_SOURCE_PERMISSIONS 
      COMPONENT plugin)
  endif()

else()
  # Linux: Install plugin to system directories
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_user_arch "64bit")
  else()
    set(_user_arch "32bit")
  endif()
  
  # Install .so file to system obs-plugins directory
  install(TARGETS ${TARGET_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
    COMPONENT plugin)

  # Linux data install to system obs-plugins directory  
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${TARGET_NAME}
      USE_SOURCE_PERMISSIONS
      COMPONENT plugin)
  endif()
  
  # Extract debug symbols during CPack install (CI builds only)
  if(DEFINED ENV{CI})
    if(NOT CMAKE_OBJCOPY)
      find_program(CMAKE_OBJCOPY objcopy)
    endif()
    
    if(CMAKE_OBJCOPY)
      install(CODE "
        if(DEFINED ENV{DESTDIR})
          set(SO_FILE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/obs-plugins/${TARGET_NAME}.so\")
        else()
          set(SO_FILE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/obs-plugins/${TARGET_NAME}.so\")
        endif()
        if(EXISTS \"\${SO_FILE}\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --only-keep-debug \"\${SO_FILE}\" \"${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.so.debug\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --strip-debug \"\${SO_FILE}\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=${TARGET_NAME}.so.debug \"\${SO_FILE}\")
        endif()
      " COMPONENT plugin)
    endif()
  endif()
  
endif()

# ============================================================================
# DEBUG SYMBOLS COMPONENT
# ============================================================================

if(WIN32)
  install(FILES $<TARGET_PDB_FILE:${TARGET_NAME}>
    DESTINATION .
    COMPONENT debugsymbols)
    
elseif(APPLE)
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TARGET_NAME}.plugin.dSYM"
    DESTINATION .
    COMPONENT debugsymbols)

elseif(UNIX AND DEFINED ENV{CI})
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.so.debug"
    DESTINATION .
    COMPONENT debugsymbols
    OPTIONAL)
endif()

# Function to create portable install pattern (flat structure for ZIP)
function(create_portable_installs target_name)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_portable_arch "64bit")
  else()
    set(_portable_arch "32bit")
  endif()
  
  message(STATUS "Creating portable install commands for target '${target_name}'")
  
  # Portable install uses flat obs-plugins structure regardless of platform
  install(TARGETS ${target_name}
    RUNTIME DESTINATION obs-plugins/${_portable_arch}
    LIBRARY DESTINATION obs-plugins/${_portable_arch}
    COMPONENT portable)
  
  # Extract debug symbols for portable component (CI builds only)
  if(UNIX AND NOT APPLE AND DEFINED ENV{CI})
    if(NOT CMAKE_OBJCOPY)
      find_program(CMAKE_OBJCOPY objcopy)
    endif()
    
    if(CMAKE_OBJCOPY)
      install(CODE "
        if(DEFINED ENV{DESTDIR})
          set(SO_FILE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/obs-plugins/${_portable_arch}/${target_name}.so\")
        else()
          set(SO_FILE \"\${CMAKE_INSTALL_PREFIX}/obs-plugins/${_portable_arch}/${target_name}.so\")
        endif()
        if(EXISTS \"\${SO_FILE}\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --only-keep-debug \"\${SO_FILE}\" \"${CMAKE_CURRENT_BINARY_DIR}/${target_name}.so.debug\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --strip-debug \"\${SO_FILE}\")
          execute_process(COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=${target_name}.so.debug \"\${SO_FILE}\")
        endif()
      " COMPONENT portable)
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
set(CPACK_NSIS_COMPRESSOR "zlib")
file(TO_NATIVE_PATH "${CPACK_NSIS_INSTALL_ROOT}" CPACK_NSIS_INSTALL_ROOT)
string(REPLACE "\\" "\\\\" CPACK_NSIS_INSTALL_ROOT "${CPACK_NSIS_INSTALL_ROOT}")

set(ICON_PATH "${CMAKE_SOURCE_DIR}/lib/atkaudio/assets/icon.ico")
file(TO_CMAKE_PATH "${ICON_PATH}" ICON_PATH)
file(TO_NATIVE_PATH "${ICON_PATH}" ICON_PATH)
string(REPLACE "\\" "\\\\" ICON_PATH "${ICON_PATH}")
set(CPACK_NSIS_MUI_ICON "${ICON_PATH}")
set(CPACK_NSIS_MUI_UNIICON "${ICON_PATH}")
set(CPACK_NSIS_INSTALLED_ICON_NAME "${CMAKE_SOURCE_DIR}/lib/atkaudio/assets/icon.ico")
set(CPACK_PACKAGE_ICON "${ICON_PATH}")

if(WIN32)
  set(CPACK_GENERATOR "NSIS")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY " ") # for some reason this is required for NSIS
  set(CPACK_PACKAGE_EXTENSION "exe")
  
  # Configure NSIS for specific architectures
  if(_win_target_arch STREQUAL "ARM64")
    # Simple architecture check for ARM64 installer
    set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS "
      ; Simple architecture check for ARM64
      \${If} \${RunningX64}
        MessageBox MB_ICONSTOP 'This installer is for Windows ARM64 only. Please download the x64 version for your system.'
        Abort
      \${EndIf}
    ")
  else()
    # No strict architecture check for x64 (compatible with ARM64 via emulation)
    set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS "")
  endif()
elseif(APPLE)
  set(CPACK_GENERATOR "productbuild")
  
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
  # Linux - use DEB package format only (installs to system directories)
  set(CPACK_GENERATOR "DEB")
  
  # Use DESTDIR for system-wide installation
  set(CPACK_SET_DESTDIR ON)
  set(CPACK_INSTALL_PREFIX "/usr")
  
  # Configure DEB package specific settings
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${AUTHOR} <${EMAIL}>")
  set(CPACK_DEBIAN_PACKAGE_SECTION "video")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${WEBSITE}")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_LICENSE "AGPL-3.0")
  set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${DISPLAYNAME} for OBS Studio
 Audio plugin that provides advanced audio processing capabilities
 for OBS Studio streaming and recording software.")
  
  # System-wide installation may need obs-studio dependency
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "")
  
  # Control debug symbol package generation
  set(CPACK_DEBIAN_DEBUGINFO_PACKAGE OFF)
  
  # Linux-specific: Force CPack to use ONLY install() commands for plugin component
  set(CPACK_DEB_COMPONENT_INSTALL OFF)
  set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};plugin;/")
  set(CPACK_INSTALLED_DIRECTORIES)
  
  message(STATUS "CPack DEB will install plugin to system directories")
  message(STATUS "CPack DEB components: ${CPACK_COMPONENTS_ALL}")
  message(STATUS "CPack DEB generator: ${CPACK_GENERATOR}")
  message(STATUS "CPack DEB package file name: ${CPACK_PACKAGE_FILE_NAME}")
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

include(CPack)

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
include(CPack)
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
      COMMAND "${GIT_EXECUTABLE}" rev-parse --is-inside-work-tree
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE GIT_REPO_CHECK
      OUTPUT_QUIET
      ERROR_QUIET
    )
    
    if(GIT_REPO_CHECK EQUAL 0)
      message(STATUS "Source archive will be generated: ${SOURCE_ARCHIVE_NAME}")
      
      add_custom_command(
        OUTPUT "${SOURCE_ARCHIVE_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E remove -f "${SOURCE_ARCHIVE_OUTPUT}"
        COMMAND "${GIT_EXECUTABLE}" archive -o "${SOURCE_ARCHIVE_OUTPUT}" HEAD --worktree-attributes
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Creating source archive ${SOURCE_ARCHIVE_NAME}"
        VERBATIM
      )
      
      add_custom_target(generate_source_archive ALL
        DEPENDS "${SOURCE_ARCHIVE_OUTPUT}"
      )
      
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

# ============================================================================
# PORTABLE INSTALLER CONFIGURATION (Define before first CPack include)
# ============================================================================

# Detect architecture for portable installer
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PORTABLE_ARCH_DIR "64bit")
else()
  set(PORTABLE_ARCH_DIR "32bit")
endif()

# Install portable installer script for Linux only
if(UNIX AND NOT APPLE)
  install(
    FILES "${CMAKE_SOURCE_DIR}/lib/atkaudio/cmake/linux/install.sh"
    DESTINATION .
    COMPONENT portable
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
  )
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
  set(CPACK_PACKAGE_FILE_NAME "portable-${PATHNAME}-${PROJECT_VERSION}-${PORTABLE_OS_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
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

message(STATUS "Portable ZIP installer configuration:")
message(STATUS "  - Package name: ${CPACK_PACKAGE_FILE_NAME}")
message(STATUS "  - Architecture: ${PORTABLE_ARCH_DIR}")
message(STATUS "  - Component: portable")
message(STATUS "  - No top-level directory: ${CPACK_INCLUDE_TOPLEVEL_DIRECTORY}")
message(STATUS "  - Config file: ${CPACK_OUTPUT_CONFIG_FILE}")

# Suppress warning about second CPack inclusion (needed for dual installer generation)
# Temporarily disable all message output for the second CPack include
set(CMAKE_MESSAGE_LOG_LEVEL_BACKUP ${CMAKE_MESSAGE_LOG_LEVEL})
set(CMAKE_MESSAGE_LOG_LEVEL ERROR)
include(CPack)
set(CMAKE_MESSAGE_LOG_LEVEL ${CMAKE_MESSAGE_LOG_LEVEL_BACKUP})