# CMake toolchain file for cross-compiling to arm64
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Specify the cross compiler
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Add custom CMake module path for finding packages
list(APPEND CMAKE_MODULE_PATH "/workspace/cmake/linux")

# Use system's multiarch directories - no separate sysroot needed
# The cross-compiler toolchain already knows about:
#   /usr/lib/aarch64-linux-gnu/
#   /usr/include/aarch64-linux-gnu/
# Ubuntu's multiarch support handles everything

# Set CMAKE_LIBRARY_ARCHITECTURE for multiarch support
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# Set root paths for finding libraries - exclude Linuxbrew/Homebrew
set(CMAKE_FIND_ROOT_PATH 
    /usr
    /usr/lib/aarch64-linux-gnu
    /usr/include/aarch64-linux-gnu
)

# Add prefix paths for finding CMake package config files
set(CMAKE_PREFIX_PATH
    /usr
    /usr/lib/aarch64-linux-gnu
    /usr/lib/aarch64-linux-gnu/cmake
    /usr/share
)

# Exclude Linuxbrew/Homebrew paths to prevent architecture mismatches
set(CMAKE_IGNORE_PATH
    /home/linuxbrew/.linuxbrew
    /opt/homebrew
    /usr/local/Homebrew
)

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers in the target directories only
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Don't build helper tools when cross-compiling
set(JUCE_BUILD_HELPER_TOOLS OFF CACHE BOOL "Don't build helper tools" FORCE)

# Point to pre-built juceaide from host build
if(EXISTS "/workspace/build-host-tools/JUCE/tools/juceaide")
    set(JUCE_JUCEAIDE_PATH "/workspace/build-host-tools/JUCE/tools/juceaide" CACHE FILEPATH "Path to juceaide" FORCE)
    message(STATUS "Using pre-built juceaide: /workspace/build-host-tools/JUCE/tools/juceaide")
endif()

# pkg-config configuration for cross-compilation
set(PKG_CONFIG_EXECUTABLE /usr/bin/aarch64-linux-gnu-pkg-config)
if(NOT EXISTS "${PKG_CONFIG_EXECUTABLE}")
    # Fallback to regular pkg-config with environment variables
    set(PKG_CONFIG_EXECUTABLE /usr/bin/pkg-config)
    set(ENV{PKG_CONFIG_PATH} "")
    set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")
endif()
