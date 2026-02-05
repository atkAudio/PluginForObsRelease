# Plugin Scanner - Sandboxed executable for safe plugin scanning
cmake_minimum_required(VERSION 3.28)

if(NOT TARGET juce::juce_audio_processors)
    message(FATAL_ERROR "JUCE must be available. Build from parent CMakeLists.txt")
endif()

add_executable(${CMAKE_PROJECT_NAME}_scanner PluginScanner.cpp)

target_link_libraries(
    ${CMAKE_PROJECT_NAME}_scanner
    PRIVATE
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
)

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
        target_compile_options(
            ${CMAKE_PROJECT_NAME}_scanner
            PRIVATE
                /wd4244
                /wd4267
                /wd4390
                /wd5105
        )
    endif()
endif()

if(APPLE)
    set_target_properties(
        ${CMAKE_PROJECT_NAME}_scanner
        PROPERTIES
            MACOSX_BUNDLE FALSE
    )
    # Ensure scanner builds before main plugin
    add_dependencies(${CMAKE_PROJECT_NAME} ${CMAKE_PROJECT_NAME}_scanner)
    
    # Note: Scanner copy and bundle re-signing is handled in root CMakeLists.txt
    # because add_custom_command(TARGET) requires the target be in the same directory.
else()
    # Copy scanner next to plugin after build (for development/local testing)
    # Installation is handled by cpack.cmake for all platforms
    add_custom_command(
        TARGET ${CMAKE_PROJECT_NAME}_scanner
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${CMAKE_PROJECT_NAME}_scanner>
            $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>
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
