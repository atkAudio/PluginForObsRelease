# Shared platform utility helpers.

include_guard(GLOBAL)

function(atk_normalize_windows_arch raw_arch out_var)
    string(TOUPPER "${raw_arch}" _arch_upper)

    if(_arch_upper MATCHES "AMD64|X64|X86_64|WIN32")
        set(_arch_upper "X64")
    elseif(_arch_upper MATCHES "ARM64|AARCH64")
        set(_arch_upper "ARM64")
    endif()

    set(${out_var} "${_arch_upper}" PARENT_SCOPE)
endfunction()

function(atk_get_windows_target_arch out_var)
    if(CMAKE_GENERATOR_PLATFORM)
        set(_raw_target_arch "${CMAKE_GENERATOR_PLATFORM}")
    elseif(CMAKE_VS_PLATFORM_NAME)
        set(_raw_target_arch "${CMAKE_VS_PLATFORM_NAME}")
    else()
        set(_raw_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    atk_normalize_windows_arch("${_raw_target_arch}" _normalized_target_arch)
    set(${out_var} "${_normalized_target_arch}" PARENT_SCOPE)
endfunction()

function(atk_get_windows_host_arch out_var)
    atk_normalize_windows_arch("${CMAKE_HOST_SYSTEM_PROCESSOR}" _normalized_host_arch)
    set(${out_var} "${_normalized_host_arch}" PARENT_SCOPE)
endfunction()

function(atk_get_obs_arch_dir out_var)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_obs_arch_dir "64bit")
    else()
        set(_obs_arch_dir "32bit")
    endif()

    set(${out_var} "${_obs_arch_dir}" PARENT_SCOPE)
endfunction()
