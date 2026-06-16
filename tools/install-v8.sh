#!/usr/bin/env bash
set -euo pipefail

started_at="$(date +%s)"
step_total=9
step_index=0
current_step="startup"

usage() {
    cat <<'EOF'
Install a V8 build suitable for winglib2.

This script checks out V8 with depot_tools, builds a monolithic static V8,
and stages it into a prefix with this layout:

  <prefix>/include/v8.h
  <prefix>/lib/libv8_monolith.a

Usage:
  tools/install-v8.sh [options]

Options:
  --prefix PATH          Install prefix. Default: ./winglib2/.deps/<system>-<arch>/<build-type>/v8
  --work-dir PATH        Checkout/build directory. Default: ./winglib2/.deps/v8-src
  --depot-tools PATH     depot_tools directory. Default: ./winglib2/.deps/depot_tools
  --branch BRANCH        Optional V8 branch/tag/ref to check out after fetch.
  --target-cpu CPU       GN target_cpu and V8 cpu. Default: x64
  --build-type TYPE      release or debug. Default: release
  --jobs N               Ninja parallelism. Default: ninja's default
  --gclient-jobs N       gclient sync parallelism. Default: 1
  --with-i18n            Enable V8 i18n/ICU support. Default: disabled
  --system-libcxx        Build against the system C++ standard library and disable V8 sandbox
  --skip-sync            Skip gclient sync. Only use with a known-good checkout.
  --skip-deps            Skip Linux install-build-deps.sh
  --unsupported-deps     Pass --unsupported to Chromium's Linux dependency installer
  --force-sync           Accepted for compatibility; sync is now the default
  --help                 Show this help

After install:
  cmake -S winglib2 -B winglib2/build/v8 -G Ninja \
    -DWL2_ENABLE_V8=ON \
    -DWL2_V8_ROOT=<prefix>
EOF
}

timestamp() {
    date "+%H:%M:%S"
}

elapsed() {
    local now
    now="$(date +%s)"
    printf "%02d:%02d:%02d" $(((now - started_at) / 3600)) $((((now - started_at) % 3600) / 60)) $(((now - started_at) % 60))
}

step() {
    step_index=$((step_index + 1))
    current_step="$1"
    printf "\n[%s] [v8] [%d/%d] %s\n" "$(timestamp)" "${step_index}" "${step_total}" "${current_step}"
}

info() {
    printf "[%s] [v8] %s\n" "$(timestamp)" "$*"
}

run() {
    printf "[%s] [v8] $" "$(timestamp)"
    for arg in "$@"; do
        printf " %q" "${arg}"
    done
    printf "\n"
    "$@"
}

finish() {
    local exit_code=$?
    if [[ ${exit_code} -ne 0 ]]; then
        printf "\n[%s] [v8] Failed during step: %s\n" "$(timestamp)" "${current_step}" >&2
        printf "[%s] [v8] Elapsed: %s\n" "$(timestamp)" "$(elapsed)" >&2
    fi
    exit "${exit_code}"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"

system_name="$(uname -s | tr '[:upper:]' '[:lower:]')"
system_arch="$(uname -m | tr '[:upper:]' '[:lower:]')"
case "${system_arch}" in
    x86_64|amd64) system_arch="x86_64" ;;
    aarch64) system_arch="aarch64" ;;
esac

prefix=""
work_dir="${project_dir}/.deps/v8-src"
depot_tools_dir="${project_dir}/.deps/depot_tools"
branch=""
target_cpu="x64"
build_type="release"
jobs=""
gclient_jobs="1"
enable_i18n="false"
system_libcxx="false"
skip_deps="false"
skip_sync="false"
unsupported_deps="false"
force_sync="false"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            prefix="$2"
            shift 2
            ;;
        --work-dir)
            work_dir="$2"
            shift 2
            ;;
        --depot-tools)
            depot_tools_dir="$2"
            shift 2
            ;;
        --branch)
            branch="$2"
            shift 2
            ;;
        --target-cpu)
            target_cpu="$2"
            shift 2
            ;;
        --build-type)
            build_type="$2"
            shift 2
            ;;
        --jobs)
            jobs="$2"
            shift 2
            ;;
        --gclient-jobs)
            gclient_jobs="$2"
            shift 2
            ;;
        --with-i18n)
            enable_i18n="true"
            shift
            ;;
        --system-libcxx)
            system_libcxx="true"
            shift
            ;;
        --skip-deps)
            skip_deps="true"
            shift
            ;;
        --unsupported-deps)
            unsupported_deps="true"
            shift
            ;;
        --skip-sync)
            skip_sync="true"
            shift
            ;;
        --force-sync)
            force_sync="true"
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "${build_type}" in
    release|debug) ;;
    *)
        echo "--build-type must be release or debug" >&2
        exit 2
        ;;
esac

case "${target_cpu}" in
    x64|arm64|arm|ia32) ;;
    *)
        echo "Unsupported --target-cpu '${target_cpu}'. Expected x64, arm64, arm, or ia32." >&2
        exit 2
        ;;
esac

if [[ -z "${prefix}" ]]; then
    prefix="${project_dir}/.deps/${system_name}-${system_arch}/${build_type}/v8"
fi

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Required command not found: $1" >&2
        exit 1
    fi
}

need_cmd git
need_cmd python3

mkdir -p "$(dirname "${prefix}")" "$(dirname "${work_dir}")" "$(dirname "${depot_tools_dir}")"

trap finish EXIT

step "Configuration"
info "project:      ${project_dir}"
info "prefix:       ${prefix}"
info "work dir:     ${work_dir}"
info "depot_tools:  ${depot_tools_dir}"
info "target cpu:   ${target_cpu}"
info "build type:   ${build_type}"
info "i18n:         ${enable_i18n}"
info "libcxx mode:  $(if [[ "${system_libcxx}" == "true" ]]; then echo "system, V8 sandbox disabled"; else echo "Chromium custom libc++"; fi)"
info "deps mode:    $(if [[ "${skip_deps}" == "true" ]]; then echo "skip"; elif [[ "${unsupported_deps}" == "true" ]]; then echo "unsupported"; else echo "supported"; fi)"
if [[ -n "${branch}" ]]; then
    info "branch/ref:   ${branch}"
fi
if [[ -n "${jobs}" ]]; then
    info "ninja jobs:   ${jobs}"
else
    info "ninja jobs:   default"
fi
info "gclient jobs: ${gclient_jobs}"

step "depot_tools"
if [[ ! -d "${depot_tools_dir}/.git" ]]; then
    info "Cloning depot_tools into ${depot_tools_dir}"
    run git clone --progress https://chromium.googlesource.com/chromium/tools/depot_tools.git "${depot_tools_dir}"
else
    info "Updating depot_tools in ${depot_tools_dir}"
    run git -C "${depot_tools_dir}" fetch --progress origin main
    run git -C "${depot_tools_dir}" checkout -B main origin/main
fi

export PATH="${depot_tools_dir}:${PATH}"
export DEPOT_TOOLS_UPDATE=1

if ! command -v fetch >/dev/null 2>&1; then
    echo "depot_tools fetch command is not available after PATH update" >&2
    exit 1
fi

step "V8 checkout"
if [[ ! -d "${work_dir}/v8/.git" ]]; then
    info "Configuring V8 checkout in ${work_dir}"
    mkdir -p "${work_dir}"
    (
        cd "${work_dir}"
        if [[ ! -f ".gclient" ]]; then
            run gclient config --name v8 https://chromium.googlesource.com/v8/v8.git
        else
            info "Reusing existing .gclient config"
        fi
    )
else
    info "Reusing existing checkout ${work_dir}/v8"
fi

v8_dir="${work_dir}/v8"

step "V8 dependency sync"
if [[ "${skip_sync}" != "true" ]]; then
    info "Running gclient sync. Default --gclient-jobs=1 avoids depot_tools gsutil lock contention."
    (
        cd "${work_dir}"
        run gclient sync --with_branch_heads --jobs "${gclient_jobs}" --verbose
    )
else
    info "Skipping gclient sync because --skip-sync was specified"
fi

cd "${v8_dir}"

step "V8 ref selection"
if [[ -n "${branch}" ]]; then
    info "Checking out ${branch}"
    run git fetch --all --tags --progress
    run git checkout "${branch}"
    info "Syncing dependencies for selected ref"
    (
        cd "${work_dir}"
        run gclient sync --with_branch_heads --jobs "${gclient_jobs}" --verbose
    )
else
    info "Using current checkout ref"
fi

step "System build dependencies"
if [[ "${skip_deps}" != "true" && "$(uname -s)" == "Linux" ]]; then
    info "Installing Linux build dependencies. This may ask for sudo."
    dep_args=()
    if [[ "${unsupported_deps}" == "true" ]]; then
        dep_args+=(--unsupported)
    fi
    run ./build/install-build-deps.sh "${dep_args[@]}"
else
    info "Skipping system dependency installation"
fi

out_dir="out.gn/${target_cpu}.${build_type}.winglib2"
is_debug="false"
if [[ "${build_type}" == "debug" ]]; then
    is_debug="true"
fi
use_custom_libcxx="true"
v8_enable_sandbox="true"
if [[ "${system_libcxx}" == "true" ]]; then
    use_custom_libcxx="false"
    v8_enable_sandbox="false"
fi

step "GN generation"
gn_args="
is_debug = ${is_debug}
is_component_build = false
target_cpu = \"${target_cpu}\"
v8_target_cpu = \"${target_cpu}\"
v8_monolithic = true
v8_use_external_startup_data = false
v8_enable_i18n_support = ${enable_i18n}
use_custom_libcxx = ${use_custom_libcxx}
v8_enable_sandbox = ${v8_enable_sandbox}
"
info "Generating GN build in ${out_dir}"
info "GN args:${gn_args}"
run gn gen "${out_dir}" --args="${gn_args}"

step "Ninja build"
ninja_args=(-C "${out_dir}")
if [[ -n "${jobs}" ]]; then
    ninja_args+=("-j${jobs}")
fi
export NINJA_STATUS="[%f/%t %es] "
info "Building v8_monolith. Ninja will show completed edges, total edges, and elapsed seconds."
run ninja "${ninja_args[@]}" v8_monolith

lib_path="${out_dir}/obj/libv8_monolith.a"
if [[ ! -f "${lib_path}" ]]; then
    echo "Expected V8 monolith not found: ${lib_path}" >&2
    exit 1
fi

step "Install"
info "Installing to ${prefix}"
rm -rf "${prefix}"
mkdir -p "${prefix}/include" "${prefix}/lib"
cp -R include/. "${prefix}/include/"
cp "${lib_path}" "${prefix}/lib/libv8_monolith.a"

cat > "${prefix}/winglib2-v8-config.cmake" <<EOF
# Generated by winglib2/tools/install-v8.sh
set(WL2_V8_ROOT "${prefix}")
set(WL2_V8_EXTRA_LIBRARIES "")
EOF

step "Summary"
cat <<EOF

[v8] Installed successfully in $(elapsed).

Installed files:
  ${prefix}/include/v8.h
  ${prefix}/lib/libv8_monolith.a
  ${prefix}/winglib2-v8-config.cmake

Use it with:
  cmake -S winglib2 -B winglib2/build/v8 -G Ninja \\
    -DWL2_ENABLE_V8=ON \\
    -DWL2_V8_ROOT=${prefix}

EOF
