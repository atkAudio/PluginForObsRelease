# Called from a post-build command to copy Qt6 debug DLLs for local Debug runs.
# Variables expected:
#   CONFIG   - build configuration (Debug/Release/RelWithDebInfo/...)
#   SRC_DIR  - source directory containing Qt6 DLLs
#   DST_DIR  - target output directory for copied DLLs

if(NOT CONFIG STREQUAL "Debug")
    return()
endif()

file(GLOB _qt6_debug_dlls "${SRC_DIR}/Qt6*d.dll")

if(NOT _qt6_debug_dlls)
    message(WARNING "No Qt6 debug DLLs found under: ${SRC_DIR}")
    return()
endif()

foreach(_src IN LISTS _qt6_debug_dlls)
    get_filename_component(_dll "${_src}" NAME)
    set(_dst "${DST_DIR}/${_dll}")

    file(COPY_FILE "${_src}" "${_dst}" ONLY_IF_DIFFERENT)
    message(STATUS "Copied Qt6 debug DLL: ${_dll}")
endforeach()
