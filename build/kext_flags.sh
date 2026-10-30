#!/bin/bash
# kext_flags.sh: sourced by build_nvrmfb.sh and build_nvaccel.sh. Sets SDK, OGKM, CFLAGS and LDFLAGS for the
# NVIDIA-facing kexts (NVRMFB, NVAccel, NMXAccel), the same command line build/accel_build.sh uses upstream.
#
# Upstream reads the RM -D set from NullMoth's private $OGKM/src/nvidia/_out/Darwin_x86_64/compile_cmds.sh (first line,
# minus -DNVRM and -D_LANGUAGE_C). That file is not published. It is the command line the RM Makefile builds on Darwin,
# so the set below is rebuilt from open-gpu-kernel-modules (610.57.04 first, 615.78.08 for 2.5.x): src/nvidia/Makefile (the CFLAGS += -D lines) plus
# utils.mk (TARGET_ARCH x86_64 adds NV_X86_64 and NV_ARCH_BITS=64; TARGET_OS Darwin adds no OS define; release build,
# so no DEBUG or DEVELOP). The first RM source (generated/g_access_cntr_buffer_nvoc.c) gets no per-file defines.
# The kext sources use none of these directly; they reach only the NVIDIA headers the kexts include.
# <wtl> project: xegfx | author: watchthelight </wtl>

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OGKM=${OGKM:-$HOME/src/deps/open-gpu-${NVRM_TAG:-615.78.08}}
OUTROOT=${OUTROOT:-$HOME/src/XeGfx-build/kexts}
SDK=${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}   # SDKROOT pins the SDK (the docs say 15.5); CLT 26 made 26.5 the default
KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
NV="$OGKM/src/nvidia"; KMS="$OGKM/src/nvidia-modeset"
[ -d "$NV/generated" ] || { echo "OGKM=$OGKM is not an open-gpu-kernel-modules tree"; exit 1; }

RMINC=( -I"$NV/arch/nvalloc/unix/include" -I"$NV/arch/nvalloc/common/inc" -I"$NV/arch/nvalloc/common/inc/gsp"
  -I"$OGKM/src/common/sdk/nvidia/inc" -I"$OGKM/src/common/sdk/nvidia/inc/hw" -I"$OGKM/src/common/inc"
  -I"$OGKM/src/common/shared/inc" -I"$NV/inc" -I"$NV/inc/os" -I"$NV/inc/kernel" -I"$NV/kernel/inc" -I"$NV/interface"
  -I"$OGKM/src/common/nvlink/interface" -I"$OGKM/src/common/inc/swref" -I"$OGKM/src/common/inc/swref/published"
  -I"$NV/inc/libraries" -I"$NV/src/libraries" -I"$NV/generated" -I"$NV/src/mm/uvm/interface"
  -I"$OGKM/src/common/uproc/os/libos-v2.0.0/include" -I"$OGKM/src/common/uproc/os/common/include"
  -I"$OGKM/src/common/inc/displayport" -I"$OGKM/src/common/nvlink/inband/interface" )
KMSINC=( -I"$KMS/os-interface/include" -I"$KMS/kapi/interface" -I"$KMS/kapi/include" -I"$OGKM/src/common/unix/nvidia-push/interface"
  -I"$KMS/interface" -I"$KMS/include" -I"$OGKM/src/common/unix/common/inc" -I"$OGKM/src/common/modeset"
  -I"$OGKM/src/common/unix/common/utils/interface" )

# utils.mk (x86_64) then src/nvidia/Makefile, in that order, as compile_cmds.sh would list them
RMDEFS=( -DNV_X86_64 -DNV_ARCH_BITS=64
  "-DMBEDTLS_USER_CONFIG_FILE=<nvrm_mbedtls_config.h>"
  -D__NO_CTYPE -DLOCK_VAL_ENABLED=0 -DPORT_ATOMIC_64_BIT_SUPPORTED=1 -DPORT_IS_KERNEL_BUILD=1 -DPORT_IS_CHECKED_BUILD=0
  -DPORT_MODULE_atomic=1 -DPORT_MODULE_core=1 -DPORT_MODULE_cpu=1 -DPORT_MODULE_crypto=1 -DPORT_MODULE_debug=1
  -DPORT_MODULE_memory=1 -DPORT_MODULE_safe=1 -DPORT_MODULE_string=1 -DPORT_MODULE_sync=1 -DPORT_MODULE_thread=1
  -DPORT_MODULE_time=1 -DPORT_MODULE_util=1 -DPORT_MODULE_example=0 -DPORT_MODULE_mmio=0
  -DRS_STANDALONE=0 -DRS_STANDALONE_TEST=0 -DRS_COMPATABILITY_MODE=1 -DRS_PROVIDES_API_STATE=0 -DNV_CONTAINERS_NO_TEMPLATES
  -DINCLUDE_NVLINK_LIB -DINCLUDE_NVSWITCH_LIB
  -DNV_PRINTF_STRINGS_ALLOWED=1 -DNV_ASSERT_FAILED_USES_STRINGS=1 -DPORT_ASSERT_FAILED_USES_STRINGS=1 )

# kernel flags exactly as build/accel_build.sh (Apple clang from the Command Line Tools)
KFLAGS=( -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KHDR"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 )
LDFLAGS=( -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext )
CXX=${CXX:-xcrun clang++}

kext_report() {   # kext_report <binary>
    echo "$(basename "$1") $(md5 -q "$1") $(stat -f %z "$1") B  ($($CXX --version | head -1))"
}
