#!/bin/bash
# build_nmintelfb.sh: NMIntelFB.kext (Intel built-in panel framebuffer, phase 1) as one translation unit.
# <wtl> project: xegfx | author: watchthelight </wtl>
#   SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk build/build_nmintelfb.sh
#   -> $OUTROOT/NMIntelFB/NMIntelFB.kext (OUTROOT default ~/src/XeGfx-build/kexts)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
OUT=${OUTROOT:-$HOME/src/XeGfx-build/kexts}/NMIntelFB
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$K" ] || { echo "STOP: no Kernel.framework headers in $SDK"; exit 1; }
SRC="$REPO/kexts/NMIntelFB"
rm -rf "$OUT/NMIntelFB.kext"; mkdir -p "$OUT/NMIntelFB.kext/Contents/MacOS"
FLAGS=(-arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K" -I"$SRC"
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
       -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -Wall -Werror -Wno-unused-parameter -g0
       -Wno-deprecated-declarations -Wno-inconsistent-missing-override -Wno-overloaded-virtual -Wno-unused-function -Wno-sign-conversion)
xcrun clang++ "${FLAGS[@]}" -c "$SRC/intelfb.cpp" -o "$OUT/intelfb.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/intelfb.o" -o "$OUT/NMIntelFB.kext/Contents/MacOS/NMIntelFB"
cp "$SRC/Info.plist" "$OUT/NMIntelFB.kext/Contents/Info.plist"
codesign -s - -f "$OUT/NMIntelFB.kext" >/dev/null 2>&1 || true
echo "NMIntelFB $(shasum -a 256 "$OUT/NMIntelFB.kext/Contents/MacOS/NMIntelFB" | cut -c1-12) $(stat -f %z "$OUT/NMIntelFB.kext/Contents/MacOS/NMIntelFB") B -> $OUT/NMIntelFB.kext"
