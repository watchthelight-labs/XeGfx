#!/bin/bash
# Mesa ANV + Intel noop drm-shim for x86_64 macOS, no LLVM in the target (mesa_clc/vtn_bindgen2 come from build_host_clc.sh)
set -euo pipefail
. "$(dirname "$0")/env.sh"
export PATH=$X/host/bin:$PATH
export PKG_CONFIG_LIBDIR=$X/prefix/lib/pkgconfig:$X/sdk-pc
MIN=$MACOSX_DEPLOYMENT_TARGET
printf "[binaries]\ncmake = 'false'\nllvm-config = 'false'\n" > $X/native-target.ini
rm -rf $X/mesa/build
meson setup $X/mesa/build $X/mesa --native-file $X/native-target.ini --prefix=$X/prefix --libdir=lib \
  -Dbuildtype=release -Db_ndebug=true \
  -Dc_args=-mmacosx-version-min=$MIN -Dcpp_args=-mmacosx-version-min=$MIN -Dobjc_args=-mmacosx-version-min=$MIN \
  -Dc_link_args=-mmacosx-version-min=$MIN -Dcpp_link_args=-mmacosx-version-min=$MIN \
  -Dvulkan-drivers=intel -Dgallium-drivers= -Dplatforms= -Dtools=drm-shim -Dllvm=disabled \
  -Dmesa-clc=system -Dprecomp-compiler=auto \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dglvnd=disabled -Dgbm=disabled \
  -Dvulkan-layers= -Dbuild-tests=false -Dexpat=enabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false -Dvideo-codecs= ${EXTRA_MESON:-}
ninja -C $X/mesa/build
meson install -C $X/mesa/build
