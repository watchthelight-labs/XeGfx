#!/bin/bash
set -euo pipefail
. "$(dirname "$0")/env.sh"
export PKG_CONFIG_LIBDIR=$X/prefix/lib/pkgconfig
rm -rf $X/drm/build
meson setup $X/drm/build $X/drm --prefix=$X/prefix --libdir=lib -Dbuildtype=release \
  -Dc_args=-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Dc_link_args=-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET \
  -Dintel=disabled -Damdgpu=disabled -Dradeon=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Domap=disabled \
  -Dexynos=disabled -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled \
  -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dtests=false -Dinstall-test-programs=false
ninja -C $X/drm/build
meson install -C $X/drm/build
