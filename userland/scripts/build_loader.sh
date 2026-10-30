#!/bin/bash
# Khronos Vulkan loader for running vkprobe (test only, installed into $X/loader-prefix)
set -euo pipefail
. "$(dirname "$0")/env.sh"
export MACOSX_DEPLOYMENT_TARGET=14.4
L=$X/loader-prefix
cmake -S $X/Vulkan-Headers -B $X/Vulkan-Headers/build -G Ninja -DCMAKE_INSTALL_PREFIX=$L
cmake --install $X/Vulkan-Headers/build
cmake -S $X/Vulkan-Loader -B $X/Vulkan-Loader/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$L \
  -DVULKAN_HEADERS_INSTALL_DIR=$L -DCMAKE_PREFIX_PATH=$L -DBUILD_TESTS=OFF -DCMAKE_OSX_SYSROOT=$SDKROOT
ninja -C $X/Vulkan-Loader/build
cmake --install $X/Vulkan-Loader/build
