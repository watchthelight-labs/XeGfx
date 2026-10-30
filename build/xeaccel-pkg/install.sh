#!/bin/bash
# xeaccel package: installs XeGfxAccel.kext into /Library/Extensions and rebuilds the auxiliary kernel collection.
# <wtl> project: xegfx | author: watchthelight </wtl>
# Run as root from the package folder. uninstall.sh (same folder) takes it out again; both keep a backup of the KC.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); EXT=/Library/Extensions; KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
K=/System/Library/Kernels/kernel; KARG=(--allow-missing-kdk); [ -f "$K" ] && KARG+=(--kernel "$K")
KB=/System/Library/KernelCollections/BootKernelExtensions.kc; KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
die() { echo "xeaccel: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"
MODE=${XEACCEL_MODE:-install}
BK=/Library/XeGfx/backup-xeaccel-$(date +%Y%m%d-%H%M%S); mkdir -p "$BK"; [ -f "$KC" ] && cp -p "$KC" "$BK/"
[ -d "$EXT/XeGfxAccel.kext" ] && ditto "$EXT/XeGfxAccel.kext" "$BK/XeGfxAccel.kext"   # the kext it replaces, for a rollback
if [ "$MODE" = install ]; then
  (cd "$HERE" && shasum -a 256 --strict -c SHA256SUMS --quiet) || die "SHA256SUMS mismatch"
  rm -rf "$EXT/XeGfxAccel.kext"; ditto "$HERE/XeGfxAccel.kext" "$EXT/XeGfxAccel.kext"
  chown -R root:wheel "$EXT/XeGfxAccel.kext"; chmod -R 755 "$EXT/XeGfxAccel.kext"
else
  rm -rf "$EXT/XeGfxAccel.kext"
fi
NEW="$KC.xeaccel-new"; rm -f "$NEW"
kmutil create -n aux --volume-root / "${KARG[@]}" -B $KB -S $KS --repository "$EXT" -A "$NEW" -z > "$BK/kmutil.log" 2>&1 || die "kernel collection build failed (log $BK/kmutil.log; old KC kept)"
INS=$(kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null) || die "cannot inspect the new collection"
if [ "$MODE" = install ]; then printf '%s\n' "$INS" | grep -q 'org.watchthelight.XeGfxAccel' || die "XeGfxAccel missing from the new collection"
else printf '%s\n' "$INS" | grep -q 'org.watchthelight.XeGfxAccel' && die "XeGfxAccel still in the new collection"; fi
for k in XeGfx XeGfxIntelFB XeGfxAGDC; do [ ! -d "$EXT/$k.kext" ] || printf '%s\n' "$INS" | grep -q "org.watchthelight.$k" || die "$k missing from the new collection"; done
for k in NVRM NVRMFB NVAccel; do [ ! -d "$EXT/$k.kext" ] || printf '%s\n' "$INS" | grep -q "com.nullmoth.$k" || die "$k missing from the new collection"; done
mv -f "$NEW" "$KC"
echo "xeaccel: $MODE done (backup $BK); reboot to apply"
