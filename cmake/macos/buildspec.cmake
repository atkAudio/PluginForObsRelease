# CMake macOS build dependencies module

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_macos: Set up macOS slice for _check_dependencies
function(_check_dependencies_macos)
  set(arch universal)
  set(platform macos)

  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)

  # Use source-directory-based dependency directory to persist across cache clears
  set(dependencies_dir "${CMAKE_SOURCE_DIR}/_deps")
  set(prebuilt_filename "macos-deps-VERSION-ARCH_REVISION.tar.xz")
  set(prebuilt_destination "obs-deps-VERSION-ARCH")
  set(qt6_filename "macos-deps-qt6-VERSION-ARCH-REVISION.tar.xz")
  set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
  set(obs-studio_filename "VERSION.tar.gz")
  set(obs-studio_destination "obs-studio-VERSION")
  set(dependencies_list prebuilt qt6 obs-studio)

  _check_dependencies()

  # Remove quarantine flags from downloaded dependencies (ignore errors on read-only git pack files)
  execute_process(
    COMMAND bash -c "find '${dependencies_dir}' -not -path '*/.git/*' -exec xattr -d com.apple.quarantine {} \\; 2>/dev/null || true"
  )

  list(APPEND CMAKE_FRAMEWORK_PATH "${dependencies_dir}/Frameworks")
  set(CMAKE_FRAMEWORK_PATH ${CMAKE_FRAMEWORK_PATH} PARENT_SCOPE)
endfunction()

_check_dependencies_macos()
