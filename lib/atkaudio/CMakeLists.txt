file(GLOB_RECURSE 
    _sources CONFIGURE_DEPENDS 
    ./src/*.c*
    )

target_sources(${PROJECT_NAME} PRIVATE ${_sources})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

cmake_policy(SET CMP0177 NEW)

add_subdirectory(JUCE EXCLUDE_FROM_ALL)
set(target ${PROJECT_NAME})

_juce_initialise_target(${target} ${target})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
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
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1 # we use QT event loop, so this is needed
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    JUCE_NEEDS_WEB_BROWSER FALSE
    JUCE_NEEDS_CURL TRUE
    JUCE_IS_PLUGIN TRUE
)

if(UNIX AND NOT APPLE)
    target_compile_definitions(
        ${PROJECT_NAME} 
        PRIVATE 
        JUCE_PLUGINHOST_LV2=1
        JUCE_PLUGINHOST_LADSPA=1
    )
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE INTERNAL "")

    target_compile_options(
        ${PROJECT_NAME} 
        PRIVATE 
        /wd4267
        /wd5105
        )
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

if(WIN32)
    file(TO_CMAKE_PATH "${CMAKE_INSTALL_PREFIX}" _sanitized_prefix)
    string(REGEX REPLACE "/$" "" _sanitized_prefix "${_sanitized_prefix}")
    set(CMAKE_INSTALL_PREFIX "${_sanitized_prefix}" CACHE PATH "Sanitized install prefix" FORCE)
endif()

if(BUILD_WITH_ASIO)
    message(STATUS "Building with ASIO")
    target_compile_definitions(${PROJECT_NAME} PRIVATE JUCE_ASIO=1)
endif()

