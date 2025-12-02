file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ./src/*.c*)

target_sources(${PROJECT_NAME} PRIVATE ${_sources})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# CMP0177 is only available in CMake 3.31+
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

set(target ${PROJECT_NAME})

# Always use JUCE modules only (no helper tools)
set(JUCE_MODULES_ONLY ON CACHE BOOL "Only build JUCE modules" FORCE)

# Store FetchContent downloads in _deps to persist across cache clears
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/_deps" CACHE PATH "" FORCE)

include(FetchContent)
FetchContent_Declare(
    juce
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG
        230340dbfc580628596243bb63b31aeb4d6b5a6d # develop 2025-11-27
    PATCH_COMMAND
        ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/apply-juce-patches.cmake"
)
FetchContent_MakeAvailable(juce)
# _juce_initialise_target(${target} ${target})

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        # juce::juce_recommended_warning_flags
        OBS::obs-frontend-api
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
        # Disable DBG() output in CI/GitHub Actions RelWithDebInfo builds (keeps debug symbols but no console spam in CI)
        $<$<AND:$<CONFIG:RelWithDebInfo>,$<OR:$<BOOL:$ENV{CI}>,$<BOOL:$ENV{GITHUB_ACTIONS}>>>:JUCE_DISABLE_ASSERTIONS>
    # SIMULATE_UPDATE_CHECK # Uncomment to test update dialog without network
)

set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES
        JUCE_NEEDS_WEB_BROWSER
            FALSE
        JUCE_NEEDS_CURL
            TRUE
        JUCE_IS_PLUGIN
            TRUE
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
    target_compile_options(
        ${PROJECT_NAME}
        PRIVATE
            /wd4244
            /wd4267
            /wd4390
            /wd5105
    )
endif()

if(NOT WIN32)
    target_compile_options(
        ${PROJECT_NAME}
        PRIVATE
            -Wno-unused-parameter
            -Wno-sign-compare
            -Wno-unused-variable
            -Wno-newline-eof
    )
endif()

# ASIO SDK (Windows only)
if(WIN32)
    set(ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/_deps/asiosdk")
    if(NOT EXISTS "${ASIO_SDK_DIR}")
        set(_asio_zip "${CMAKE_SOURCE_DIR}/_deps/asiosdk.zip")
        set(_asio_temp "${CMAKE_SOURCE_DIR}/_deps/asiosdk_temp")
        message(STATUS "Downloading ASIO SDK...")
        file(DOWNLOAD "https://www.steinberg.net/asiosdk" "${_asio_zip}" STATUS _status SHOW_PROGRESS)
        list(GET _status 0 _code)
        if(_code EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${_asio_zip}" DESTINATION "${_asio_temp}")
            file(GLOB _dirs "${_asio_temp}/*")
            list(GET _dirs 0 _extracted)
            file(RENAME "${_extracted}" "${ASIO_SDK_DIR}")
            file(
                REMOVE_RECURSE
                "${_asio_temp}"
                "${_asio_zip}"
            )
            message(STATUS "ASIO SDK installed to ${ASIO_SDK_DIR}")
        endif()
    endif()
    target_include_directories(${PROJECT_NAME} PRIVATE "${ASIO_SDK_DIR}/common")
endif()

if(WIN32)
    file(TO_CMAKE_PATH "${CMAKE_INSTALL_PREFIX}" _sanitized_prefix)
    string(REGEX REPLACE "/$" "" _sanitized_prefix "${_sanitized_prefix}")
    set(CMAKE_INSTALL_PREFIX "${_sanitized_prefix}" CACHE PATH "Sanitized install prefix" FORCE)
endif()

if(BUILD_WITH_ASIO)
    message(STATUS "Building with ASIO")
    target_compile_definitions(${PROJECT_NAME} PRIVATE JUCE_ASIO=1)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE JUCE_ASIO=1)

file(COPY_FILE ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/INSTALLER-LICENSE)

# Build the sandboxed plugin scanner executable
add_subdirectory(src/scanner)
