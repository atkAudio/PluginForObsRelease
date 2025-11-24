#!/bin/bash
# Portable Linux installer for atkAudio Plugin
# Installs to user's .config/obs-studio directory

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$HOME/.config/obs-studio/plugins/atkaudio-pluginforobs"

echo "================================================================================"
echo "atkAudio Plugin - Portable Installer"
echo "================================================================================"
echo ""
echo "This will install the plugin to your user directory:"
echo "  $INSTALL_DIR"
echo ""
read -p "Do you wish to continue? [y/N] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Installation cancelled."
    exit 0
fi

echo ""
echo "Installing atkAudio Plugin..."

# Create installation directory
mkdir -p "$INSTALL_DIR/bin/64bit"

# Install plugin binary
if [ -f "$SCRIPT_DIR/obs-plugins/64bit/atkaudio-pluginforobs.so" ]; then
    cp "$SCRIPT_DIR/obs-plugins/64bit/atkaudio-pluginforobs.so" "$INSTALL_DIR/bin/64bit/"
    echo "✓ Installed plugin binary"
else
    echo "✗ Error: Plugin binary not found in $SCRIPT_DIR/obs-plugins/64bit/"
    exit 1
fi

echo ""
echo "================================================================================"
echo "Installation complete!"
echo "================================================================================"
echo "Plugin installed to: $INSTALL_DIR"
echo ""
echo "To uninstall, run:"
echo "  rm -rf $INSTALL_DIR"
echo ""

