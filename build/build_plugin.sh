#!/bin/bash
# NvGfx: build the Metal plugin (NVMTLDriver) from this checkout, the way NullMoth's build/build_plugin.sh does it,
# but with repo-relative sources. Needs only the Command Line Tools and Khronos Vulkan-Headers.
#   XEGFX_DEPS (default ~/src/deps) holds Vulkan-Headers; output goes to XEGFX_OUT (default ~/src/XeGfx-build/plugin).
#   RELEASE=1 matches the shipped release build (-DNVMTL_RELEASE=1, diagnostics compiled out).
# <wtl> project: xegfx | author: watchthelight </wtl>
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${XEGFX_DEPS:-$HOME/src/deps}
O=${XEGFX_OUT:-$HOME/src/XeGfx-build}/plugin
mkdir -p "$O"
RDEF=""; [ -n "${RELEASE:-}" ] && RDEF="-DNVMTL_RELEASE=1"
cd "$R/plugin"
xcrun clang -arch x86_64 -bundle -fobjc-arc -O1 $RDEF -Wno-protocol -Wno-incomplete-implementation \
  -mmacosx-version-min=15.0 -I"$DEPS/Vulkan-Headers/include" -o "$O/NVMTLDriver" NVMTLDevice.m \
  -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation 2>"$O/NVMTLDriver.err" \
  || { echo "compile failed"; grep -A2 ': error:' "$O/NVMTLDriver.err" | head -30; exit 5; }
codesign -f -s - "$O/NVMTLDriver" 2>/dev/null
echo "NVMTLDriver $(git -C "$R" rev-parse --short HEAD) sha $(shasum -a256 "$O/NVMTLDriver" | cut -c1-12) warnings $(grep -c ': warning:' "$O/NVMTLDriver.err" || true)"
echo "$O/NVMTLDriver"
