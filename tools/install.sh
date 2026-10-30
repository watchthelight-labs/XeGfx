#!/bin/bash
# Installs out/XeGfx.kext into /Library/Extensions and rebuilds the auxiliary kernel collection beside the old one,
# keeping a backup and checking that every third-party kext of the old collection is still in the new one.
# Run as root. XEGFX_MODE=uninstall removes it again. The kext loads idle; sysctl debug.xegfx_start=1 starts it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); EXT=/Library/Extensions; KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
K=/System/Library/Kernels/kernel; KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
KB=/System/Library/KernelCollections/BootKernelExtensions.kc; KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
die() { echo "xegfx: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"
MODE=${XEGFX_MODE:-install}
BK=/Library/XeGfx/backup-$(date +%Y%m%d-%H%M%S); mkdir -p "$BK"; [ -f "$KC" ] && cp -p "$KC" "$BK/"
OLD=$(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null | awk -F'\t' '$1 ~ /^[a-z]+\.[A-Za-z0-9.-]+$/ && $1 !~ /^com\.apple\./ && $1 != "org.watchthelight.XeGfx" && $1 != "org.watchthelight.XeGfxAccel" && $1 != "com.nullmoth.NMIntelFB" && $1 != "org.watchthelight.XeGfxAGDC" {print $1}' || true)
# the Metal accelerator over XeGfx (build/build_xegfxaccel.sh) and ANV for the Metal plugin; both optional
ACC=${XEGFX_ACCEL:-$HOME/src/XeGfx-build/kexts/XeGfxAccel/26/XeGfxAccel.kext}
[ -n "${SUDO_USER:-}" ] && ACC=${XEGFX_ACCEL:-$(eval echo "~$SUDO_USER")/src/XeGfx-build/kexts/XeGfxAccel/26/XeGfxAccel.kext}
XU=${XEGFX_USERLAND:-$(eval echo "~${SUDO_USER:-root}")/src/xe-userland/prefix/lib}; GB=/Library/GPUBundles/nvmtl
if [ "$MODE" = install ]; then
  rm -rf "$EXT/XeGfx.kext.new"; ditto "$ROOT/out/XeGfx.kext" "$EXT/XeGfx.kext.new"
  chown -R root:wheel "$EXT/XeGfx.kext.new"; chmod -R 755 "$EXT/XeGfx.kext.new"
  rm -rf "$EXT/XeGfx.kext"; mv "$EXT/XeGfx.kext.new" "$EXT/XeGfx.kext"
  # NMIntelFB takes the built-in panel at boot so the accelerator above can drive it (X6); optional
  IFB=${XEGFX_INTELFB:-$(dirname "$(dirname "$(dirname "$ACC")")")/NMIntelFB/NMIntelFB.kext}
  if [ -d "$IFB" ]; then
    rm -rf "$EXT/NMIntelFB.kext.new"; ditto "$IFB" "$EXT/NMIntelFB.kext.new"
    chown -R root:wheel "$EXT/NMIntelFB.kext.new"; chmod -R 755 "$EXT/NMIntelFB.kext.new"
    rm -rf "$EXT/NMIntelFB.kext"; mv "$EXT/NMIntelFB.kext.new" "$EXT/NMIntelFB.kext"
  fi
  # the AppleGraphicsDeviceControl service IOPresentment needs to give the boot framebuffer a display pipe (N-249)
  AGDC=${XEGFX_AGDC:-$(dirname "$(dirname "$(dirname "$ACC")")")/XeGfxAGDC/XeGfxAGDC.kext}
  if [ -d "$AGDC" ]; then
    rm -rf "$EXT/XeGfxAGDC.kext.new"; ditto "$AGDC" "$EXT/XeGfxAGDC.kext.new"
    chown -R root:wheel "$EXT/XeGfxAGDC.kext.new"; chmod -R 755 "$EXT/XeGfxAGDC.kext.new"
    rm -rf "$EXT/XeGfxAGDC.kext"; mv "$EXT/XeGfxAGDC.kext.new" "$EXT/XeGfxAGDC.kext"
  fi
  if [ -d "$ACC" ]; then
    rm -rf "$EXT/XeGfxAccel.kext.new"; ditto "$ACC" "$EXT/XeGfxAccel.kext.new"
    chown -R root:wheel "$EXT/XeGfxAccel.kext.new"; chmod -R 755 "$EXT/XeGfxAccel.kext.new"
    rm -rf "$EXT/XeGfxAccel.kext"; mv "$EXT/XeGfxAccel.kext.new" "$EXT/XeGfxAccel.kext"
  fi
  if [ -f "$XU/libvulkan_intel.dylib" ] && [ -d "$GB" ]; then
    # copied beside, renamed into place (a cp over a mapped dylib kills the next loader); libdrm moves with ANV
    cp "$XU/libdrm.2.dylib" "$GB/libdrm.2.dylib.new"; cp "$XU/libvulkan_intel.dylib" "$GB/libvulkan_intel.dylib.new"
    install_name_tool -id "$GB/libdrm.2.dylib" "$GB/libdrm.2.dylib.new" 2>/dev/null
    install_name_tool -id "$GB/libvulkan_intel.dylib" -change "$XU/libdrm.2.dylib" "$GB/libdrm.2.dylib" \
      "$GB/libvulkan_intel.dylib.new" 2>/dev/null
    codesign -s - -f "$GB/libdrm.2.dylib.new" "$GB/libvulkan_intel.dylib.new" >/dev/null 2>&1
    mv -f "$GB/libdrm.2.dylib.new" "$GB/libdrm.2.dylib"; mv -f "$GB/libvulkan_intel.dylib.new" "$GB/libvulkan_intel.dylib"
    printf '{\n    "file_format_version": "1.0.1",\n    "ICD": {\n        "library_path": "%s",\n        "api_version": "1.4.363",\n        "library_arch": "64"\n    }\n}\n' \
      "$GB/libvulkan_intel.dylib" > "$GB/intel_icd.json"
  fi
  # the userspace probe the guard runs after the stages
  if [ -f "$ROOT/out/xgprobe" ]; then
    mkdir -p /usr/local/bin; install -m 755 -o root -g wheel "$ROOT/out/xgprobe" /usr/local/bin/xgprobe.new
    mv -f /usr/local/bin/xgprobe.new /usr/local/bin/xgprobe
  fi
else
  rm -f /usr/local/bin/xgprobe "$GB/libvulkan_intel.dylib" "$GB/libdrm.2.dylib" "$GB/intel_icd.json"
  rm -rf "$EXT/XeGfx.kext" "$EXT/XeGfxAccel.kext" "$EXT/NMIntelFB.kext" "$EXT/XeGfxAGDC.kext"
fi
NEW="$KC.xegfx-new"; rm -f "$NEW"
kmutil create -n aux --volume-root / "${KARG[@]}" -B $KB -S $KS --repository "$EXT" -A "$NEW" -z > "$BK/kmutil.log" 2>&1 \
  || die "collection build failed (log $BK/kmutil.log; old collection kept)"
INS=$(kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null) || die "cannot inspect the new collection"
if [ "$MODE" = install ]; then printf '%s\n' "$INS" | grep -q '^org.watchthelight.XeGfx[[:space:]]' || die "XeGfx missing from the new collection"
  [ -d "$EXT/XeGfxAccel.kext" ] && { printf '%s\n' "$INS" | grep -q 'org.watchthelight.XeGfxAccel' || die "XeGfxAccel missing from the new collection"; }
  [ -d "$EXT/XeGfxAGDC.kext" ] && { printf '%s\n' "$INS" | grep -q 'org.watchthelight.XeGfxAGDC' || die "XeGfxAGDC missing from the new collection"; }
else printf '%s\n' "$INS" | grep -q '^org.watchthelight.XeGfx[[:space:]]' && die "XeGfx still in the new collection"; fi
for b in $OLD; do printf '%s\n' "$INS" | grep -q "^$b[[:space:]]" || die "$b was in the old collection but is missing from the new one"; done
mv -f "$NEW" "$KC"
echo "xegfx: $MODE done (backup $BK, kept: $(echo $OLD | wc -w | tr -d ' ') kexts); reboot to apply"
