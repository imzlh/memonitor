#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${MEMONITOR_BUILD_DIR:-${project_dir}/build-deb}"
package_dir="${MEMONITOR_PACKAGE_DIR:-${project_dir}/dist}"
build_testing="${BUILD_TESTING:-ON}"

case "$build_testing" in
    ON|OFF) ;;
    *) printf 'BUILD_TESTING must be ON or OFF\n' >&2; exit 1 ;;
esac

cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING="$build_testing"

package_version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build_dir/CMakeCache.txt")"
if [[ -z "$package_version" ]]; then
    printf 'Could not determine the package version from CMake\n' >&2
    exit 1
fi
if [[ -n "${RELEASE_TAG:-}" && "$RELEASE_TAG" != "v${package_version}" ]]; then
    printf 'Release tag %s must match the CMake project version v%s\n' "$RELEASE_TAG" "$package_version" >&2
    exit 1
fi

cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
if [[ "$build_testing" == ON ]]; then
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" --output-on-failure --timeout 120
fi

staging_dir="$(mktemp -d)"
trap 'rm -rf -- "$staging_dir"' EXIT
cpack --config "$build_dir/CPackConfig.cmake" -G DEB -B "$staging_dir/packages"

architecture="$(dpkg --print-architecture)"
package_name="memonitor_${package_version}_${architecture}.deb"
package_path="${package_dir}/${package_name}"
mkdir -p -- "$package_dir"
mv -- "$staging_dir/packages/$package_name" "$package_path"
test "$(dpkg-deb --field "$package_path" Architecture)" = "$architecture"
test "$(dpkg-deb --field "$package_path" Version)" = "$package_version"
dpkg-deb --info "$package_path"

dpkg-deb --extract "$package_path" "$staging_dir/root"
test -f "$staging_dir/root/usr/share/applications/memonitor.desktop"
timeout 30s "$staging_dir/root/usr/bin/memonitor" --dump

(
    cd -- "$package_dir"
    sha256sum "$package_name" > "${package_name}.sha256"
)
