include_guard(GLOBAL)

execute_process(
    COMMAND
        xcrun --sdk macosx --show-sdk-version
    OUTPUT_VARIABLE macos_sdk_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE xcrun_result
)
string(
    REGEX MATCH "^([0-9]+)"
    macos_sdk_major
    "${macos_sdk_version}"
)
if(NOT xcrun_result EQUAL 0 OR macos_sdk_major STREQUAL "")
    message(FATAL_ERROR "Unable to determine the macOS SDK version")
endif()
execute_process(
    COMMAND
        xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE macos_sdk_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE sdk_path_result
)
execute_process(
    COMMAND
        /usr/libexec/PlistBuddy -c "Print :SupportedTargets:macosx:MinimumDeploymentTarget"
        "${macos_sdk_path}/SDKSettings.plist"
    OUTPUT_VARIABLE macos_xcode_minimum_deployment_target
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE plist_result
)
string(
    REGEX MATCH "^([0-9]+)"
    macos_deployment_major
    "${macos_xcode_minimum_deployment_target}"
)
if(NOT sdk_path_result EQUAL 0 OR NOT plist_result EQUAL 0 OR macos_deployment_major STREQUAL "")
    message(FATAL_ERROR "Unable to determine the Xcode deployment target range")
endif()
while(macos_deployment_major LESS_EQUAL macos_sdk_major)
    execute_process(
        COMMAND
            xcrun --sdk macosx clang++ -target "arm64-apple-macos${macos_deployment_major}" -x c++ -c /dev/null -o
            /dev/null
        RESULT_VARIABLE compiler_probe_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(compiler_probe_result EQUAL 0)
        break()
    endif()
    math(EXPR macos_deployment_major "${macos_deployment_major} + 1")
endwhile()
if(macos_deployment_major GREATER macos_sdk_major)
    message(FATAL_ERROR "Unable to find a valid macOS deployment target")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "${macos_deployment_major}.0" CACHE STRING "Minimum macOS version" FORCE)
message(STATUS "Detected macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")

cmake_host_system_information(RESULT macos_architectures QUERY OS_PLATFORM)
if(DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
    set(macos_architecture_probe "${CMAKE_BINARY_DIR}/CMakeFiles/macos-architecture-probe")
    file(MAKE_DIRECTORY "${macos_architecture_probe}")
    file(
        WRITE "${macos_architecture_probe}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.28)\nproject(SdkArchitectures LANGUAGES NONE)\nadd_custom_target(sdk_architectures)\n"
    )
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -S "${macos_architecture_probe}" -B "${macos_architecture_probe}/build" -G Xcode
            "-DCMAKE_OSX_SYSROOT=${macos_sdk_path}" "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        OUTPUT_QUIET
        COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
        COMMAND
            xcrun xcodebuild -showBuildSettings -json -project
            "${macos_architecture_probe}/build/SdkArchitectures.xcodeproj" -target sdk_architectures -sdk
            "${macos_sdk_path}"
        OUTPUT_VARIABLE macos_build_settings
        ERROR_VARIABLE architecture_settings_error
        RESULT_VARIABLE architecture_settings_result
    )
    if(NOT architecture_settings_result EQUAL 0)
        message(FATAL_ERROR "Unable to determine Xcode standard architectures: ${architecture_settings_error}")
    endif()
    string(
        JSON macos_standard_architectures
        GET "${macos_build_settings}"
        0
        buildSettings
        ARCHS_STANDARD
    )
    execute_process(
        COMMAND
            plutil -extract SupportedTargets.macosx.Archs json -o - "${macos_sdk_path}/SDKSettings.plist"
        OUTPUT_VARIABLE macos_sdk_architectures
        COMMAND_ERROR_IS_FATAL ANY
    )
    string(JSON macos_sdk_architecture_count LENGTH "${macos_sdk_architectures}")
    set(macos_architectures "")
    separate_arguments(macos_standard_architectures UNIX_COMMAND "${macos_standard_architectures}")
    foreach(macos_standard_architecture IN LISTS macos_standard_architectures)
        set(macos_sdk_architecture_index 0)
        while(macos_sdk_architecture_index LESS macos_sdk_architecture_count)
            string(JSON macos_sdk_architecture GET "${macos_sdk_architectures}" ${macos_sdk_architecture_index})
            if(macos_standard_architecture STREQUAL macos_sdk_architecture)
                list(APPEND macos_architectures "${macos_standard_architecture}")
                break()
            endif()
            math(EXPR macos_sdk_architecture_index "${macos_sdk_architecture_index} + 1")
        endwhile()
    endforeach()
    set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)
endif()
if(NOT macos_architectures)
    message(FATAL_ERROR "Unable to determine the macOS build architecture")
endif()
set(CMAKE_OSX_ARCHITECTURES "${macos_architectures}" CACHE STRING "Detected macOS build architectures" FORCE)
message(STATUS "Detected macOS build architectures: ${CMAKE_OSX_ARCHITECTURES}")
