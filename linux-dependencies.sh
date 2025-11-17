# Linux build dependencies for OBS Plugin
# This file is used by both container builds and native builds

# Build tools (architecture-independent)
BUILD_TOOLS=(
  build-essential
  jq
  cmake
  pkg-config
  ninja-build
  file
)

# JUCE dependencies (from official JUCE Linux Dependencies.md)
# These need architecture suffix for cross-compilation
JUCE_DEPS=(
  libasound2-dev
  libjack-jackd2-dev
  ladspa-sdk
  libcurl4-openssl-dev
  libfreetype6-dev
  libfontconfig1-dev
  libx11-dev
  libxcomposite-dev
  libxcursor-dev
  libxext-dev
  libxinerama-dev
  libxrandr-dev
  libxrender-dev
  libwebkit2gtk-4.1-dev
  libglu1-mesa-dev
  mesa-common-dev
)

# OBS dependencies
OBS_DEPS=(
  libobs-dev
)

# Qt6 dependencies
QT6_DEPS=(
  qt6-base-dev
  libqt6svg6-dev
  qt6-base-private-dev
)

# Qt6 tools (x86_64 only, for cross-compilation)
QT6_TOOLS=(
  qt6-base-dev-tools
)
