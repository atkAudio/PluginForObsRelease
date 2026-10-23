#!/usr/bin/env bash
set -euo pipefail

mode="${1:-}"
case "${mode}" in
    asan-ubsan-lsan|tsan|rtsan) shift ;;
    *) printf 'Usage: bash %s {asan-ubsan-lsan|tsan|rtsan} [ctest arguments...]\n' "$0" >&2; exit 2 ;;
esac

source "$(dirname "${BASH_SOURCE[0]}")/../.github/scripts/linux-docker-helpers.sh"
export CONTAINER_ARCH=amd64
export CI=true
preset="ubuntu-x86_64-${mode}"
build_dir="build_x86_64_${mode//-/_}"
runtime="$(container_runtime)"

if ! "${runtime}" image inspect "$(builder_image)" >/dev/null 2>&1; then
    ubuntu_version="${UBUNTU_VERSION:-}"
    if [[ -z "${ubuntu_version}" ]]; then
        ubuntu_version="$("${runtime}" run --rm ubuntu:latest sh -c '. /etc/os-release; printf "%s" "$VERSION_ID"')"
    fi
    if [[ ! "${ubuntu_version}" =~ ^[0-9]{2}\.[0-9]{2}$ ]]; then
        printf 'Invalid Ubuntu release: %s\n' "${ubuntu_version}" >&2
        exit 1
    fi
    "${runtime}" build --platform linux/amd64 --build-arg "UBUNTU_VERSION=${ubuntu_version}" \
        -f "${workspace_dir}/.github/docker/linux-amd64-builder.Dockerfile" \
        -t "$(builder_image)" "${workspace_dir}/.github/docker"
fi

reset_build_dir_on_image_change "${build_dir}"
qt_version="$(run_in_builder /usr/bin/pkg-config --modversion Qt6Core)"
qt_dir="_deps/sanitizer-qt/${mode}/${qt_version}"
reset_build_dir_on_image_change "${qt_dir}"

if [[ "${mode}" == tsan ]]; then
    export CONTAINER_SECCOMP_PROFILE="${workspace_dir}/${build_dir}/tsan-seccomp.json"
    curl --fail --silent --show-error --location --retry 3 \
        https://raw.githubusercontent.com/moby/profiles/main/seccomp/default.json \
        | jq '.syscalls += [{"names":["personality"],"action":"SCMP_ACT_ALLOW","args":[{"index":0,"value":262144,"op":"SCMP_CMP_EQ"}]}]' \
        > "${CONTAINER_SECCOMP_PROFILE}"
fi

run_in_builder env CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
    bash "${workspace_dir}/.github/scripts/build-sanitizer-qt.sh" "${mode}"
qt_root="${workspace_dir}/${qt_dir}/build"
run_in_builder env ATK_SANITIZER_QT_ROOT="${qt_root}" \
    cmake -U 'Qt6*' --preset "${preset}"
run_in_builder cmake --build --preset "${preset}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
run_in_builder env LD_LIBRARY_PATH="${qt_root}/lib" QT_PLUGIN_PATH="${qt_root}/plugins" \
    QT_QPA_PLATFORM_PLUGIN_PATH="${qt_root}/plugins/platforms" \
    ctest --preset "${preset}" "$@"