# CMake Windows build dependencies module

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_windows: Set up Windows slice for _check_dependencies
function(_check_dependencies_windows)
  set(arch ${CMAKE_VS_PLATFORM_NAME})
  set(platform windows-${arch})

  # Use source-directory-based dependency directory to persist across cache clears
  set(dependencies_dir "${CMAKE_SOURCE_DIR}/_deps/${arch}")
  set(prebuilt_filename "windows-deps-VERSION-ARCH-REVISION.zip")
  set(prebuilt_destination "obs-deps-VERSION-ARCH")
  set(qt6_filename "windows-deps-qt6-VERSION-ARCH-REVISION.zip")
  set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
  set(obs-studio_filename "VERSION.zip")
  set(obs-studio_destination "obs-studio-VERSION")
  set(dependencies_list prebuilt qt6 obs-studio)

  _check_dependencies()
  
  # For ARM64 cross-compilation, we also need x64 Qt host tools
  if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
    message(STATUS "ARM64 build detected - also downloading x64 Qt host tools for cross-compilation")
    
    # Call a separate function to download x64 Qt tools
    _download_x64_qt_for_arm64()
  endif()
endfunction()

# Helper function to download x64 Qt tools for ARM64 cross-compilation
function(_download_x64_qt_for_arm64)
  set(arch "x64")
  set(platform "windows-x64")
  # Store x64 tools in a persistent deps directory for cross-compilation
  set(dependencies_dir "${CMAKE_SOURCE_DIR}/_deps/x64")
  set(qt6_filename "windows-deps-qt6-VERSION-ARCH-REVISION.zip")
  set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
  set(dependencies_list qt6)
  
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)
  string(JSON dependency_data GET ${buildspec} dependencies)
  
  # Process only qt6 dependency for x64
  string(JSON data GET ${dependency_data} qt6)
  string(JSON version GET ${data} version)
  string(JSON hash GET ${data} hashes ${platform})
  string(JSON url GET ${data} baseUrl)
  string(JSON label GET ${data} label)
  string(JSON revision ERROR_VARIABLE error GET ${data} revision ${platform})

  message(STATUS "Setting up ${label} x64 host tools for ARM64 cross-compilation")

  set(file "${qt6_filename}")
  set(destination "${qt6_destination}")
  string(REPLACE "VERSION" "${version}" file "${file}")
  string(REPLACE "VERSION" "${version}" destination "${destination}")
  string(REPLACE "ARCH" "${arch}" file "${file}")
  string(REPLACE "ARCH" "${arch}" destination "${destination}")
  if(revision)
    string(REPLACE "_REVISION" "_v${revision}" file "${file}")
    string(REPLACE "-REVISION" "-v${revision}" file "${file}")
  else()
    string(REPLACE "_REVISION" "" file "${file}")
    string(REPLACE "-REVISION" "" file "${file}")
  endif()

  if(EXISTS "${dependencies_dir}/.dependency_qt6_${arch}.sha256")
    file(READ "${dependencies_dir}/.dependency_qt6_${arch}.sha256" OBS_DEPENDENCY_qt6_x64_HASH)
  endif()

  set(skip FALSE)
  if(OBS_DEPENDENCY_qt6_x64_HASH STREQUAL ${hash})
    set(skip TRUE)
    message(STATUS "Setting up ${label} x64 host tools - skipped")
  endif()

  if(NOT skip)
    set(url "${url}/${version}/${file}")
    set(destination "${dependencies_dir}/${destination}")
    
    message(STATUS "Downloading x64 Qt host tools: ${file}")
    file(DOWNLOAD "${url}" "${dependencies_dir}/${file}" EXPECTED_HASH SHA256=${hash} STATUS download_status)
    
    list(GET download_status 0 status_code)
    if(NOT status_code EQUAL 0)
      list(GET download_status 1 error_message)
      message(FATAL_ERROR "Failed to download x64 Qt host tools: ${error_message}")
    endif()
    
    message(STATUS "Extracting x64 Qt host tools to: ${destination}")
    file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${destination}")
    file(REMOVE "${dependencies_dir}/${file}")
    file(WRITE "${dependencies_dir}/.dependency_qt6_x64.sha256" "${hash}")
    
    # Verify extraction was successful
    if(EXISTS "${destination}/bin/moc.exe")
      message(STATUS "Setting up ${label} x64 host tools - done")
    else()
      message(FATAL_ERROR "Failed to extract x64 Qt host tools to ${destination} - moc.exe not found")
    endif()
  endif()
  
  message(STATUS "ARM64 cross-compilation ready: x64 host tools + ARM64 target libraries available")
endfunction()

_check_dependencies_windows()
