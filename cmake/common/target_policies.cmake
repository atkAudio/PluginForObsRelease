# Shared target policy helpers for plugin, scanner, and tests.

include_guard(GLOBAL)

function(atk_suppress_juce_target_warnings target_name)
    target_compile_options(
        ${target_name}
        PRIVATE
            $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-range-loop-bind-reference>
            $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-ambiguous-macro>
            $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-ambiguous-macro>
            $<$<COMPILE_LANG_AND_ID:OBJC,Clang,AppleClang>:-Wno-arc-repeated-use-of-weak>
            $<$<COMPILE_LANG_AND_ID:OBJCXX,Clang,AppleClang>:-Wno-arc-repeated-use-of-weak>
    )
endfunction()

function(atk_disable_msvc_modules_scan target_name)
    if(NOT MSVC)
        return()
    endif()

    set_property(
        TARGET
            ${target_name}
        PROPERTY
            CXX_SCAN_FOR_MODULES
                OFF
    )
endfunction()

function(atk_apply_msvc_juce_warning_overrides target_name)
    if(NOT MSVC)
        return()
    endif()

    atk_disable_msvc_modules_scan(${target_name})

    target_compile_options(
        ${target_name}
        PRIVATE
            /wd4244
            /wd4267
            /wd4390
            /wd5105
    )
endfunction()

function(atk_apply_juce_recommended_flags target_name)
    if(NOT TARGET ${target_name})
        return()
    endif()

    if(TARGET juce::juce_recommended_config_flags)
        target_link_libraries(${target_name} PRIVATE juce::juce_recommended_config_flags)
    endif()

    # LTO is intentionally CI-only to keep local iteration faster.
    if(TARGET juce::juce_recommended_lto_flags)
        target_link_libraries(
            ${target_name}
            PRIVATE
                $<$<OR:$<BOOL:$ENV{CI}>,$<BOOL:$ENV{GITHUB_ACTIONS}>>:juce::juce_recommended_lto_flags>
        )
    endif()
endfunction()

function(atk_sanitize_windows_install_prefix)
    if(NOT WIN32)
        return()
    endif()

    file(
        TO_CMAKE_PATH
        "${CMAKE_INSTALL_PREFIX}"
        _sanitized_prefix
    )
    string(
        REGEX REPLACE "/$"
        ""
        _sanitized_prefix
        "${_sanitized_prefix}"
    )
    set(CMAKE_INSTALL_PREFIX "${_sanitized_prefix}" CACHE PATH "Sanitized install prefix" FORCE)
endfunction()

function(atk_apply_packaging_build_flags target_name)
    if(NOT TARGET ${target_name})
        return()
    endif()

    target_compile_definitions(
        ${target_name}
        PRIVATE
            $<$<OR:$<BOOL:$ENV{CI}>,$<BOOL:$ENV{GITHUB_ACTIONS}>>:ATK_CI_BUILD>
            $<$<AND:$<NOT:$<OR:$<BOOL:$ENV{CI}>,$<BOOL:$ENV{GITHUB_ACTIONS}>>>,$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>>:ATK_DEBUG>
    )
endfunction()
