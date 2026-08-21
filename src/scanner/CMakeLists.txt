# Plugin Scanner - Sandboxed executable for safe plugin scanning
if(NOT TARGET juce::juce_audio_processors)
    message(FATAL_ERROR "JUCE must be available. Build from parent CMakeLists.txt")
endif()

add_executable(${CMAKE_PROJECT_NAME}_scanner PluginScanner.cpp)
atk_apply_packaging_build_flags(${CMAKE_PROJECT_NAME}_scanner)
atk_apply_juce_recommended_flags(${CMAKE_PROJECT_NAME}_scanner)

target_link_libraries(${CMAKE_PROJECT_NAME}_scanner PRIVATE juce::juce_audio_utils)

target_compile_definitions(
    ${CMAKE_PROJECT_NAME}_scanner
    PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
)

atk_suppress_juce_target_warnings(${CMAKE_PROJECT_NAME}_scanner)

if(UNIX AND NOT APPLE)
    target_compile_definitions(
        ${CMAKE_PROJECT_NAME}_scanner
        PRIVATE
            JUCE_PLUGINHOST_LV2=1
            JUCE_PLUGINHOST_LADSPA=1
    )
    target_link_libraries(${CMAKE_PROJECT_NAME}_scanner PRIVATE pthread)
endif()

if(WIN32)
    set_target_properties(
        ${CMAKE_PROJECT_NAME}_scanner
        PROPERTIES
            WIN32_EXECUTABLE
                FALSE
    )
    if(MSVC)
        atk_apply_msvc_juce_warning_overrides(${CMAKE_PROJECT_NAME}_scanner)
    endif()
endif()

# The plugin install/package step expects the scanner binary to exist.
# Build it before the main plugin on every platform.
add_dependencies(${CMAKE_PROJECT_NAME} ${CMAKE_PROJECT_NAME}_scanner)

if(APPLE)
    set_target_properties(
        ${CMAKE_PROJECT_NAME}_scanner
        PROPERTIES
            MACOSX_BUNDLE
                FALSE
    )

    # Note: Scanner copy and bundle re-signing is handled in cmake/common/plugin-cpack.cmake
else()
    # Copy scanner next to plugin after build (for development/local testing)
    # Installation is handled by cpack.cmake for all platforms
    add_custom_command(
        TARGET ${CMAKE_PROJECT_NAME}_scanner
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${CMAKE_PROJECT_NAME}_scanner>
            $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/$<TARGET_FILE_NAME:${CMAKE_PROJECT_NAME}_scanner>
        COMMENT "Copying scanner to plugin directory"
    )
endif()

# Strip debug symbols on Linux (scanner binary can be very large with JUCE)
if(UNIX AND NOT APPLE)
    if(NOT CMAKE_STRIP)
        find_program(CMAKE_STRIP strip)
    endif()

    if(CMAKE_STRIP)
        add_custom_command(
            TARGET ${CMAKE_PROJECT_NAME}_scanner
            POST_BUILD
            COMMAND
                ${CMAKE_STRIP} --strip-debug $<TARGET_FILE:${CMAKE_PROJECT_NAME}_scanner>
            COMMENT "Stripping debug symbols from scanner"
        )
    endif()
endif()
