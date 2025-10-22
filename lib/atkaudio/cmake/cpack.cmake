file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _buildspec_json)
string(JSON AUTHOR GET "${_buildspec_json}" author)
string(JSON EMAIL GET "${_buildspec_json}" email)
string(JSON VERSION GET "${_buildspec_json}" version)
string(JSON WEBSITE GET "${_buildspec_json}" website)
string(JSON DISPLAYNAME GET "${_buildspec_json}" displayName)
string(JSON PATHNAME GET "${_buildspec_json}" name)

file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_BINARY_DIR}/README.txt" SYMBOLIC)
set(CPACK_RESOURCE_FILE_README "${CMAKE_BINARY_DIR}/README.txt")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE-AGPL3" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

set(CPACK_PACKAGE_VENDOR "${AUTHOR}")
set(CPACK_PACKAGE_NAME "${DISPLAYNAME}")

# Include architecture in package name only for Windows ARM64
if(WIN32)
  if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
    set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-arm64)
  else()
    # Windows x64 - no architecture suffix
    set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME})
  endif()
elseif(APPLE)
  # macOS - no architecture suffix
  set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-macos)
else()
  set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR})
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

  install(FILES "$<TARGET_PDB_FILE:${TARGET_NAME}>"
    CONFIGURATIONS RelWithDebInfo Debug
    DESTINATION "${TARGET_NAME}/bin/64bit"
    COMPONENT plugin
    OPTIONAL)

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
    
  install(FILES "$<TARGET_BUNDLE_DIR:${TARGET_NAME}>.dsym" 
    CONFIGURATIONS Release 
    DESTINATION . 
    COMPONENT plugin
    OPTIONAL)

  # macOS data install pattern (same as Windows for packaging)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" 
      DESTINATION "${TARGET_NAME}/data" 
      USE_SOURCE_PERMISSIONS 
      COMPONENT plugin)
  endif()

else()
  # Linux: copy cmake/linux/helpers.cmake patterns
  install(TARGETS ${TARGET_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
    COMPONENT plugin)

  # Linux data install pattern
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${TARGET_NAME}
      USE_SOURCE_PERMISSIONS
      COMPONENT plugin)
  endif()
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

  # Portable data goes to flat data/obs-plugins structure
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" 
      DESTINATION data/obs-plugins/${target_name}
      USE_SOURCE_PERMISSIONS 
      COMPONENT portable)
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
  if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
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
  # Linux - use DEB package format only
  set(CPACK_GENERATOR "DEB")
  
  # Use DESTDIR approach to ensure proper staging directory structure
  set(CPACK_SET_DESTDIR ON)
  set(CPACK_INSTALL_PREFIX "/usr")
  
  # Configure DEB package specific settings
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${AUTHOR} <${EMAIL}>")
  set(CPACK_DEBIAN_PACKAGE_SECTION "video")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${WEBSITE}")
  set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${DISPLAYNAME} for OBS Studio
 Audio plugin that provides advanced audio processing capabilities
 for OBS Studio streaming and recording software.")
  
  # Set package dependencies - OBS Studio is typically required
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "obs-studio")
  
  # Linux-specific: Force CPack to use ONLY install() commands for plugin component
  # This ensures only the installed plugin component (binary + locale) is packaged
  set(CPACK_DEB_COMPONENT_INSTALL OFF)
  set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};plugin;/")
  # Completely disable directory-based packaging for Linux
  set(CPACK_INSTALLED_DIRECTORIES)
  
  message(STATUS "CPack DEB will install plugin to: ${CPACK_INSTALL_PREFIX}")
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
# PORTABLE INSTALLER CONFIGURATION (Define before first CPack include)
# ============================================================================

# Detect architecture for portable installer
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PORTABLE_ARCH_DIR "64bit")
else()
  set(PORTABLE_ARCH_DIR "32bit")
endif()

# Install locale files for portable component (target install is in helpers.cmake)
install(
  DIRECTORY "${CMAKE_SOURCE_DIR}/data/locale/"
  DESTINATION data/obs-plugins/${PATHNAME}/locale
  COMPONENT portable
  FILES_MATCHING PATTERN "*.ini"
)

# Configure portable installer CPack settings
set(CPACK_GENERATOR "ZIP")
set(PORTABLE_OS_NAME ${CMAKE_SYSTEM_NAME})
if(APPLE)
  set(PORTABLE_OS_NAME "macos")
endif()
set(CPACK_PACKAGE_FILE_NAME "portable-${PATHNAME}-${PROJECT_VERSION}-${PORTABLE_OS_NAME}")
if(WIN32 AND CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
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