#!/bin/bash

# ==============================================================================
# Linux dependencies for atkAudio Plugin Host
# ==============================================================================
#
# This file defines the required apt packages for building the project.
# 
# Usage:
#   1. Direct installation: ./linux-dependencies.sh
#   2. Source in scripts: source linux-dependencies.sh
#      Then access via: "${DEPENDENCIES[@]}"
#   3. Used automatically by: cross-compile.sh
#
# ==============================================================================

# Add OBS Project PPA repository if not already added
if ! grep -q "obsproject/obs-studio" /etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null; then
    echo "Adding OBS Project PPA repository..."
    sudo add-apt-repository --yes ppa:obsproject/obs-studio
    sudo apt-get update
fi

# Array of dependencies (without architecture suffix)
# The cross-compile script will append :arch as needed
DEPENDENCIES=(
    # Audio libraries
    "libasound2-dev"
    "libjack-jackd2-dev"
    "ladspa-sdk"
    
    # Network libraries
    "libcurl4-openssl-dev"
    
    # Font libraries
    "libfreetype-dev"
    "libfontconfig1-dev"
    
    # X11 libraries
    "libx11-dev"
    "libxcomposite-dev"
    "libxcursor-dev"
    "libxext-dev"
    "libxinerama-dev"
    "libxrandr-dev"
    "libxrender-dev"
    
    # GTK and WebKit
    "libgtk-3-dev"
    "libwebkit2gtk-4.1-dev"
    
    # OpenGL/Mesa
    "libglu1-mesa-dev"
    "mesa-common-dev"
    "libgles2-mesa-dev"
    
    # OBS Studio development libraries
    # Note: obs-studio package may not provide CMake configs for cross-arch
    # We rely on FindLibObs.cmake to locate the library
    "libobs-dev"
    
    # Qt6 development packages
    "qt6-base-dev"
    "libqt6svg6-dev"
    "qt6-base-private-dev"
    
    # Assembler for optimized builds
    "nasm"
)

# Export the array so it can be sourced by other scripts
export DEPENDENCIES
