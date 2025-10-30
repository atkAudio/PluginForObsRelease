#!/bin/bash
# Exit on error, but we'll handle specific errors manually
set -e

# ==============================================================================
# Cross-compilation script for Ubuntu
# ==============================================================================
#
# Usage: 
#   ./cross-compile.sh <architecture>       - Cross-compile for architecture
#   ./cross-compile.sh clean <architecture> - Remove cross-arch packages
#
# Supported architectures: arm64, armhf, i386
#
# Examples:
#   ./cross-compile.sh arm64        - Cross-compile for ARM64
#   ./cross-compile.sh clean arm64  - Remove all ARM64 packages and cleanup
#
# ==============================================================================

# Speed up apt operations by disabling unnecessary triggers
export DEBIAN_FRONTEND=noninteractive
export DEBCONF_NONINTERACTIVE_SEEN=true

# Disable man-db triggers to speed up package installation
if [ ! -f /etc/dpkg/dpkg.cfg.d/01_nodoc ]; then
    echo "path-exclude /usr/share/man/*" | sudo tee /etc/dpkg/dpkg.cfg.d/01_nodoc > /dev/null
    echo "path-exclude /usr/share/doc/*" | sudo tee -a /etc/dpkg/dpkg.cfg.d/01_nodoc > /dev/null
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Temporarily disable Linuxbrew/Homebrew to prevent interference
disable_brew() {
    if [ -d "/home/linuxbrew/.linuxbrew" ] || [ -d "/opt/homebrew" ] || [ -d "/usr/local/Homebrew" ]; then
        log_info "Temporarily disabling Linuxbrew/Homebrew for clean cross-compilation..."
        
        # Save original PATH
        export ORIGINAL_PATH="$PATH"
        
        # Remove brew paths from PATH
        export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v -E 'linuxbrew|homebrew|Homebrew' | tr '\n' ':' | sed 's/:$//')
        
        # Clear pkg-config paths that might point to brew
        export ORIGINAL_PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"
        export PKG_CONFIG_PATH=""
        
        log_info "Brew paths removed from environment"
    fi
}

# Re-enable Linuxbrew/Homebrew after cross-compilation
enable_brew() {
    if [ -n "${ORIGINAL_PATH:-}" ]; then
        log_info "Re-enabling Linuxbrew/Homebrew..."
        export PATH="$ORIGINAL_PATH"
        export PKG_CONFIG_PATH="${ORIGINAL_PKG_CONFIG_PATH:-}"
        unset ORIGINAL_PATH
        unset ORIGINAL_PKG_CONFIG_PATH
        log_info "Brew paths restored"
    fi
}

# Check if running as root for apt operations
check_sudo() {
    if ! sudo -n true 2>/dev/null; then
        log_warn "This script requires sudo access for installing packages"
        sudo -v || {
            log_error "Failed to obtain sudo access"
            exit 1
        }
    fi
}

# Function to check and fix broken packages
fix_broken_packages() {
    log_info "Checking for broken packages..."
    if ! dpkg -l | grep -q "^iU\|^iF"; then
        log_info "No broken packages found"
        return 0
    fi
    
    log_warn "Broken packages detected, attempting to fix..."
    sudo apt-get install -f -y || {
        log_warn "apt-get install -f failed, trying to remove problematic packages..."
        # Try to identify and remove problematic ARM packages
        dpkg -l | grep ":${DEBIAN_ARCH}" | grep "^iU\|^iF" | awk '{print $2}' | while read pkg; do
            sudo dpkg --remove --force-all "$pkg" 2>/dev/null || true
        done
        sudo apt-get install -f -y || log_warn "Still have issues, continuing anyway..."
    }
}

# Function to clean up cross-architecture packages and restore system
cleanup_cross_arch() {
    local ARCH="$1"
    local FULL_CLEANUP="${2:-false}"  # Optional parameter: true for full cleanup
    
    log_info "========================================"
    log_info "Cleaning up cross-architecture: $ARCH"
    log_info "========================================"
    
    # Clean up build artifacts (always done)
    local BUILD_DIR="$PROJECT_ROOT/build-${ARCH}"
    local SYSROOT_DIR="$PROJECT_ROOT/sysroot-${ARCH}"
    local TOOLCHAIN_FILE="$PROJECT_ROOT/cmake-toolchain-${ARCH}.cmake"
    local HOST_BUILD_DIR="$PROJECT_ROOT/build-host-tools"
    
    if [ -d "$BUILD_DIR" ]; then
        log_info "Removing build directory: $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi
    
    if [ -d "$SYSROOT_DIR" ]; then
        log_info "Removing sysroot directory: $SYSROOT_DIR"
        rm -rf "$SYSROOT_DIR"
    fi
    
    if [ -f "$TOOLCHAIN_FILE" ]; then
        log_info "Removing toolchain file: $TOOLCHAIN_FILE"
        rm -f "$TOOLCHAIN_FILE"
    fi
    
    if [ -d "$HOST_BUILD_DIR" ]; then
        log_info "Removing host tools directory: $HOST_BUILD_DIR"
        rm -rf "$HOST_BUILD_DIR"
    fi
    
    # Full cleanup (packages and system configuration) - optional
    if [ "$FULL_CLEANUP" = "true" ]; then
        log_info ""
        log_info "Performing FULL cleanup (removing packages and architecture)..."
        log_info ""
        
        check_sudo
        
        log_info "Removing all $ARCH packages..."
        
        # Get list of all installed packages for this architecture
        local packages=$(dpkg -l | grep ":${ARCH}" | awk '{print $2}')
        
        if [ -z "$packages" ]; then
            log_info "No $ARCH packages found"
        else
            log_info "Found $(echo "$packages" | wc -l) $ARCH packages to remove"
            
            # Remove packages
            for pkg in $packages; do
                log_info "Removing $pkg..."
                sudo apt-get remove --purge -y "$pkg" 2>/dev/null || sudo dpkg --remove --force-all "$pkg" 2>/dev/null || true
            done
            
            # Autoremove orphaned packages
            log_info "Removing orphaned packages..."
            sudo apt-get autoremove -y
        fi
        
        # Remove foreign architecture
        if dpkg --print-foreign-architectures | grep -q "^${ARCH}$"; then
            log_info "Removing foreign architecture: $ARCH"
            sudo dpkg --remove-architecture "$ARCH" || log_warn "Could not remove architecture (packages may still be installed)"
        fi
        
        # Restore sources files if backups exist
        if [ -f "/etc/apt/sources.list.d/ubuntu.sources.bak" ]; then
            log_info "Restoring ubuntu.sources from backup..."
            sudo mv /etc/apt/sources.list.d/ubuntu.sources.bak /etc/apt/sources.list.d/ubuntu.sources
        fi
        
        if [ -f "/etc/apt/sources.list.backup-cross-compile" ]; then
            log_info "Restoring sources.list from backup..."
            sudo mv /etc/apt/sources.list.backup-cross-compile /etc/apt/sources.list
        fi
        
        # Remove ports repository
        if [ -f "/etc/apt/sources.list.d/ubuntu-ports.list" ]; then
            log_info "Removing Ubuntu Ports repository..."
            sudo rm -f /etc/apt/sources.list.d/ubuntu-ports.list
        fi
        
        # Unhold Python packages
        log_info "Removing holds on Python packages..."
        sudo apt-mark unhold python3:$ARCH python3-minimal:$ARCH python3.12:$ARCH python3.12-minimal:$ARCH 2>/dev/null || true
        
        # Update package lists
        log_info "Updating package lists..."
        sudo apt-get update
        
        log_info "========================================"
        log_info "FULL cleanup complete!"
        log_info "========================================"
        log_info "Note: Cross-compilation toolchains (gcc/g++) for $ARCH were NOT removed."
        log_info "To remove them manually, run:"
        log_info "  sudo apt-get remove --purge crossbuild-essential-${ARCH}"
        log_info "  sudo apt-get autoremove"
    else
        log_info "========================================"
        log_info "Build cleanup complete!"
        log_info "========================================"
        log_info ""
        log_info "Installed packages and architecture configuration were NOT removed."
        log_info "To perform a full cleanup (remove packages and architecture), run:"
        log_info "  $0 clean $ARCH --full"
    fi
}

# Check for special commands
if [ "$1" = "clean" ] || [ "$1" = "cleanup" ]; then
    if [ -z "$2" ]; then
        log_error "No architecture specified for cleanup"
        echo "Usage: $0 clean <architecture> [--full]"
        echo "Supported architectures: arm64, armhf, i386"
        echo ""
        echo "Options:"
        echo "  --full    Also remove installed packages and architecture (requires sudo)"
        echo ""
        echo "Without --full, only build artifacts are removed."
        exit 1
    fi
    
    # Check for --full flag
    FULL_CLEANUP="false"
    if [ "$3" = "--full" ] || [ "$3" = "-f" ]; then
        FULL_CLEANUP="true"
    fi
    
    # Validate and normalize architecture
    case "$2" in
        arm64|aarch64)
            DEBIAN_ARCH="arm64"
            ;;
        armhf|armv7)
            DEBIAN_ARCH="armhf"
            ;;
        i386|x86)
            DEBIAN_ARCH="i386"
            ;;
        *)
            log_error "Unsupported architecture: $2"
            echo "Supported architectures: arm64, armhf, i386"
            exit 1
            ;;
    esac
    
    cleanup_cross_arch "$DEBIAN_ARCH" "$FULL_CLEANUP"
    exit 0
fi

# Check if architecture is provided
if [ -z "$1" ]; then
    log_error "No architecture specified"
    echo "Usage: $0 <architecture>           - Cross-compile for architecture"
    echo "       $0 clean <architecture>      - Remove build artifacts only"
    echo "       $0 clean <architecture> --full - Remove build artifacts AND packages"
    echo ""
    echo "Supported architectures: arm64, armhf, i386"
    exit 1
fi

TARGET_ARCH="$1"

# Validate architecture
case "$TARGET_ARCH" in
    arm64|aarch64)
        TARGET_ARCH="arm64"
        DEBIAN_ARCH="arm64"
        CMAKE_SYSTEM_PROCESSOR="aarch64"
        CROSS_COMPILE_PREFIX="aarch64-linux-gnu"
        ;;
    armhf|armv7)
        TARGET_ARCH="armhf"
        DEBIAN_ARCH="armhf"
        CMAKE_SYSTEM_PROCESSOR="arm"
        CROSS_COMPILE_PREFIX="arm-linux-gnueabihf"
        ;;
    i386|x86)
        TARGET_ARCH="i386"
        DEBIAN_ARCH="i386"
        CMAKE_SYSTEM_PROCESSOR="i686"
        CROSS_COMPILE_PREFIX="i686-linux-gnu"
        ;;
    *)
        log_error "Unsupported architecture: $TARGET_ARCH"
        echo "Supported architectures: arm64, armhf, i386"
        exit 1
        ;;
esac

log_info "Cross-compiling for architecture: $TARGET_ARCH"

# Source dependencies
DEPS_FILE="$PROJECT_ROOT/linux-dependencies.sh"
if [ ! -f "$DEPS_FILE" ]; then
    log_error "Dependencies file not found: $DEPS_FILE"
    exit 1
fi

log_info "Sourcing dependencies from $DEPS_FILE"
source "$DEPS_FILE"

if [ -z "${DEPENDENCIES+x}" ] || [ ${#DEPENDENCIES[@]} -eq 0 ]; then
    log_error "DEPENDENCIES array not set or empty in $DEPS_FILE"
    exit 1
fi

# Setup directories
# Note: SYSROOT_DIR kept for backwards compatibility in cleanup, but not used for building
SYSROOT_DIR="$PROJECT_ROOT/sysroot-$TARGET_ARCH"  
BUILD_DIR="$PROJECT_ROOT/build-$TARGET_ARCH"
TOOLCHAIN_FILE="$PROJECT_ROOT/cmake-toolchain-$TARGET_ARCH.cmake"

log_info "Build directory: $BUILD_DIR"
log_info "Using system multiarch (no separate sysroot)"

# Setup multiarch and install cross-compilation tools
setup_cross_tools() {
    log_info "Setting up cross-compilation tools..."
    
    check_sudo
    
    # For ARM architectures, ensure ports.ubuntu.com is configured
    if [ "$DEBIAN_ARCH" = "arm64" ] || [ "$DEBIAN_ARCH" = "armhf" ]; then
        log_info "Configuring Ubuntu Ports repository for ARM architecture..."
        UBUNTU_CODENAME=$(lsb_release -cs)
        
        # Handle new DEB822 format (.sources files)
        UBUNTU_SOURCES="/etc/apt/sources.list.d/ubuntu.sources"
        if [ -f "$UBUNTU_SOURCES" ]; then
            UBUNTU_SOURCES_BACKUP="/etc/apt/sources.list.d/ubuntu.sources.bak"
            if [ ! -f "$UBUNTU_SOURCES_BACKUP" ]; then
                log_info "Backing up ubuntu.sources..."
                sudo cp "$UBUNTU_SOURCES" "$UBUNTU_SOURCES_BACKUP"
            fi
            
            log_info "Restricting ubuntu.sources to amd64 architecture..."
            # Add "Architectures: amd64" line after each "Types: deb" line if not already present
            sudo awk '/^Types: deb/ && !done {print; print "Architectures: amd64"; done=1; next} 
                      /^Types: deb/ {print; print "Architectures: amd64"; next}
                      /^Architectures:/ {next}
                      {print}' "$UBUNTU_SOURCES_BACKUP" | sudo tee "$UBUNTU_SOURCES" > /dev/null
        fi
        
        # Handle old format sources.list if it exists and has content
        SOURCES_LIST="/etc/apt/sources.list"
        if [ -s "$SOURCES_LIST" ]; then
            SOURCES_BACKUP="/etc/apt/sources.list.backup-cross-compile"
            if [ ! -f "$SOURCES_BACKUP" ]; then
                log_info "Backing up sources.list..."
                sudo cp "$SOURCES_LIST" "$SOURCES_BACKUP"
            fi
            
            log_info "Restricting sources.list to amd64..."
            sudo sed -i '/^deb / { /\[arch/ !s/^deb /deb [arch=amd64] / }' "$SOURCES_LIST"
        fi
        
        # Create Ubuntu Ports sources file
        PORTS_LIST="/etc/apt/sources.list.d/ubuntu-ports.list"
        cat << EOF | sudo tee "$PORTS_LIST" > /dev/null
# Ubuntu Ports for ARM architectures
deb [arch=arm64,armhf] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME} main restricted universe multiverse
deb [arch=arm64,armhf] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-updates main restricted universe multiverse
deb [arch=arm64,armhf] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-security main restricted universe multiverse
deb [arch=arm64,armhf] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-backports main restricted universe multiverse
EOF
        
        log_info "Ubuntu Ports repository configured"
    fi
    
    # Add foreign architecture
    if ! dpkg --print-foreign-architectures | grep -q "^${DEBIAN_ARCH}$"; then
        log_info "Adding foreign architecture: $DEBIAN_ARCH"
        sudo dpkg --add-architecture "$DEBIAN_ARCH"
        sudo apt-get update
    else
        log_info "Architecture $DEBIAN_ARCH already configured"
        # Still update to ensure we have the ports repo indexed
        if [ "$DEBIAN_ARCH" = "arm64" ] || [ "$DEBIAN_ARCH" = "armhf" ]; then
            log_info "Updating package lists for ports repository..."
            sudo apt-get update
        fi
    fi
    
    # Install cross-compilation toolchain and essential build tools
    log_info "Installing cross-compilation toolchain and build tools..."
    
    # Check and fix any broken packages first
    fix_broken_packages
    
    # First ensure Python and core packages are locked to amd64
    log_info "Protecting critical system packages from cross-arch installation..."
    sudo apt-mark hold python3:$DEBIAN_ARCH python3-minimal:$DEBIAN_ARCH python3.12:$DEBIAN_ARCH python3.12-minimal:$DEBIAN_ARCH 2>/dev/null || true
    
    # Mark git and jq as manually installed to prevent auto-removal
    sudo apt-mark manual git jq 2>/dev/null || true
    
    # Install toolchain with error handling
    # Update package lists to get latest versions
    sudo apt-get update
    
    # Install build-essential first (for amd64) - must be done before arm64 libc6-dev
    log_info "Installing build-essential for host (amd64)..."
    sudo apt-get install -y build-essential git jq
    
    # Now install core development packages for target architecture
    log_info "Installing core development packages for ${DEBIAN_ARCH}..."
    
    # Install core packages with --fix-missing to handle unavailable versions
    if ! sudo apt-get install -y --fix-missing \
        linux-libc-dev:${DEBIAN_ARCH} \
        libc6-dev:${DEBIAN_ARCH} \
        libssh-4:${DEBIAN_ARCH}; then
        log_warn "Some core packages failed to install"
        
        # Try installing without libssh-4 if it's not available
        log_info "Attempting to install without libssh-4..."
        sudo apt-get install -y --fix-missing \
            linux-libc-dev:${DEBIAN_ARCH} \
            libc6-dev:${DEBIAN_ARCH} || log_warn "Critical packages failed to install"
    fi
    
    # Then install cross-compilation specific packages (avoid crossbuild-essential which pulls conflicting packages)
    if ! sudo apt-get install -y \
        g++-${CROSS_COMPILE_PREFIX} \
        gcc-${CROSS_COMPILE_PREFIX} \
        binutils-${CROSS_COMPILE_PREFIX}; then
        log_warn "Some toolchain packages failed, attempting to fix..."
        fix_broken_packages
        sudo apt-get install -y \
            g++-${CROSS_COMPILE_PREFIX} \
            gcc-${CROSS_COMPILE_PREFIX} \
            binutils-${CROSS_COMPILE_PREFIX} || log_error "Failed to install toolchain"
    fi
    
    # Install build tools if not present
    sudo apt-get install -y \
        cmake \
        ninja-build \
        pkg-config \
        rsync \
        git \
        jq || log_warn "Some build tools may not have installed"
    
    # Verify compilers are installed
    if ! command -v ${CROSS_COMPILE_PREFIX}-gcc &> /dev/null; then
        log_error "Cross-compiler ${CROSS_COMPILE_PREFIX}-gcc not found after installation"
        exit 1
    fi
    
    if ! command -v ${CROSS_COMPILE_PREFIX}-g++ &> /dev/null; then
        log_error "Cross-compiler ${CROSS_COMPILE_PREFIX}-g++ not found after installation"
        exit 1
    fi
    
    log_info "Cross-compilation toolchain verified:"
    log_info "  C compiler: $(${CROSS_COMPILE_PREFIX}-gcc --version | head -n1)"
    log_info "  C++ compiler: $(${CROSS_COMPILE_PREFIX}-g++ --version | head -n1)"
}

# Install dependencies for target architecture
install_dependencies() {
    log_info "Installing dependencies for $TARGET_ARCH..."
    
    check_sudo
    
    # Check and fix any broken packages first
    fix_broken_packages
    
    # Build list of packages with architecture suffix
    local packages=()
    for dep in "${DEPENDENCIES[@]}"; do
        # Skip wildcard packages for cross-arch installation
        if [[ "$dep" != *"*"* ]]; then
            packages+=("${dep}:${DEBIAN_ARCH}")
        fi
    done
    
    log_info "Installing packages: ${packages[*]}"
    
    # Install dependencies with conflict handling
    if [ ${#packages[@]} -gt 0 ]; then
        if ! sudo apt-get install -y "${packages[@]}" 2>&1 | tee /tmp/apt-install-$$.log; then
            log_warn "Initial installation failed, checking for conflicts..."
            
            # Check for file conflicts (like Pango GIR files)
            if grep -q "trying to overwrite shared" /tmp/apt-install-$$.log; then
                log_info "Detected file conflicts, using --force-overwrite for conflicting packages..."
                
                # Get list of conflicting packages
                local conflict_pkgs=$(grep "trying to overwrite shared" /tmp/apt-install-$$.log | grep -oP "package \K[^:]+:\S+" | sort -u)
                
                # Download and force install conflicting packages
                for pkg in $conflict_pkgs; do
                    log_info "Force installing $pkg..."
                    sudo apt-get download "$pkg" 2>/dev/null || true
                    local deb_file=$(ls -t ${pkg%%:*}_*.deb 2>/dev/null | head -1)
                    if [ -f "$deb_file" ]; then
                        sudo dpkg -i --force-overwrite "$deb_file" || true
                        rm -f "$deb_file"
                    fi
                done
                
                # Now fix dependencies
                sudo apt-get install -f -y || log_warn "Some dependencies may be incomplete"
                
                # Retry installation
                sudo apt-get install -y "${packages[@]}" || {
                    log_warn "Some packages still failed to install, continuing anyway..."
                }
            else
                log_warn "Installation failed but no obvious conflicts detected"
                fix_broken_packages
                # Try once more
                sudo apt-get install -y "${packages[@]}" || {
                    log_error "Failed to install some dependencies"
                }
            fi
            log_warn "Continuing anyway, build might fail if critical packages are missing"
        fi
        
        # Cleanup temp log
        rm -f /tmp/apt-install-$$.log
    fi
}

# Create sysroot - Using system multiarch instead (simpler and more reliable)
create_sysroot() {
    log_info "Skipping sysroot creation - using system multiarch directories"
    log_info "Cross-compiler will use:"
    log_info "  Libraries: /usr/lib/${CROSS_COMPILE_PREFIX}/"
    log_info "  Headers: /usr/include/${CROSS_COMPILE_PREFIX}/"
    log_info "  System headers: /usr/${CROSS_COMPILE_PREFIX}/include/"
    
    # Verify the directories exist
    if [ ! -d "/usr/lib/${CROSS_COMPILE_PREFIX}" ]; then
        log_warn "Target library directory not found: /usr/lib/${CROSS_COMPILE_PREFIX}"
        log_warn "Make sure target architecture packages are installed"
    fi
}

# OLD SYSROOT CREATION (kept for reference, not used)
create_sysroot_old() {
    log_info "Creating sysroot..."
    
    mkdir -p "$SYSROOT_DIR/usr/lib"
    mkdir -p "$SYSROOT_DIR/usr/include"
    
    # IMPORTANT: Do NOT copy base system headers like /usr/include/bits, /usr/include/sys, etc.
    # The cross-compiler has its own architecture-appropriate versions
    # We only copy library-specific headers and the actual libraries
    
    # Copy architecture-specific libraries
    if [ -d "/usr/lib/${CROSS_COMPILE_PREFIX}" ]; then
        log_info "Copying ${CROSS_COMPILE_PREFIX} libraries..."
        rsync -av "/usr/lib/${CROSS_COMPILE_PREFIX}/" "$SYSROOT_DIR/usr/lib/${CROSS_COMPILE_PREFIX}/" || true
    fi
    
    # Copy from alternative lib location if it exists
    if [ -d "/usr/${CROSS_COMPILE_PREFIX}/lib" ]; then
        log_info "Copying libraries from /usr/${CROSS_COMPILE_PREFIX}/lib..."
        rsync -av "/usr/${CROSS_COMPILE_PREFIX}/lib/" "$SYSROOT_DIR/usr/lib/" || true
    fi
    
    # Copy library-specific headers (pkg-config, etc.) but NOT base system headers
    # Only copy headers from /usr/include/<arch>/ which are library-specific
    if [ -d "/usr/include/${CROSS_COMPILE_PREFIX}" ]; then
        log_info "Copying architecture-specific library headers..."
        rsync -av "/usr/include/${CROSS_COMPILE_PREFIX}/" "$SYSROOT_DIR/usr/include/${CROSS_COMPILE_PREFIX}/" || true
    fi
    
    # Copy pkg-config files
    if [ -d "/usr/lib/${CROSS_COMPILE_PREFIX}/pkgconfig" ]; then
        log_info "Copying pkg-config files..."
        mkdir -p "$SYSROOT_DIR/usr/lib/pkgconfig"
        rsync -av "/usr/lib/${CROSS_COMPILE_PREFIX}/pkgconfig/" "$SYSROOT_DIR/usr/lib/pkgconfig/" || true
    fi
    
    # Copy library .so files and development files
    if [ -d "/usr/lib/${DEBIAN_ARCH}-linux-gnu" ]; then
        log_info "Copying ${DEBIAN_ARCH} libraries..."
        mkdir -p "$SYSROOT_DIR/usr/lib/${DEBIAN_ARCH}-linux-gnu"
        rsync -av "/usr/lib/${DEBIAN_ARCH}-linux-gnu/" "$SYSROOT_DIR/usr/lib/${DEBIAN_ARCH}-linux-gnu/" \
            --exclude '*.py' --exclude '__pycache__' || true
    fi
    
    # Copy architecture-specific headers from the multiarch include directory
    # These are library-specific headers, not base system headers
    if [ -d "/usr/include/${DEBIAN_ARCH}-linux-gnu" ]; then
        log_info "Copying ${DEBIAN_ARCH} library-specific headers..."
        mkdir -p "$SYSROOT_DIR/usr/include/${DEBIAN_ARCH}-linux-gnu"
        rsync -av "/usr/include/${DEBIAN_ARCH}-linux-gnu/" "$SYSROOT_DIR/usr/include/${DEBIAN_ARCH}-linux-gnu/" || true
    fi
    
    log_info "Sysroot created at $SYSROOT_DIR"
    log_info "Note: Base system headers are provided by the cross-compiler toolchain"
}

# Generate CMake toolchain file
generate_toolchain() {
    log_info "Generating CMake toolchain file..."
    
    # Get the path to pre-built juceaide
    local HOST_BUILD_DIR="$PROJECT_ROOT/build-host-tools"
    local JUCEAIDE_PATH="$HOST_BUILD_DIR/JUCE/tools/juceaide"
    
    cat > "$TOOLCHAIN_FILE" << EOF
# CMake toolchain file for cross-compiling to $TARGET_ARCH
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $CMAKE_SYSTEM_PROCESSOR)

# Specify the cross compiler
set(CMAKE_C_COMPILER ${CROSS_COMPILE_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE_PREFIX}-g++)

# Add custom CMake module path for finding packages
list(APPEND CMAKE_MODULE_PATH "$PROJECT_ROOT/cmake/linux")

# Use system's multiarch directories - no separate sysroot needed
# The cross-compiler toolchain already knows about:
#   /usr/lib/${CROSS_COMPILE_PREFIX}/
#   /usr/include/${CROSS_COMPILE_PREFIX}/
# Ubuntu's multiarch support handles everything

# Set CMAKE_LIBRARY_ARCHITECTURE for multiarch support
set(CMAKE_LIBRARY_ARCHITECTURE ${CROSS_COMPILE_PREFIX})

# Set root paths for finding libraries - exclude Linuxbrew/Homebrew
set(CMAKE_FIND_ROOT_PATH 
    /usr
    /usr/lib/${CROSS_COMPILE_PREFIX}
    /usr/include/${CROSS_COMPILE_PREFIX}
)

# Add prefix paths for finding CMake package config files
set(CMAKE_PREFIX_PATH
    /usr
    /usr/lib/${CROSS_COMPILE_PREFIX}
    /usr/lib/${CROSS_COMPILE_PREFIX}/cmake
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
if(EXISTS "$JUCEAIDE_PATH")
    set(JUCE_JUCEAIDE_PATH "$JUCEAIDE_PATH" CACHE FILEPATH "Path to juceaide" FORCE)
    message(STATUS "Using pre-built juceaide: $JUCEAIDE_PATH")
endif()

# pkg-config configuration for cross-compilation
set(PKG_CONFIG_EXECUTABLE /usr/bin/${CROSS_COMPILE_PREFIX}-pkg-config)
if(NOT EXISTS "\${PKG_CONFIG_EXECUTABLE}")
    # Fallback to regular pkg-config with environment variables
    set(PKG_CONFIG_EXECUTABLE /usr/bin/pkg-config)
    set(ENV{PKG_CONFIG_PATH} "")
    set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${CROSS_COMPILE_PREFIX}/pkgconfig:/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")
endif()

# Explicit hints for FindLibObs.cmake
set(LIBOBS_LIB_PATH "/usr/lib/${CROSS_COMPILE_PREFIX}" CACHE PATH "Path to libobs library")
set(LIBOBS_INCLUDE_PATH "/usr/include" CACHE PATH "Path to libobs headers")
EOF

    log_info "Toolchain file created at $TOOLCHAIN_FILE"
}

# Build juceaide for host system
build_host_juceaide() {
    log_info "Building juceaide for host system..."
    
    local HOST_BUILD_DIR="$PROJECT_ROOT/build-host-tools"
    
    # Check if juceaide already exists and works
    if [ -f "$HOST_BUILD_DIR/JUCE/tools/juceaide" ] && "$HOST_BUILD_DIR/JUCE/tools/juceaide" --help &>/dev/null; then
        log_info "juceaide already built and working for host system"
        export PATH="$HOST_BUILD_DIR/JUCE/tools:$PATH"
        return 0
    fi
    
    # Ensure native build dependencies are installed for host
    log_info "Installing native build dependencies for juceaide..."
    sudo apt-get install -y \
        libfreetype-dev \
        libfontconfig1-dev \
        pkg-config || log_warn "Some native dependencies failed to install"
    
    mkdir -p "$HOST_BUILD_DIR"
    cd "$HOST_BUILD_DIR"
    
    # Configure entire project for host (just to build juceaide)
    # Make sure to unset any cross-compilation variables
    log_info "Configuring project for host architecture to build juceaide..."
    (unset CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT; \
     cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -G Ninja \
        "$PROJECT_ROOT") || {
        log_warn "Failed to configure for host, continuing anyway..."
        return 0
    }
    
    # Try to build just the juceaide target if it exists
    log_info "Building juceaide target..."
    if cmake --build . --target juceaide 2>/dev/null; then
        log_info "Host juceaide built successfully"
        export PATH="$HOST_BUILD_DIR/JUCE/tools:$PATH"
    else
        log_warn "Could not build juceaide specifically, trying full build..."
        # Build everything for host - this ensures juceaide is available
        cmake --build . || {
            log_warn "Failed to build juceaide, cross-compilation may fail..."
            return 0
        }
        log_info "Host tools built successfully"
        export PATH="$HOST_BUILD_DIR/JUCE/tools:$PATH"
    fi
}

# Configure CMake
configure_cmake() {
    log_info "Configuring CMake..."
    
    # Debug: Check for OBS library files
    log_info "Checking for OBS library files..."
    find /usr/lib/${CROSS_COMPILE_PREFIX} -name "*obs*" -type f 2>/dev/null | head -10 || true
    find /usr/lib/${CROSS_COMPILE_PREFIX} -name "*Obs*.cmake" -o -name "*obs*.cmake" 2>/dev/null || true
    find /usr/share -name "*Obs*.cmake" -o -name "*obs*.cmake" 2>/dev/null | head -5 || true
    
    # Check pkg-config
    log_info "Checking pkg-config for libobs..."
    PKG_CONFIG_LIBDIR="/usr/lib/${CROSS_COMPILE_PREFIX}/pkgconfig:/usr/share/pkgconfig" \
        pkg-config --exists libobs && \
        PKG_CONFIG_LIBDIR="/usr/lib/${CROSS_COMPILE_PREFIX}/pkgconfig:/usr/share/pkgconfig" \
        pkg-config --cflags --libs libobs || log_warn "pkg-config cannot find libobs"
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Try to find libobs library and headers for cross-arch
    local LIBOBS_LIB_PATH=""
    local LIBOBS_INCLUDE_PATH=""
    
    if [ -f "/usr/lib/${CROSS_COMPILE_PREFIX}/libobs.so" ]; then
        LIBOBS_LIB_PATH="/usr/lib/${CROSS_COMPILE_PREFIX}"
    elif [ -f "/usr/lib/${CROSS_COMPILE_PREFIX}/lib/libobs.so" ]; then
        LIBOBS_LIB_PATH="/usr/lib/${CROSS_COMPILE_PREFIX}/lib"
    fi
    
    if [ -d "/usr/include/${CROSS_COMPILE_PREFIX}/obs" ]; then
        LIBOBS_INCLUDE_PATH="/usr/include/${CROSS_COMPILE_PREFIX}"
    elif [ -d "/usr/include/obs" ]; then
        LIBOBS_INCLUDE_PATH="/usr/include"
    fi
    
    log_info "LibOBS library path: ${LIBOBS_LIB_PATH:-not found}"
    log_info "LibOBS include path: ${LIBOBS_INCLUDE_PATH:-not found}"
    
    local -a cmake_args=(
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
        -DCMAKE_BUILD_TYPE=Release
        -DJUCE_BUILD_HELPER_TOOLS=OFF
        -G Ninja
    )
    
    # Add libobs paths if found
    if [ -n "$LIBOBS_LIB_PATH" ]; then
        cmake_args+=(-DLIBOBS_LIB_PATH="$LIBOBS_LIB_PATH")
    fi
    if [ -n "$LIBOBS_INCLUDE_PATH" ]; then
        cmake_args+=(-DLIBOBS_INCLUDE_PATH="$LIBOBS_INCLUDE_PATH")
    fi
    
    cmake "${cmake_args[@]}" "$PROJECT_ROOT"
    
    log_info "CMake configuration complete"
}

# Build project
build_project() {
    log_info "Building project..."
    
    cd "$BUILD_DIR"
    cmake --build . --config Release -j$(nproc)
    
    log_info "Build complete!"
}

# Main execution
main() {
    log_info "Starting cross-compilation for $TARGET_ARCH"
    log_info "================================================"
    
    # Disable Linuxbrew/Homebrew to prevent interference
    disable_brew
    
    # Ensure brew is re-enabled even if script fails
    trap enable_brew EXIT INT TERM
    
    setup_cross_tools
    install_dependencies
    create_sysroot
    build_host_juceaide
    generate_toolchain
    configure_cmake
    build_project
    
    # Re-enable Linuxbrew/Homebrew
    enable_brew
    
    log_info "================================================"
    log_info "Cross-compilation successful!"
    log_info "Build artifacts are in: $BUILD_DIR"
    log_info "Architecture: $TARGET_ARCH"
}

# Run main function
main
