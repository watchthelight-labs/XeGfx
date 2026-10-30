#!/bin/bash
# Builds XeGfx.kext (Intel Xe) into out/: every drm/i915/LinuxKPI object, then the link with the
# KPI, shadow and kcabi link checks. Sources: ~/src/xe-src (linux, freebsd, drm-kmod), firmware ~/src/xe-src/linux-firmware.
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"; X="$R"
export XEGFX_OVERLAY="$R/xkpi"
python3 "$X/harness/score.py" --obj > "$X/out/score.log" 2>&1 || { tail -20 "$X/out/score.log"; exit 1; }
# a source that did not compile would be linked as panicking stubs (N-223 died in one), so stop here instead
read -r ok all < <(sed -nE 's/^total +([0-9]+)\/ *([0-9]+).*/\1 \2/p' "$X/out/score.log")
[ -n "${ok:-}" ] && [ "$ok" = "$all" ] || { echo "STOP: ${ok:-?}/${all:-?} sources compiled"; grep -A3 "first errors" "$X/out/score.log"; exit 1; }
bash "$X/tools/link.sh"
