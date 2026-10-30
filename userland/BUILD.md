# xe-userland: Mesa ANV and libdrm for x86_64 macOS

Built 2026-10-21 on the Legion (macOS Tahoe 26.7.1 25G241, Core Ultra 9 275HX), native x86_64, nothing installed outside `~/src/xe-userland` and the new venv `~/.venvs/mesa`. Result: the build works, and all 28 simulated Intel chips create a device and compile the compute pipeline through the Darwin noop drm-shim.

## Sources

| Tree | Commit | Patch | Patch sha256 (first 16) | Status |
|---|---|---|---|---|
| libdrm (`drm/`) | `b97cbde15c5c3abfe44d78e8f57139e50f612fec` (2.4.134, "amdgpu: add missing marketing names and minor name fixes") | `NvGfx-wt-n67/gpu-port/patches/libdrm-macos.patch` | `a82d4d40b7090926` | `git apply --check` clean, applied, 3 files |
| Mesa (`mesa/`) | `bffd91947efc1932da99aca69fc540d6d4201b0c` (26.3.0-devel, "nvk: Use meta gfx for depth copies", 2026-10-10) | `NvGfx-wt-n67/gpu-port/patches/mesa-macos.patch` | `5ebeef3a0466e7d3` | `git apply --check` clean, applied, 12 files (11 modified + `drm_shim_darwin.c`) |
| Vulkan-Headers, Vulkan-Loader | tag `v1.4.328` (Khronos GitHub) | none | | test loader for vkprobe only |

Both patches applied with no rejects. No source change was needed on top of them. Mesa was fetched shallow (depth 1) by full SHA, which was resolved from the short one through the GitLab API.

## Toolchain

Apple clang 21.0.0 (CLT), `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk`, MacPorts meson 1.12.1, ninja, glslang 16.6.0, cmake, LLVM 20.1.8 (host tools only), SPIRV-LLVM-Translator from `~/.local/nvgfx-llvm/lib/pkgconfig` (host tools only). Python 3.14.8 venv `~/.venvs/mesa` (from MacPorts python3.14) with mako 1.4.3, PyYAML 6.0.3, packaging 26.3; its `bin` is first in PATH so meson's python finds mako.

## Build steps (scripts in `scripts/`, logs in `logs/`)

1. `scripts/env.sh`: PATH, SDKROOT pin, `CC=/usr/bin/clang`, deployment target.
2. `scripts/build_libdrm.sh` (`logs/libdrm.log`): prefix `~/src/xe-userland/prefix`, `--libdir=lib`, release, every driver off, `-Dintel=disabled` (see deviations), tests, man pages, cairo, valgrind off. `PKG_CONFIG_LIBDIR` limited to the prefix.
3. `scripts/build_host_clc.sh` (`logs/host-clc.log`): `mesa_clc` and `vtn_bindgen2` from the same patched Mesa tree, LLVM 20 shared, installed to `~/src/xe-userland/host/bin`. Options as in NvGfx `build_nvk.sh` step 1 (`-Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dllvm=enabled -Dshared-llvm=enabled`, all drivers off), native file with `cmake = 'false'`. These run only on the build machine.
4. `scripts/build_mesa.sh` (`logs/mesa.log`), the target build:

```
meson setup mesa/build mesa --native-file native-target.ini --prefix=$X/prefix --libdir=lib \
  -Dbuildtype=release -Db_ndebug=true \
  -D{c,cpp,objc}_args=-mmacosx-version-min=14.4 -D{c,cpp}_link_args=-mmacosx-version-min=14.4 \
  -Dvulkan-drivers=intel -Dgallium-drivers= -Dplatforms= -Dtools=drm-shim -Dllvm=disabled \
  -Dmesa-clc=system -Dprecomp-compiler=auto \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dglvnd=disabled -Dgbm=disabled \
  -Dvulkan-layers= -Dbuild-tests=false -Dexpat=enabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false -Dvideo-codecs=
```

`native-target.ini` sets `cmake = 'false'` and `llvm-config = 'false'`; `PATH` has `host/bin` first so `-Dmesa-clc=system` finds `mesa_clc` and `vtn_bindgen2`; `PKG_CONFIG_LIBDIR=$X/prefix/lib/pkgconfig:$X/sdk-pc`. `-Dllvm=disabled` works: ANV needs LLVM only for its precompiled OpenCL helper kernels (`src/intel/shaders`, built by `mesa_clc` + `vtn_bindgen2`), and no `intel_clc` is needed at this commit. The target dylib links no LLVM. 62 compiler warnings, none fatal.

5. `scripts/build_loader.sh` (`logs/loader.log`): Khronos headers and loader into `loader-prefix/`, for vkprobe only.
6. vkprobe and drmlist in `tools/`: `fill.comp` compiled with `glslangValidator -V --target-env vulkan1.3`, checked with `spirv-val`, turned into `fill_spv.h` with `xxd -i -n fill_spv`; `vkprobe` linked against `loader-prefix/lib/libvulkan.dylib`, `drmlist` against the new libdrm (built, not run: there are no `/dev/dri` nodes yet).

## Deviations and fixes

No diffs to Mesa or libdrm sources beyond the two NullMoth patches. Build-side changes:

- libdrm `-Dintel=disabled` instead of `enabled`. `-Dintel` builds `libdrm_intel`, the legacy GEM helper, which hard-requires libpciaccess (no Darwin backend, not installed). Mesa uses `libdrm_intel` only for gallium i915 (`meson.build:777-779`); ANV and the noop shim use core libdrm only. So the core `libdrm.2.dylib` is all ANV needs.
- libdrm b97cbde has no `freedreno` option; it was dropped from the option list.
- expat from the macOS SDK. With `-Dexpat=disabled`, `libintel_decoder_gen.a` has no sources and Apple `ar` refuses an empty archive (`ar: no archive members specified`). Rather than pull in MacPorts expat, `sdk-pc/expat.pc` points at the SDK's `-lexpat` (2.7.1, `/usr/lib/libexpat.1.dylib`):

```
Name: expat
Version: 2.7.1
Libs: -lexpat
Cflags:
```

- ICD manifest for tests: `intel_test_icd.json` points at `prefix/lib/libvulkan_intel.dylib`.

## Minimum macOS target

- `-mmacosx-version-min=11.0` compiles and links (log kept as `logs/mesa-min11.0.log`), but the binary is not correct there: the patch's futex code in `src/util/futex.c` calls `os_sync_wait_on_address`, `os_sync_wait_on_address_with_timeout`, `os_sync_wake_by_address_any/all`, which are macOS 14.4 API and get weak-linked, so they would be NULL calls on older systems. `intel_measure.c` also uses `mkfifoat` (13.0, only with `INTEL_MEASURE`).
- Shipped build: Mesa at 14.4 (`minos 14.4`, no `-Wunguarded-availability` warnings left), libdrm at 11.0 (`minos 11.0`, no newer API used). Going below 14.4 would need a runtime fallback in `futex.c` (for example `__ulock_wait`, private API), not done.

## Outputs

| File | Size (bytes) | sha256 (first 16) | minos / sdk |
|---|---|---|---|
| `prefix/lib/libvulkan_intel.dylib` | 32,829,768 | `bb83266133d6d545` | 14.4 / 15.5 |
| `prefix/lib/libintel_noop_drm_shim.dylib` | 275,408 | `e65e5c39253ba92d` | 14.4 / 15.5 |
| `prefix/lib/libdrm.2.dylib` | 81,080 | `53f7f1588fcdf8d7` | 11.0 / 15.5 |

Also installed: `prefix/share/vulkan/icd.d/intel_icd.x86_64.json` (api_version 1.4.363), `prefix/share/drirc.d/00-{mesa,anv}-defaults.conf`, libdrm headers and `libdrm.pc`.

`otool -L libvulkan_intel.dylib`:

```
/Users/bash/src/xe-userland/prefix/lib/libvulkan_intel.dylib
/usr/lib/libSystem.B.dylib (current 1351.0.0)
/Users/bash/src/xe-userland/prefix/lib/libdrm.2.dylib (compat 2.0.0)
/usr/lib/libz.1.dylib (current 1.2.12)
/usr/lib/libexpat.1.dylib (current 8.0.0)
/usr/lib/libc++.1.dylib (current 1900.180.0)
```

The install names are absolute paths into this tree; a package will need `install_name_tool -id` and `-change` (or `@rpath`) for libdrm. The shim links only libdrm and libSystem.

### Undefined symbols (`nm -m -u libvulkan_intel.dylib`)

11,195 undefined in total: 261 from libSystem, libc++, libz, libexpat; 58 from libdrm (`drmIoctl`, `drmGetDevices2`, `drmMode*`, `drmCrtc*Sequence`, and so on); **10,876 weak, flat-namespace, "dynamically looked up"**, none of them non-weak. By prefix: `anv_` 4085, `vk_` 1569, `wsi_` 742, `gfx9_`/`gfx11_`/`gfx12_` 648 each, `gfx125_`/`gfx20_`/`gfx30_`/`gfx35_` 634 each. These are the entrypoint-table slots for Vulkan commands a layer does not implement (for example `anv_AllocateCommandBuffers`, which `vk_common` covers), left undefined on purpose by the patch's `b_lundef=false` and resolved to NULL at load. Two things to keep in mind:

- Flat-namespace lookup means a same-named exported symbol in another image of the process would be bound instead of NULL. Mesa drivers keep these names hidden, so this is theoretical, but loading ANV and NVK in one process should be tested once.
- Exports: the three `vk_icd*` functions, plus 140 `gfx*_libanv_*`, `gfx*_write_*`, `gfx*_copy_data`/`set_data` symbols from the `vtn_bindgen2`-generated shader bindings, which are not hidden. On Linux the version script hides them; on Darwin nothing does. Harmless for function, but an `-exported_symbols_list` would be cleaner.

## Chip matrix

`scripts/vk_chip_matrix_intel.sh` is the Intel half of NullMoth's `vk_chip_matrix.sh` with this tree's paths. Each run uses `env -i`, `DYLD_INSERT_LIBRARIES=prefix/lib/libintel_noop_drm_shim.dylib`, `VK_ICD_FILENAMES=VK_DRIVER_FILES=intel_test_icd.json`, `MESA_SHADER_CACHE_DISABLE=true` (so every chip really compiles), and `INTEL_STUB_GPU_PLATFORM=<name>` or `INTEL_STUB_GPU_DEVICE_ID=<id>`. The loader log (`VK_LOADER_DEBUG=driver`) confirms only our dylib is loaded. Per-chip output is in `logs/matrix/`, the summary in `logs/chip-matrix.txt`.

| Chip | PCI ID | Device name | vkCreateDevice | Compute pipeline |
|---|---|---|---|---|
| Gen12 TGL | 0x9a49 | Iris Xe Graphics (TGL GT2) | OK | OK |
| Gen12 RKL | 0x4c8a | Graphics (RKL GT1) | OK | OK |
| Gen12 DG1 | 0x4905 | Iris Xe MAX Graphics (DG1) | OK | OK |
| Gen12 ADL | 0x4680 | UHD Graphics 770 (ADL-S GT1) | OK | OK |
| Gen12 RPL | 0xa780 | Graphics (RPL-S) | OK | OK |
| Gen12 RPL-P | 0xa7a0 | Iris Xe Graphics (RPL-P) | OK | OK |
| Xe-HPG DG2 | 0x5690 | Arc A770M Graphics (DG2) | OK | OK |
| Xe-HPG DG2 | 0x56a0 | Arc A770 Graphics (DG2) | OK | OK |
| Xe-LPG MTL | 0x7d60 | Graphics (MTL) | OK | OK |
| Xe-LPG MTL | 0x7dd5 | Graphics (MTL) | OK | OK |
| Xe-LPG MTL | 0x7d55 | Arc Graphics (MTL) | OK | OK |
| **ARL (Legion iGPU)** | **0x7d67** | Graphics (ARL) | OK | OK |
| ARL | 0x7d41 | Graphics (ARL) | OK | OK |
| ARL | 0x7d51 | Graphics (ARL) | OK | OK |
| Xe2 LNL | 0x64a0 | Graphics (LNL) | OK | OK |
| Xe2 BMG | 0xe202 | Graphics (BMG G21) | OK | OK |
| Xe3 PTL | 0xb080 | Arc B390 (PTL) | OK | OK |

Older chips also pass: SKL, BXT, KBL, GLK, CFL, WHL, CML, ICL, EHL, JSL. Total 28 of 28.

For 0x7D67 the compute shader really compiles to Xe ISA: `logs/arl-intel-debug-cs.txt` (`INTEL_DEBUG=cs`) shows "SIMD16 shader: 11 instructions ... 2 sends" with the `store.ugm.d32.a32` and EOT sends.

Limits of this proof: the noop shim fakes the **i915** uAPI for every chip (it prints a few unknown query/getparam ids, which ANV tolerates), so LNL, BMG and PTL pass here through ANV's i915 path, while on real hardware they need the xe KMD path. Nothing is submitted to a GPU; this proves the driver loads, picks the device info, creates a device and runs the brw compiler on macOS. It does not exercise the xe uAPI, `drm_mmap` through `NMX_DRM_IOCTL_MMAP`, or a real `/dev/dri` node.

## XeGfx patches (2026-10-22)

On top of NullMoth's two patches, for real XeGfx hardware: `patches/libdrm-xegfx.patch` (adds `xf86drm_xegfx.c`, the dma-buf and sync_file stand-ins, called first from `drmIoctl()`) and `patches/mesa-xegfx.patch` (`intel_ioctl()` through `drmIoctl()`, i915 GEM mappings through `drmNmxMmap`/`drmNmxMunmap` with a fallback to mmap(2) for drm-shim). Apply with `git apply` in `drm/` and `mesa/` after the NullMoth patches, then rerun `build_libdrm.sh` and the Mesa build. The noop drm-shim matrix still passes 28 of 28 chips with both applied.
