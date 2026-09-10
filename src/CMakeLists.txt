# Single source-tree entry point for the plugin sources.
file(GLOB_RECURSE OBS_PLUGIN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER OBS_PLUGIN_SOURCES EXCLUDE REGEX [[.*/scanner/.*]])

target_sources(${PROJECT_NAME} PRIVATE ${OBS_PLUGIN_SOURCES})

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/core
)

# Always use JUCE modules only (no helper tools)
set(JUCE_MODULES_ONLY ON CACHE BOOL "Only build JUCE modules" FORCE)

# Persist JUCE across cache resets by reusing the repository's checked-out source tree.
set(_juce_source_dir "${CMAKE_SOURCE_DIR}/_deps/juce-src")
if(EXISTS "${_juce_source_dir}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_JUCE "${_juce_source_dir}" CACHE PATH "Source directory for JUCE" FORCE)
    message(STATUS "Using existing JUCE checkout at ${FETCHCONTENT_SOURCE_DIR_JUCE}")
endif()

include(FetchContent)
FetchContent_Declare(
    juce
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.2
    GIT_SHALLOW TRUE
    PATCH_COMMAND
        ${CMAKE_COMMAND} -P "${CMAKE_SOURCE_DIR}/cmake/patches/apply-juce-patches.cmake"
)
FetchContent_MakeAvailable(juce)

# JUCE module targets are INTERFACE targets, so suppress their known upstream
# diagnostics on each concrete target that compiles the module implementation
# units.
atk_suppress_juce_target_warnings(${PROJECT_NAME})
atk_apply_packaging_build_flags(${PROJECT_NAME})
atk_apply_juce_recommended_flags(${PROJECT_NAME})

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_dsp
)

target_compile_definitions(
    ${PROJECT_NAME}
    PRIVATE
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_WIN_PER_MONITOR_DPI_AWARE=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=1
        JUCE_USE_CURL=1
        JUCE_WEB_BROWSER=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        $<$<OR:$<BOOL:$ENV{CI}>,$<BOOL:$ENV{GITHUB_ACTIONS}>>:ATKAUDIO_CI_BUILD=1>
        $<$<AND:$<CONFIG:RelWithDebInfo>,$<OR:$<BOOL:$ENV{CI}>,$<BOOL:$ENV{GITHUB_ACTIONS}>>>:JUCE_DISABLE_ASSERTIONS>
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
    # This target does not use C++ modules. Disabling MSVC's module scan avoids
    # spurious `*.module.json` warnings when multiple sources share a basename.
    atk_apply_msvc_juce_warning_overrides(${PROJECT_NAME})
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

atk_sanitize_windows_install_prefix()

target_compile_definitions(${PROJECT_NAME} PRIVATE JUCE_ASIO=1)

file(COPY_FILE ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/INSTALLER-LICENSE)

# Keep the scanner as a separate executable / process boundary.
add_subdirectory(${CMAKE_SOURCE_DIR}/src/scanner ${CMAKE_BINARY_DIR}/scanner)
