#!/bin/bash
# build_xegfxagdc.sh: XeGfxAGDC.kext, kexts/XeGfxAGDC/nvrm-agdc.cpp built with -DNMX_XEGFX (the AppleGraphicsDeviceControl
# service IOPresentment needs on the Intel GPU before WindowServer gives its framebuffer a display pipe), into
# $OUTROOT/XeGfxAGDC (default ~/src/XeGfx-build/kexts). Same command line as build/build_agdc.sh.
# <wtl> project: xegfx | author: watchthelight </wtl>
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); D="$REPO/kexts/XeGfxAGDC"
B="${OUTROOT:-$HOME/src/XeGfx-build/kexts}/XeGfxAGDC"; rm -rf "$B"; mkdir -p "$B/XeGfxAGDC.kext/Contents/MacOS"
SDK="$(xcrun --show-sdk-path)"; KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
xcrun clang++ -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$D" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -DNMX_XEGFX -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -Wall -Wno-unused-parameter \
  -Wno-inconsistent-missing-override -Wno-unused-function -O2 \
  -c "$D/nvrm-agdc.cpp" -o "$B/k1.o" 2>"$B/cc.err" || { grep error "$B/cc.err" | head; exit 1; }
xcrun clang++ -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext "$B/k1.o" \
  -o "$B/XeGfxAGDC.kext/Contents/MacOS/XeGfxAGDC" 2>"$B/ld.err" || { head "$B/ld.err"; exit 1; }
cp "$D/xe/Info.plist" "$B/XeGfxAGDC.kext/Contents/Info.plist"
echo "XeGfxAGDC $(md5 -q "$B/XeGfxAGDC.kext/Contents/MacOS/XeGfxAGDC") $(dwarfdump --uuid "$B/XeGfxAGDC.kext/Contents/MacOS/XeGfxAGDC" | awk '{print $2}')"
