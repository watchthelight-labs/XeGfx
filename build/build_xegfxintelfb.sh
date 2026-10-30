#!/bin/bash
# build_xegfxintelfb.sh: XeGfxIntelFB.kext (IONDRVFramebuffer with an Intel class name, I-108) as one translation unit.
# <wtl> project: xegfx | author: watchthelight </wtl>
#   SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk build/build_xegfxintelfb.sh
#   -> $OUTROOT/XeGfxIntelFB/XeGfxIntelFB.kext (OUTROOT default ~/src/XeGfx-build/kexts)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
OUT=${OUTROOT:-$HOME/src/XeGfx-build/kexts}/XeGfxIntelFB
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$K" ] || { echo "STOP: no Kernel.framework headers in $SDK"; exit 1; }
SRC="$REPO/kexts/XeGfxIntelFB"
rm -rf "$OUT/XeGfxIntelFB.kext"; mkdir -p "$OUT/XeGfxIntelFB.kext/Contents/MacOS"
FLAGS=(-arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K" -I"$SRC"
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
       -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -Wall -Werror -Wno-unused-parameter -g0
       -Wno-deprecated-declarations -Wno-inconsistent-missing-override -Wno-overloaded-virtual -Wno-unused-function -Wno-sign-conversion)
xcrun clang++ "${FLAGS[@]}" -c "$SRC/intelfb.cpp" -o "$OUT/intelfb.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/intelfb.o" -o "$OUT/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB"
cp "$SRC/Info.plist" "$OUT/XeGfxIntelFB.kext/Contents/Info.plist"
codesign -s - -f "$OUT/XeGfxIntelFB.kext" >/dev/null 2>&1 || true
echo "XeGfxIntelFB $(shasum -a 256 "$OUT/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB" | cut -c1-12) $(stat -f %z "$OUT/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB") B -> $OUT/XeGfxIntelFB.kext"
