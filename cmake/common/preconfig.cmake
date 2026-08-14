# Prefer system pkg-config over Linuxbrew on Linux (native builds only)
# Only applies when Linuxbrew is installed and not cross-compiling
if(UNIX AND NOT APPLE AND NOT CMAKE_CROSSCOMPILING)
    if(EXISTS "$ENV{HOME}/.linuxbrew" OR EXISTS "/home/linuxbrew/.linuxbrew")
        if(EXISTS "/usr/bin/pkg-config")
            # Force CMake to use system pkg-config executable
            set(PKG_CONFIG_EXECUTABLE "/usr/bin/pkg-config" CACHE FILEPATH "pkg-config executable" FORCE)
            # Override PKG_CONFIG_PATH to prioritize system libraries
            set(ENV{PKG_CONFIG_PATH}
                "/usr/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:/usr/local/lib/pkgconfig"
            )
            message(STATUS "Using system pkg-config to avoid Linuxbrew library conflicts")
        endif()
    endif()
endif()
