#!/bin/bash
# NvGfx: build the shader translator dylib (libnvmtl_translate.dylib, Metal AIR -> SPIR-V; LGPL, metal2vulkan fork) from
# this checkout, like NullMoth's build/build_xlate.sh. Exports are checked against the shipped 2.1.0 dylib instead of
# the unpublished LIVE/xlate.exports list. Needs rustup stable (>= 1.87).
# <wtl> project: xegfx | author: watchthelight </wtl>
set -euo pipefail
export PATH=$HOME/.cargo/bin:/usr/bin:/bin:/usr/sbin:/sbin
R=$(cd "$(dirname "$0")/.." && pwd)
X=$R/translator
O=${XEGFX_OUT:-$HOME/src/XeGfx-build}/xlate; mkdir -p "$O"
REF=${NVGFX_REF_PKGROOT:-$HOME/Hackintosh/tooling/quarantine/nullmoth-release-2.3.5/x/pkgroot}
export RUSTFLAGS="--remap-path-prefix=$X=/src --remap-path-prefix=$HOME/.cargo=/cargo --remap-path-prefix=$HOME=/build"
cd "$X"
cargo build --release --manifest-path wrapper/Cargo.toml --target-dir "$O/target" > "$O/cargo.log" 2>&1 || { echo "cargo build failed: $O/cargo.log"; tail -20 "$O/cargo.log"; exit 4; }
D=$O/libnvmtl_translate.dylib
cp "$O/target/release/libnvmtl_translate.dylib" "$D"; strip -S "$D"
install_name_tool -id @rpath/libnvmtl_translate.dylib "$D" 2>/dev/null; codesign -f -s - "$D" 2>/dev/null
SHIP=$(find "$REF" -name libnvmtl_translate.dylib | head -1)
gone=$(comm -23 <(nm -gU "$SHIP" | awk '{print $3}' | sort) <(nm -gU "$D" | awk '{print $3}' | sort) | wc -l | tr -d ' ')
for p in NVMTL_NO_BINDLESS_ALL NVMTL_NO_TIER2; do strings -a "$D" | grep -q -- "$p" || echo "MISSING marker $p"; done
echo "libnvmtl_translate $(git -C "$R" rev-parse --short HEAD) sha $(shasum -a256 "$D" | cut -c1-12) exports-missing-vs-2.1.0 $gone"
echo "$D"
