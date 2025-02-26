cmake_minimum_required(VERSION 3.28)

project(atkaudio VERSION ${CMAKE_PROJECT_VERSION})

find_package(Git REQUIRED)

add_library(${PROJECT_NAME} SHARED)

file(GLOB_RECURSE 
    _sources CONFIGURE_DEPENDS 
    ./src/*.c*
    )

target_sources(${PROJECT_NAME} PRIVATE ${_sources})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(${PROJECT_NAME} PRIVATE BUILDING_DLL)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${PROJECT_NAME})

cmake_policy(SET CMP0177 NEW)

add_subdirectory(JUCE-7.0.12 EXCLUDE_FROM_ALL)
set(target ${PROJECT_NAME})
set_target_properties(${target} PROPERTIES
    JUCE_NEEDS_WEB_BROWSER TRUE
    JUCE_NEEDS_CURL TRUE
    JUCE_IS_PLUGIN TRUE
)

_juce_initialise_target(${target} ${target})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        # AudioPluginData           # If we'd created a binary data target, we'd link to it here
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        # juce::juce_recommended_warning_flags
       )

target_compile_definitions(
    ${PROJECT_NAME} 
    PRIVATE
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=1
    JUCE_USE_CURL=1
    JUCE_WEB_BROWSER=1
)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE INTERNAL "")

    target_compile_options(
        ${PROJECT_NAME} 
        PRIVATE 
        /wd4267
        )
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${PROJECT_NAME})

add_dependencies(${CMAKE_PROJECT_NAME} ${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
    set(destination "bin/64bit")
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${destination}" LIBRARY DESTINATION "${destination}")

    install(
    FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
    CONFIGURATIONS RelWithDebInfo Debug
    DESTINATION "${destination}"
    OPTIONAL
    )
elseif(APPLE)
    install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION $<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/Frameworks)
endif()

set(ASIO_SDK_URL "https://www.steinberg.net/asiosdk")
set(ASIO_SDK_TEMPDIR "${CMAKE_CURRENT_BINARY_DIR}/asiosdk_temp")
set(ASIO_SDK_DIR "${CMAKE_CURRENT_BINARY_DIR}/asiosdk")

if(NOT EXISTS "${ASIO_SDK_DIR}")
    message(STATUS "Downloading ASIO SDK...")
    file(DOWNLOAD "${ASIO_SDK_URL}" "${CMAKE_CURRENT_BINARY_DIR}/asiosdk.zip"
        STATUS download_status
        SHOW_PROGRESS
    )
    if(download_status)
        message(STATUS "Extracting ASIO SDK...")
        file(ARCHIVE_EXTRACT 
            INPUT "${CMAKE_CURRENT_BINARY_DIR}/asiosdk.zip"
            DESTINATION "${ASIO_SDK_TEMPDIR}"
        )
        file(GLOB ASIO_SDK_DIRS "${ASIO_SDK_TEMPDIR}/*")
        foreach(dir ${ASIO_SDK_DIRS})
            if(IS_DIRECTORY ${dir} AND dir MATCHES ".*asiosdk.*")
            message(STATUS "ASIO SDK extracted to ${dir}")
            file(RENAME ${dir} ${ASIO_SDK_DIR})
            break()
            endif()
        endforeach()
    endif()
endif()

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/asiosdk/common
)

target_include_directories(${PROJECT_NAME} PRIVATE "${ASIO_SDK_DIR}")

if(BUILD_WITH_ASIO)
    message(STATUS "Building with ASIO")
    target_compile_definitions(${PROJECT_NAME} PRIVATE JUCE_ASIO=1)
endif()

