#!/usr/bin/env bash
# Builds the WebAssembly package and assembles it under bindings/wasm/pkg.
#
# Needs the Emscripten SDK on PATH (emcmake, emcc). Install it with:
#   git clone https://github.com/emscripten-core/emsdk && cd emsdk
#   ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh
#
#   bindings/wasm/build.sh            # release build into bindings/wasm/pkg
#   bindings/wasm/build.sh --test     # and run the Node smoke test
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$root/build-wasm"
package_dir="$root/bindings/wasm/pkg"

if ! command -v emcmake >/dev/null 2>&1; then
  echo "build.sh: emcmake not found. Activate the Emscripten SDK first." >&2
  exit 1
fi

# On Windows the default CMake generator is Visual Studio, which cannot drive
# the Emscripten toolchain, so name a makefile generator explicitly there.
generator=()
case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    if command -v ninja >/dev/null 2>&1; then
      generator=(-G Ninja)
    else
      generator=(-G "MinGW Makefiles")
    fi
    ;;
esac

emcmake cmake -S "$root" -B "$build_dir" "${generator[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGMF_BUILD_TESTS=OFF \
  -DGMF_BUILD_CLI=OFF \
  -DGMF_BUILD_PYTHON=OFF
cmake --build "$build_dir" --config Release

rm -rf "$package_dir"
mkdir -p "$package_dir"
cp "$build_dir/gmaxflow.js" "$build_dir/gmaxflow.wasm" "$package_dir/"
cp "$root/bindings/wasm/index.mjs" \
   "$root/bindings/wasm/index.d.ts" \
   "$root/bindings/wasm/package.json" \
   "$root/bindings/wasm/test_node.mjs" \
   "$package_dir/"
cp "$root/README.md" "$root/LICENSE" "$package_dir/"

echo "assembled $package_dir"
ls -l "$package_dir"

if [[ "${1:-}" == "--test" ]]; then
  (cd "$package_dir" && node test_node.mjs)
fi
