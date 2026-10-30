#!/bin/bash
# vk_chip_matrix.sh (NullMoth gpu-port/tools), Intel half only, with this tree's paths: runs vkprobe once per simulated
# Intel GPU through the Darwin noop drm-shim, against only the freshly built libvulkan_intel.dylib.
X=$HOME/src/xe-userland
cd $X/logs || exit 1
mkdir -p matrix
SHIM=$X/prefix/lib/libintel_noop_drm_shim.dylib
ICD=$X/intel_test_icd.json
pass=0; fail=0
run() { # name env...
  local name=$1; shift
  local o=matrix/$name.txt
  env -i HOME=$HOME PATH=/usr/bin:/bin MESA_SHADER_CACHE_DISABLE=true "$@" DYLD_INSERT_LIBRARIES=$SHIM VK_ICD_FILENAMES=$ICD VK_DRIVER_FILES=$ICD \
    VK_LOADER_LAYERS_DISABLE='~all~' $X/tools/vkprobe >$o 2>&1; local rc=$?
  local line; line=$(grep -m1 "^device" $o | sed -E "s/^device 0: //; s/ \| API.*//")
  local dev; dev=$(grep -m1 "^device" $o | sed -nE 's/.*device (0x[0-9a-f]+).*/\1/p')
  if grep -q "vkCreateDevice: OK" $o && grep -q "compute pipeline: OK" $o; then pass=$((pass+1)); printf "%-10s %-7s OK    %s\n" "$name" "$dev" "$line"
  else fail=$((fail+1)); printf "%-10s %-7s FAIL  rc=%s dev=%s | %s\n" "$name" "$dev" "$rc" "$(grep -c vkCreateDevice:\ OK $o)" "$(grep -m1 -iE "error|fail|assert|unsupported|abort" $o | cut -c1-120)"; fi
}
for g in skl bxt kbl glk cfl whl cml icl ehl jsl tgl rkl dg1 adl rpl dg2 mtl arl lnl bmg ptl; do
  run $g INTEL_STUB_GPU_PLATFORM=$g
done
# explicit PCI IDs: the Legion's iGPU (ARL-HX 0x7D67) and other ARL / MTL parts
for id in 7d67 7d41 7d51 7dd5 7d55 a7a0 56a0; do
  run id-$id INTEL_STUB_GPU_DEVICE_ID=$id
done
echo "matrix: $pass chip(s) device+pipeline OK, $fail failed"
