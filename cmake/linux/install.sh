#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
DATA_HOME="${XDG_DATA_HOME:-${HOME:?HOME must be set}/.local/share}"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
if [[ "$DATA_HOME" != /* || "$CONFIG_HOME" != /* ]]; then
    echo "Error: XDG_DATA_HOME and XDG_CONFIG_HOME must be absolute paths." >&2
    exit 1
fi

PLUGIN_NAME="atkaudio-pluginforobs"
PAYLOAD="$SCRIPT_DIR/plugins/$PLUGIN_NAME"
MODERN_DIR="$DATA_HOME/obs-studio/plugins/$PLUGIN_NAME"
LEGACY_DIR="$CONFIG_HOME/obs-studio/plugins/$PLUGIN_NAME"

for required in "$PLUGIN_NAME.so" "${PLUGIN_NAME}_scanner" "data/manifest.json" "data/locale/en-US.ini"; do
    if [[ ! -f "$PAYLOAD/$required" ]]; then
        echo "Error: Required payload file missing: $PAYLOAD/$required" >&2
        exit 1
    fi
done

for install_root in "$MODERN_DIR" "$LEGACY_DIR"; do
    if [[ -L "$install_root" ]]; then
        echo "Error: Refusing a symlinked plugin directory: $install_root" >&2
        exit 1
    fi
    resolved_root="$(realpath -m "$install_root")"
    if [[ "$SCRIPT_DIR" == "$resolved_root" || "$SCRIPT_DIR" == "$resolved_root/"* ||
            "$resolved_root" == "$SCRIPT_DIR/"* ]]; then
        echo "Error: Installation directory overlaps the extracted archive: $install_root" >&2
        exit 1
    fi
done
MODERN_DIR="$(realpath -m "$MODERN_DIR")"
LEGACY_DIR="$(realpath -m "$LEGACY_DIR")"

Confirm() {
    local reply default="${2:-yes}" prompt_suffix="[y/N]"
    if [[ "$default" == yes ]]; then
        prompt_suffix="[Y/n]"
    fi
    while true; do
        printf '%s %s ' "$1" "$prompt_suffix"
        if ! read -r reply; then
            printf '\n'
            echo "Operation cancelled."
            exit 0
        fi
        printf '\n'
        case "$reply" in
            [Yy]|[Yy][Ee][Ss]) return 0 ;;
            '' ) [[ "$default" == yes ]] && return 0 || return 1 ;;
            [Nn]|[Nn][Oo]) return 1 ;;
            *) echo "Please answer yes or no." ;;
        esac
    done
}

echo "atkAudio Plugin - native OBS Studio installer"
echo "Close OBS before installing or removing the plugin."

DETECTED_DIRS=()
if [[ -f "$MODERN_DIR/$PLUGIN_NAME.so" ]]; then
    DETECTED_DIRS+=("$MODERN_DIR")
fi
if [[ -f "$LEGACY_DIR/bin/64bit/$PLUGIN_NAME.so" ]] &&
    [[ "$LEGACY_DIR" != "$MODERN_DIR" || ${#DETECTED_DIRS[@]} -eq 0 ]]; then
    DETECTED_DIRS+=("$LEGACY_DIR")
fi
for old_dir in "${DETECTED_DIRS[@]}"; do
    echo "Existing installation found: $old_dir"
    echo "Uninstalling removes this plugin directory and its data, not separate OBS settings."
    if Confirm "Uninstall the existing installation at $old_dir?" yes; then
        rm -rf -- "$old_dir"
        echo "Uninstalled: $old_dir"
    fi
done
if ! Confirm "Continue with installation?"; then
    echo "Installation cancelled."
    exit 0
fi

INSTALL_DIR="$MODERN_DIR"
BINARY_DIR="$INSTALL_DIR"
if ! Confirm "Install for OBS 33 or later?" yes; then
    INSTALL_DIR="$LEGACY_DIR"
    BINARY_DIR="$INSTALL_DIR/bin/64bit"
fi

for destination in "$BINARY_DIR" "$INSTALL_DIR/bin" "$INSTALL_DIR/data" "$INSTALL_DIR/data/locale"; do
    if [[ -L "$destination" ]]; then
        echo "Error: Refusing a symlinked destination: $destination" >&2
        exit 1
    fi
done

echo "This will install the plugin to: $INSTALL_DIR"
echo "Existing packaged files at this destination will be overwritten."
for old_dir in "${DETECTED_DIRS[@]}"; do
    if [[ -e "$old_dir" && "$old_dir" != "$INSTALL_DIR" ]]; then
        echo "Warning: Keeping the other installation may cause duplicate plugin loading."
    fi
done
if ! Confirm "Install to this location?" yes; then
    echo "Installation cancelled."
    exit 0
fi

mkdir -p "$BINARY_DIR" "$INSTALL_DIR/data"
cp --remove-destination "$PAYLOAD/$PLUGIN_NAME.so" "$BINARY_DIR/"
cp --remove-destination "$PAYLOAD/${PLUGIN_NAME}_scanner" "$BINARY_DIR/"
chmod +x "$BINARY_DIR/${PLUGIN_NAME}_scanner"
cp -R "$PAYLOAD/data/." "$INSTALL_DIR/data/"

echo "Installation complete: $INSTALL_DIR"
echo "Restart OBS to load the plugin."
