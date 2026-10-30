# xkpi

Overlay for the XeGfx build (`XEGFX_OVERLAY`, see `harness/score.py`): replacements for drm-kmod files
that are GPL-only, so the kext contains no GPL-only code (Legion I-097), and LinuxKPI fixes not yet in FreeBSD.

| Path | Origin | Licence |
|---|---|---|
| `include/linux/dma-buf.h`, `dma-fence-unwrap.h`, `sync_file.h`, `src/dmabuf/nmx_dma_buf.c`, `nmx_dma_fence_unwrap.c` | NullMoth, nvidia-macos-driver commit bf2dee3, `gpu-port/xkpi/override/` (audited: Hackintosh `research/repository-audits/nullmoth-gpu-port.md`) | PolyForm Noncommercial 1.0.0 |
| `include/uapi/linux/sync_file.h` | Linux v6.14 | GPL-1.0+ WITH Linux-syscall-note (interface header) |
| `include/drm/drm_format_helper.h` | written for this fork | MIT |
| `exclude.txt` | written for this fork | MIT |
| `src/lkpi/linux_schedule.c` | FreeBSD `sys/compat/linuxkpi/common/src/linux_schedule.c` (freebsd-src d2145e9) with one change: `woken_wake_function` sets `TASK_WAKING` under the channel lock, so a wake between a waiter's condition check and `schedule_timeout()` is not lost (GuC replies timed out). The bug and fix were found by [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) (ROADMAP Phase 4, `override/src/linux_schedule.c`) | BSD-2-Clause, as the FreeBSD file |
| `src/lkpi/linux_page.c` | FreeBSD `sys/compat/linuxkpi/common/src/linux_page.c` (freebsd-src d2145e9) with `vmap()`, `linuxkpi_vmap_pfn()` and `vunmap()` rewritten to map through XeGfx's `xg_vm_vmap_pa()` at an address IOKit picks, since kva_alloc + pmap_qenter cannot overlay pages on Tahoe (Legion N-221) | BSD-2-Clause, as the FreeBSD file |
| `src/i915/intel_breadcrumbs.c` | drm-kmod `drivers/gpu/drm/i915/gt/intel_breadcrumbs.c` with two changes: its local includes say `gt/` because the file builds from here, and `signal_irq_work()` reads the next signaler before the loop body can drop the last reference to the current one. That is the upstream patch "drm/i915/gt: Fix ce use-after-free in signal_irq_work()" by Wentao Liang (linux-kernel, 2026-09-16), written with FreeBSD LinuxKPI's RCU list macros; [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) hit this use-after-free on Meteor Lake | MIT, as the i915 file |
| `src/i915/xg_i915_exec.c` | written for this fork: the XeGfx stage 4 GT check, the stage 5 per-engine request and the stage 6 store-dword check | MIT |
| `src/i915/xg_i915_map.c` | written for this fork: GEM mapping through ioctls, with the struct layout and ioctl numbers of NullMoth's Darwin libdrm patch so that patch works unchanged (an interface, no code taken) | MIT |
| `src/i915/xg_drm_sync.c`, `xg_drm_sync.h` | written for this fork: a GEM object's implicit fences to and from a syncobj, following Linux's dma-buf sync_file export and import, and `DRM_IOCTL_GET_PCIINFO` | MIT |
| `src/i915/intel_display_device.c` | drm-kmod `drivers/gpu/drm/i915/display/intel_display_device.c` with its own headers written as `display/` and one addition: the tunable `compat.linuxkpi.i915_xegfx_no_display` (default 1) sends the probe down i915's no-display path, because `disable_display` still sets up every connector (Legion N-223) | MIT, as the i915 file |
| `src/drm/xg_drm_nohelpers.c` | written for this fork: HDCP and writeback helpers answering "not supported" (drm-kmod's are GPL-only and excluded) | MIT |
| `src/i915/i915_gem_stolen.c` | drm-kmod `drivers/gpu/drm/i915/gem/i915_gem_stolen.c` with its own header include written as `gem/`, and `xg_stolen_start()`: the stolen allocator starts after the firmware's console image when pipe A was lit at probe. The lesson came from VedCloud's port and kitsuneislife/nmx-intel | MIT, as the i915 file |

# userland

| Path | Origin | Licence |
|---|---|---|
| `patches/mesa-macos.patch`, `patches/libdrm-macos.patch`, `tools/vkprobe.c`, `drmlist.c`, `fill.comp` | NullMoth, nvidia-macos-driver commit bf2dee3, `gpu-port/` (audited) | PolyForm Noncommercial 1.0.0 |
| `scripts/`, `BUILD.md`, `*.ini` | written for this fork | BSD 2-Clause |
| `patches/libdrm-xegfx.patch`, `patches/mesa-xegfx.patch` | written for this fork, applied after NullMoth's two patches: dma-buf and sync_file stand-ins in libdrm, Mesa ioctls and GEM mapping through libdrm | MIT |

Upstream sources the scripts fetch: [Mesa](https://gitlab.freedesktop.org/mesa/mesa) bffd919 (MIT) and [libdrm](https://gitlab.freedesktop.org/mesa/drm) b97cbde (MIT).
