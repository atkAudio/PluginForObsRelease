cmake_minimum_required(VERSION 3.28)

include(./cmake/common/preconfig.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake")

project(${_name} VERSION ${_version})

# Keep FetchContent downloads and source trees under the repository's _deps directory.
set(FETCHCONTENT_BASE_DIR
    "${CMAKE_SOURCE_DIR}/_deps"
    CACHE PATH
    "Directory for FetchContent downloads and source trees"
    FORCE
)

# Set default build type to RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        "RelWithDebInfo"
        CACHE STRING
        "Choose the type of build (Debug, Release, or RelWithDebInfo)"
        FORCE
    )
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

include(compilerconfig)
include(defaults)
include(helpers)
include(./cmake/common/qt_setup.cmake)
include(./cmake/common/target_policies.cmake)
include(./cmake/common/platform_utils.cmake)

# Read buildspec.json early for Qt configuration
file(READ "${CMAKE_SOURCE_DIR}/buildspec.json" buildspec)
string(
    JSON QT6_VERSION
    GET ${buildspec}
    dependencies
    qt6
    version
)

# Define architecture-specific dependency directory in the build directory
# Use CMAKE_BINARY_DIR so it works with any configured build directory
set(DEPS_DIR "${CMAKE_BINARY_DIR}/obs-deps")

# Configure Qt package roots for Windows builds, with the ARM64-specific host-tool override.
if(WIN32)
    atk_configure_qt_paths(${QT6_VERSION} ${DEPS_DIR})
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_FRONTEND_API)

# Qt is always required for proper window parenting
find_package(
    Qt6
    COMPONENTS
        Widgets
        Core
    REQUIRED
)
target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
        Qt6::Core
        Qt6::Widgets
)
target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
        $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
        -Wno-comma>
)
set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES
        AUTOMOC
            ON
        AUTOUIC
            ON
        AUTORCC
            ON
)

add_subdirectory(src)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_23)
set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES
        CXX_STANDARD_REQUIRED
            ON
        CXX_EXTENSIONS
            OFF
)

string(JSON PLUGIN_DISPLAY_NAME GET ${buildspec} displayName)
string(JSON PLUGIN_AUTHOR GET ${buildspec} author)
string(JSON PLUGIN_WEBSITE GET ${buildspec} website)
string(
    JSON PLUGIN_OBS_VERSION_REQUIRED
    GET ${buildspec}
    dependencies
    obs-studio
    version
)
string(TIMESTAMP PLUGIN_YEAR "%Y")
set(PLUGIN_AUTHOR "${PLUGIN_AUTHOR}")

configure_file(src/config.h.in config.h @ONLY)

include(./cmake/common/plugin-cpack.cmake)
include(./cmake/common/install_after_build.cmake)

# Build tests
include(CTest)
add_subdirectory(tests)

atk_setup_git_hooks()
atk_configure_local_post_build_deploy(${CMAKE_PROJECT_NAME} ${_name})
