#!/bin/bash
set -e

# Parse command-line arguments
TARGET_ARCH=""
DEBIAN_ARCH=""
APT_ARCH_SUFFIX=""
CROSS_COMPILE="false"
CROSS_TRIPLE=""
CMAKE_SYSTEM_PROCESSOR=""
BUILD_CONFIG="RelWithDebInfo"

while [[ $# -gt 0 ]]; do
  case $1 in
    --target-arch) TARGET_ARCH="$2"; shift 2 ;;
    --debian-arch) DEBIAN_ARCH="$2"; shift 2 ;;
    --apt-arch-suffix) APT_ARCH_SUFFIX="$2"; shift 2 ;;
    --cross-compile) CROSS_COMPILE="$2"; shift 2 ;;
    --cross-triple) CROSS_TRIPLE="$2"; shift 2 ;;
    --cmake-system-processor) CMAKE_SYSTEM_PROCESSOR="$2"; shift 2 ;;
    --build-config) BUILD_CONFIG="$2"; shift 2 ;;
    *) echo "Unknown parameter: $1"; exit 1 ;;
  esac
done

# Validate required parameters
if [ -z "$TARGET_ARCH" ]; then
  echo "ERROR: --target-arch is required"
  exit 1
fi

# Map TARGET_ARCH to cross-compile triple prefix for binutils
case "${TARGET_ARCH}" in
  arm64) export CROSS_PREFIX="aarch64" ;;
  *) export CROSS_PREFIX="${TARGET_ARCH}" ;;
esac

# Verify we're running on x86_64 (not ARM64 emulation)
echo "Container architecture: $(uname -m)"
[ "$(uname -m)" = "aarch64" ] && echo "ERROR: Running on ARM64 emulation!" && exit 1

# Configure environment
export DEBIAN_FRONTEND=noninteractive

repair_dpkg_state() {
  if [ -d /var/lib/dpkg ] && [ -n "$(ls -A /var/lib/dpkg/updates 2>/dev/null)" ]; then
    echo "Detected interrupted dpkg state; repairing"
  fi

  dpkg --configure -a || true
  apt-get -f install -y || true
}

safe_apt_install() {
  repair_dpkg_state
  apt-get install -y --no-install-recommends "$@"
}

# Install base packages
apt-get update
safe_apt_install git software-properties-common
git config --global --add safe.directory /workspace

# Configure multi-arch for cross-compilation
if [ "${CROSS_COMPILE}" = "true" ]; then
  echo "=== Setting up cross-compilation for ${TARGET_ARCH} ==="
  
  # Detect Ubuntu codename dynamically
  UBUNTU_CODENAME=$(. /etc/os-release && echo "$VERSION_CODENAME")
  echo "Detected Ubuntu codename: ${UBUNTU_CODENAME}"
  
  # Restrict default sources to amd64 (Ubuntu 24.04+ DEB822 format)
  for sources_file in /etc/apt/sources.list.d/*.sources; do
    [ -f "$sources_file" ] && sed -i '/^Types: deb$/a Architectures: amd64' "$sources_file"
  done
  
  # Add ports mirror for ARM64
  if [[ "${DEBIAN_ARCH}" =~ ^(arm64|armhf|ppc64el|riscv64|s390x)$ ]]; then
    cat > /etc/apt/sources.list.d/ports.sources <<EOF
Types: deb
Architectures: ${DEBIAN_ARCH}
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: ${UBUNTU_CODENAME} ${UBUNTU_CODENAME}-updates ${UBUNTU_CODENAME}-backports ${UBUNTU_CODENAME}-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
  fi
  
  # Enable target architecture
  dpkg --add-architecture ${DEBIAN_ARCH}
  rm -rf /var/lib/apt/lists/*
  apt-get update
fi

# Load dependencies from central file
source /workspace/linux-dependencies.sh

# Build package lists
BUILD_DEPS_BASE="${BUILD_TOOLS[@]}"
[ "${CROSS_COMPILE}" = "true" ] && BUILD_DEPS_BASE="${BUILD_DEPS_BASE} crossbuild-essential-${DEBIAN_ARCH}"

# Apply architecture suffix for cross-compilation
apply_arch_suffix() {
  local result=""
  for pkg in "$@"; do result="${result} ${pkg}${APT_ARCH_SUFFIX}"; done
  echo "${result}"
}

JUCE_DEPS=$(apply_arch_suffix "${JUCE_DEPS[@]}")
OBS_DEPS=$(apply_arch_suffix "${OBS_DEPS[@]}")
QT6_DEPS=$(apply_arch_suffix "${QT6_DEPS[@]}")

# Install build dependencies
safe_apt_install ${BUILD_DEPS_BASE}

# Install target architecture libraries
if [ "${CROSS_COMPILE}" = "true" ]; then
  echo "Installing ${DEBIAN_ARCH} libraries for cross-compilation..."
  
  # Block Python dev packages (allow runtime libs like libpython3.12t64)
  cat > /etc/apt/preferences.d/no-foreign-python <<EOF
Package: python3:${DEBIAN_ARCH} python3.*:${DEBIAN_ARCH} python3-dev:${DEBIAN_ARCH} libpython3-dev:${DEBIAN_ARCH}
Pin: release *
Pin-Priority: -1
EOF

  # Block executable packages that can't run on x86_64
  cat > /etc/apt/preferences.d/no-foreign-bin <<EOF
Package: *-dev-bin:${DEBIAN_ARCH} *-dev-bin-*:${DEBIAN_ARCH}
Pin: release *
Pin-Priority: -1
EOF
  
  safe_apt_install ${JUCE_DEPS} ${OBS_DEPS} ${QT6_DEPS}
  safe_apt_install ${QT6_TOOLS[@]}  # x86_64 moc/uic/rcc
else
  safe_apt_install ${JUCE_DEPS} ${OBS_DEPS} ${QT6_DEPS}
fi

# Configure CMake
BUILD_CONFIG="${BUILD_CONFIG:-Release}"
BUILD_DIR="build_${TARGET_ARCH}"

if [ -f "${BUILD_DIR}/CMakeCache.txt" ]; then
  CACHEFILE_DIR=$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")
  CACHE_HOME_DIR=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")
  CURRENT_BUILD_DIR="$(pwd)/${BUILD_DIR}"
  CURRENT_SOURCE_DIR="$(pwd)"

  if [ "${CACHEFILE_DIR}" != "${CURRENT_BUILD_DIR}" ] || [ "${CACHE_HOME_DIR}" != "${CURRENT_SOURCE_DIR}" ]; then
    echo "Removing stale ${BUILD_DIR} because it was configured for a different workspace path"
    rm -rf "${BUILD_DIR}"
  fi
fi

CMAKE_FLAGS="-B ${BUILD_DIR} -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_CONFIG}"
CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_INSTALL_PREFIX=/usr"

# Cross-compilation configuration
if [ "${CROSS_COMPILE}" = "true" ]; then
  echo "=== Configuring cross-compilation for ${TARGET_ARCH} ==="
  
  # Set PKG_CONFIG paths for ARM64 libraries
  export PKG_CONFIG_PATH="/usr/lib/${CROSS_TRIPLE}/pkgconfig:/usr/share/pkgconfig"
  export PKG_CONFIG_LIBDIR="/usr/lib/${CROSS_TRIPLE}/pkgconfig:/usr/share/pkgconfig"
  
  # Create CMake toolchain file (based on ReaPack's approach)
  # NOTE: CMAKE_SYSTEM_PROCESSOR is hardcoded to arm64 as that's currently the only
  # cross-compile target. If adding other architectures, use ${CMAKE_SYSTEM_PROCESSOR} instead.
  cat > /tmp/toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_C_COMPILER ${CROSS_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_TRIPLE}-g++)
set(CMAKE_OBJCOPY ${CROSS_TRIPLE}-objcopy)
set(CMAKE_STRIP ${CROSS_TRIPLE}-strip)

# Debian multi-arch paths
set(CMAKE_FIND_ROOT_PATH /usr/${CROSS_TRIPLE})
set(CMAKE_LIBRARY_PATH /usr/lib/${CROSS_TRIPLE})
set(CMAKE_INCLUDE_PATH /usr/include/${CROSS_TRIPLE})

# Prevent --sysroot flags (we use multi-arch, not sysroot)
set(CMAKE_SYSROOT_COMPILE "")
set(CMAKE_SYSROOT_LINK "")

# Search modes
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

  # Detect Qt6 tools location
  QT6_TOOLS_BINDIR="/usr/lib/qt6/libexec"
  [ ! -f "${QT6_TOOLS_BINDIR}/moc" ] && QT6_TOOLS_BINDIR="/usr/lib/qt6/bin"
  
  CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_TOOLCHAIN_FILE=/tmp/toolchain.cmake"
  CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_PREFIX_PATH=/usr/lib/${CROSS_TRIPLE}/cmake"
  CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_AUTOMOC_MOC_EXECUTABLE=${QT6_TOOLS_BINDIR}/moc"
  CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_AUTOUIC_UIC_EXECUTABLE=${QT6_TOOLS_BINDIR}/uic"
  CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_AUTORCC_RCC_EXECUTABLE=${QT6_TOOLS_BINDIR}/rcc"
  CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_AUTOMOC_COMPILER_PREDEFINES=OFF"
fi

# Build
cmake ${CMAKE_FLAGS}
cmake --build ${BUILD_DIR} --config ${BUILD_CONFIG} --parallel

# Verify and strip (only strip for Release builds, keep debug symbols for RelWithDebInfo)
BUILT_BINARY=$(find "${BUILD_DIR}" -name '*.so' | head -n 1)
if [ -n "${BUILT_BINARY}" ]; then
  echo "Built binary: $(file "${BUILT_BINARY}")"
else
  echo "Built binary: Not found"
fi
if [ "${CROSS_COMPILE}" = "true" ] && [ "${BUILD_CONFIG}" = "Release" ]; then
  find ${BUILD_DIR} -name '*.so' -exec ${CROSS_TRIPLE}-strip --strip-debug {} \; 2>/dev/null || true
fi



