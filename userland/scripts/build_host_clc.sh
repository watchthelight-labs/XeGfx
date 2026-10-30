#!/bin/bash
# host tools for the Intel precompiled shaders: mesa_clc + vtn_bindgen2, same Mesa tree, LLVM 20 (build machine only)
set -euo pipefail
. "$(dirname "$0")/env.sh"
unset MACOSX_DEPLOYMENT_TARGET
LLVM_CONFIG=/opt/local/libexec/llvm-20/bin/llvm-config
LLVM_LIB=$($LLVM_CONFIG --libdir)
export PKG_CONFIG_PATH=$HOME/.local/nvgfx-llvm/lib/pkgconfig
printf "[binaries]\nllvm-config = '%s'\ncmake = 'false'\n" "$LLVM_CONFIG" > $X/native-llvm.ini
rm -rf $X/mesa/build-host
meson setup $X/mesa/build-host $X/mesa --native-file $X/native-llvm.ini --prefix=$X/host -Dbuildtype=release \
  -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dllvm=enabled -Dshared-llvm=enabled \
  -Dc_link_args=-Wl,-rpath,$LLVM_LIB -Dcpp_link_args=-Wl,-rpath,$LLVM_LIB \
  -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms= -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dglx=disabled -Dglvnd=disabled -Dvulkan-layers= -Dtools= \
  -Dbuild-tests=false -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false
ninja -C $X/mesa/build-host
meson install -C $X/mesa/build-host
$X/host/bin/mesa_clc --help >/dev/null
