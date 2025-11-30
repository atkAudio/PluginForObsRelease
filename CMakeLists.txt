cmake_minimum_required(VERSION 3.28...3.30)

include(./lib/atkaudio/cmake/preconfig.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake")

project(${_name} VERSION ${_version})

# Set default build type to RelWithDebInfo for local development
# RelWithDebInfo provides optimized code with debug symbols (PDB files on Windows)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the type of build (Debug, Release, or RelWithDebInfo)" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

# Read buildspec.json early for Qt configuration
file(READ "${CMAKE_SOURCE_DIR}/buildspec.json" buildspec)
string(JSON QT6_VERSION GET ${buildspec} dependencies qt6 version)

# Persistent dependency directory (survives cache clears)
if(WIN32)
    set(DEPS_DIR "${CMAKE_SOURCE_DIR}/_deps/${CMAKE_VS_PLATFORM_NAME}")
else()
    set(DEPS_DIR "${CMAKE_SOURCE_DIR}/_deps")
endif()

# Qt paths for Windows builds
if(WIN32)
    if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
        # ARM64 cross-compilation: use x64 Qt tools with ARM64 libraries
        set(QT_HOST_PATH "${CMAKE_SOURCE_DIR}/_deps/x64/obs-deps-qt6-${QT6_VERSION}-x64")
        set(Qt6_DIR "${DEPS_DIR}/obs-deps-qt6-${QT6_VERSION}-ARM64/lib/cmake/Qt6" CACHE PATH "" FORCE)
        set(Qt6CoreTools_DIR "${QT_HOST_PATH}/lib/cmake/Qt6CoreTools" CACHE PATH "" FORCE)
        set(Qt6GuiTools_DIR "${QT_HOST_PATH}/lib/cmake/Qt6GuiTools" CACHE PATH "" FORCE)
        set(Qt6WidgetsTools_DIR "${QT_HOST_PATH}/lib/cmake/Qt6WidgetsTools" CACHE PATH "" FORCE)
        set(CMAKE_AUTOMOC_MOC_EXECUTABLE "${QT_HOST_PATH}/bin/moc.exe" CACHE FILEPATH "" FORCE)
        set(CMAKE_AUTOUIC_UIC_EXECUTABLE "${QT_HOST_PATH}/bin/uic.exe" CACHE FILEPATH "" FORCE)
        set(CMAKE_AUTORCC_RCC_EXECUTABLE "${QT_HOST_PATH}/bin/rcc.exe" CACHE FILEPATH "" FORCE)
    else()
        set(Qt6_DIR "${DEPS_DIR}/obs-deps-qt6-${QT6_VERSION}-x64/lib/cmake/Qt6" CACHE PATH "" FORCE)
    endif()
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_FRONTEND_API)
endif()

# Qt is always required for proper window parenting
find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_QT)
target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
        -Wno-comma>
)
set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

file(GLOB_RECURSE _sources CONFIGURE_DEPENDS src/*.cpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${_sources})

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 23)

add_subdirectory(lib/atkaudio)

string(JSON PLUGIN_DISPLAY_NAME GET ${buildspec} displayName)
string(JSON PLUGIN_AUTHOR GET ${buildspec} author)
string(JSON PLUGIN_OBS_VERSION_REQUIRED GET ${buildspec} dependencies obs-studio version)
string(TIMESTAMP PLUGIN_YEAR "%Y")
set(PLUGIN_AUTHOR "${PLUGIN_AUTHOR}")

configure_file(src/config.h.in config.h @ONLY)

include(./lib/atkaudio/cmake/cpack.cmake)


# Build tests
include(CTest)
add_subdirectory(tests)

# Automatically run install after build (skip in CI or when installing to system directories without permissions)
if(NOT DEFINED ENV{CI} AND NOT DEFINED ENV{GITHUB_ACTIONS} AND NOT CMAKE_INSTALL_PREFIX MATCHES "^/usr")
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --component plugin
        COMMENT "Installing plugin after build..."
    )
endif()

# On non-CI Linux builds, copy plugin to user home directory after build
if(NOT DEFINED ENV{CI} AND NOT DEFINED ENV{GITHUB_ACTIONS} AND UNIX AND NOT APPLE)
    message(STATUS "Configuring post-build copy to ~/.config/obs-studio/plugins/")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_user_arch "64bit")
    else()
        set(_user_arch "32bit")
    endif()
    
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.config/obs-studio/plugins/${_name}/bin/${_user_arch}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "$ENV{HOME}/.config/obs-studio/plugins/${_name}/bin/${_user_arch}/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.config/obs-studio/plugins/${_name}/data"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "$ENV{HOME}/.config/obs-studio/plugins/${_name}/data"
        COMMENT "Copying ${_name} to user home OBS plugins directory..."
        VERBATIM
    )
endif()

