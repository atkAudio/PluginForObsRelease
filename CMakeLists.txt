cmake_minimum_required(VERSION 3.28...3.30)

include(./lib/atkaudio/cmake/preconfig.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake")

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Core)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
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
endif()

file(GLOB_RECURSE _sources CONFIGURE_DEPENDS src/*.cpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${_sources})

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 23)

add_subdirectory(lib/atkaudio)

file(READ "${CMAKE_SOURCE_DIR}/buildspec.json" buildspec)
string(JSON PLUGIN_DISPLAY_NAME GET ${buildspec} displayName)

configure_file(src/config.h.in config.h @ONLY)

include(./lib/atkaudio/cmake/cpack.cmake)
