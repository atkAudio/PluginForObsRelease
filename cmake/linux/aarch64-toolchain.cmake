set(CMAKE_SYSTEM_NAME Linux)
# "arm64" (not aarch64) keeps package file names stable.
set(CMAKE_SYSTEM_PROCESSOR arm64)

set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)
set(CMAKE_AR /usr/bin/aarch64-linux-gnu-ar)
set(CMAKE_RANLIB /usr/bin/aarch64-linux-gnu-ranlib)
set(CMAKE_OBJCOPY /usr/bin/aarch64-linux-gnu-objcopy)
set(CMAKE_STRIP /usr/bin/aarch64-linux-gnu-strip)

set(CMAKE_FIND_ROOT_PATH
    /usr/aarch64-linux-gnu
    /
)
set(CMAKE_LIBRARY_PATH /usr/lib/aarch64-linux-gnu)
set(CMAKE_INCLUDE_PATH /usr/include/aarch64-linux-gnu)
set(CMAKE_PREFIX_PATH /usr/lib/aarch64-linux-gnu/cmake)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
