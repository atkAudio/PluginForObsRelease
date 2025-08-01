file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _buildspec_json)
string(JSON AUTHOR GET "${_buildspec_json}" author)
string(JSON EMAIL GET "${_buildspec_json}" email)
string(JSON VERSION GET "${_buildspec_json}" version)
string(JSON WEBSITE GET "${_buildspec_json}" website)
string(JSON DISPLAYNAME GET "${_buildspec_json}" displayName)
string(JSON PATHNAME GET "${_buildspec_json}" name)

file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_BINARY_DIR}/README.txt" SYMBOLIC)
set(CPACK_RESOURCE_FILE_README "${CMAKE_BINARY_DIR}/README.txt")

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/INSTALLER-LICENSE")

set(CPACK_PACKAGE_VENDOR "${AUTHOR}")
set(CPACK_PACKAGE_NAME "${DISPLAYNAME}")
set(CPACK_PACKAGE_FILE_NAME ${PATHNAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME})

set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")

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
else()
  set(CPACK_GENERATOR "productbuild")
  set(CPACK_PACKAGE_EXTENSION "pkg")
endif()


set(CPACK_RELEASE_STAGING_DIRECTORY "${CMAKE_SOURCE_DIR}/release")
set(BUILD_TYPE_FOR_CPACK "Release")

set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_ABSOLUTE_PATH ${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.${CPACK_PACKAGE_EXTENSION})

if(NOT DEFINED ENV{CI})
  set(BUILD_TYPE_FOR_CPACK "Debug")
  return()
endif()
if(NOT WIN32)
  return()
endif()

include(CPack)

if(NOT EXISTS ${CPACK_PACKAGE_ABSOLUTE_PATH})
  add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND cpack --config CPackConfig.cmake -C ${BUILD_TYPE_FOR_CPACK}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

# hack
# Copy the package to the release staging directory
install(CODE
"
if(EXISTS \"${CPACK_PACKAGE_ABSOLUTE_PATH}\" AND EXISTS \"${CPACK_RELEASE_STAGING_DIRECTORY}\")
    message(STATUS \"Copying package to release staging directory: ${CPACK_RELEASE_STAGING_DIRECTORY}\")
    file(COPY \"${CPACK_PACKAGE_ABSOLUTE_PATH}\" DESTINATION \"${CPACK_RELEASE_STAGING_DIRECTORY}\")
endif()
"
)

