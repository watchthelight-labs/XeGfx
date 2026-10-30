#!/bin/bash
# build_xegfxaccel.sh: XeGfxAccel.kext, the Metal accelerator over XeGfx, from kexts/XeGfxAccel/accel/nvrm-accel.cpp
# (NullMoth's NMXAccel built with NMX_XEGFX) for macOS 26 into $OUTROOT/XeGfxAccel/26/XeGfxAccel.kext.
# The RM -D set and the NVIDIA headers it compiles against come from kext_flags.sh.
# Environment: OGKM (open-gpu-kernel-modules 615.78.08), OUTROOT (default ~/src/XeGfx-build/kexts).
# <wtl> project: xegfx | author: watchthelight </wtl>
set -uo pipefail
. "$(dirname "$0")/kext_flags.sh"
SRC="$REPO/kexts/XeGfxAccel"; name=XeGfxAccel; out="$OUTROOT/$name/26"

ABI=$(sed -n 's/^#define NVRM_VRAM_ABI_VERSION \([0-9]*\)u.*/\1/p' "$SRC/nvrm_vram_abi.h")
[ -n "$ABI" ] || { echo "cannot read NVRM_VRAM_ABI_VERSION"; exit 1; }
rm -rf "$out"; mkdir -p "$out/$name.kext/Contents/MacOS"
$CXX "${KFLAGS[@]}" -I"$SRC" "${RMINC[@]}" "${KMSINC[@]}" "${RMDEFS[@]}" -DNMX_ACCEL -DNMX_XEGFX -DNM_TAHOE \
    -I"$SRC/accel" -I"$SRC/accel/iofam" -I"$SRC/accel/re" \
    -c "$SRC/accel/nvrm-accel.cpp" -o "$out/accel.o" 2>"$out/accel.err" || { grep error "$out/accel.err" | head; exit 1; }
$CXX "${LDFLAGS[@]}" "$out/accel.o" -o "$out/$name.kext/Contents/MacOS/$name" 2>"$out/accelld.err" || { head "$out/accelld.err"; exit 1; }
cp "$SRC/accel/xe/Info.plist" "$out/$name.kext/Contents/Info.plist"
printf '26  '; kext_report "$out/$name.kext/Contents/MacOS/$name"
echo "built from $(git -C "$REPO" rev-parse --short HEAD) (VRAM ABI $ABI)"
