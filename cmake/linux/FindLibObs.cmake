# FindLibObs.cmake - Find OBS Studio libraries
# This module finds the OBS Studio libraries, particularly useful for cross-compilation
# where CMake config files may not be available in multiarch directories.
#
# This module defines:
#  LibObs_FOUND - System has OBS Studio libraries
#  LibObs_INCLUDE_DIRS - The OBS Studio include directories
#  LibObs_LIBRARIES - The libraries needed to use OBS Studio
#  LibObs_DEFINITIONS - Compiler switches required for using OBS Studio
#
# And creates the following imported targets:
#  libobs - The main OBS library

include(FindPackageHandleStandardArgs)

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBOBS QUIET libobs)
  set(LibObs_DEFINITIONS ${PC_LIBOBS_CFLAGS_OTHER})
endif()

# Find the include directory
find_path(LibObs_INCLUDE_DIR
  NAMES obs.h obs-module.h
  PATHS
    ${PC_LIBOBS_INCLUDE_DIRS}
    ${LIBOBS_INCLUDE_PATH}
    /usr/include
    /usr/local/include
    ${CMAKE_SYSROOT}/usr/include
  PATH_SUFFIXES obs libobs
)

# Find the library
find_library(LibObs_LIBRARY
  NAMES obs libobs
  PATHS
    ${PC_LIBOBS_LIBRARY_DIRS}
    ${LIBOBS_LIB_PATH}
    /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
    /usr/lib
    /usr/local/lib
    ${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
    ${CMAKE_SYSROOT}/usr/lib
)

# Handle the QUIETLY and REQUIRED arguments
find_package_handle_standard_args(LibObs
  REQUIRED_VARS LibObs_LIBRARY LibObs_INCLUDE_DIR
  VERSION_VAR PC_LIBOBS_VERSION
)

if(LibObs_FOUND)
  set(LibObs_LIBRARIES ${LibObs_LIBRARY})
  set(LibObs_INCLUDE_DIRS ${LibObs_INCLUDE_DIR})
  
  # Create imported target
  if(NOT TARGET libobs)
    add_library(libobs UNKNOWN IMPORTED)
    set_target_properties(libobs PROPERTIES
      IMPORTED_LOCATION "${LibObs_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LibObs_INCLUDE_DIR}"
      INTERFACE_COMPILE_OPTIONS "${LibObs_DEFINITIONS}"
    )
  endif()
  
  # Also create OBS::libobs alias for compatibility
  if(NOT TARGET OBS::libobs)
    add_library(OBS::libobs ALIAS libobs)
  endif()
  
  mark_as_advanced(LibObs_INCLUDE_DIR LibObs_LIBRARY)
endif()
