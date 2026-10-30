# How XeGfx works

This is the long version of the README, for anyone who wants to change the code or check that it does what it claims.

## The layers

```
 Metal apps, WindowServer
 NvGfx Metal plugin (Metal -> Vulkan)          userspace
 Mesa ANV  +  libdrm  +  a transport shim
 ------------------------------------------------------------
 DRM device nodes (devfs clone nodes)           kernel, XeGfx.kext
 drm core, TTM, dma-buf, i915                   unchanged drm-kmod sources
 LinuxKPI                                       unchanged FreeBSD sources
 FreeBSD kernel API on XNU                      shim/ (this repository)
 XNU: IOKit, Mach, BSD KPIs
```

Everything above the shim is someone else's code, compiled unchanged, on purpose: i915 is hundreds of thousands of lines that Intel and the Linux community keep current, and FreeBSD already proved the Linux driver can run on a different kernel through LinuxKPI. What XeGfx adds is the bottom layer, the FreeBSD kernel interfaces that LinuxKPI and drm-kmod call, implemented on what a macOS kext is allowed to use.

## The shim, area by area

Each area has its own design note in `docs/`:

- **sched** (`xg_sched.md`): kernel threads on XNU threads, a thread registry so `curthread` works, `sched_pin` and critical sections on a spin lock (no CPU binding, it is not a public KPI), ck_epoch, kernel FPU sections.
- **vm** (`xg_vm.md`): real FreeBSD `vm_page` arrays, backed by IOKit allocations and kept in a pctrie. There is no direct map on macOS, so physical-to-virtual lookups go through the IOKit mappings the shim made. `kva_alloc` takes from a pageable reservation.
- **pci** (`xg_pci.md`): a newbus tree (`root0`, `pcib0`, `pci0`) built over every IOPCIDevice, config space and BARs through IOPCIDevice, MSI through IOInterruptEventSource. LinuxKPI's `vgapci` and `drmn` attach to it the way they do on FreeBSD.
- **dma** (`xg_dma.md`): `bus_dma` tags on IOKit memory, the stolen-memory probe, GTT entry points, ACPI notifiers, the backlight registry.
- **fd** (`xg_fd.md`): character devices as XNU devfs clone nodes, so every `open` of a DRM node gets its own file, as LinuxKPI expects; private file tables; eventfd.
- **misc** (`xg_misc.md`): a private sysctl tree, tunables read from boot-args or from `debug.xegfx_env`, sbuf, counters, task queue groups, firmware loading from the kext's `Resources`, select and knotes.

Rules the shim keeps:

- Public KPIs only. No `com.apple.kpi.private`, no unexported kernel symbols. `tools/kpi_check.py` enforces it at link time.
- No shadowing of kernel symbols. The FreeBSD and Linux code is linked statically into the kext, so any function it defines with the same name as a kernel export captures the kext's own XNU-side callers. That is how the first hardware tests died: `_start` registered an XNU sysctl through FreeBSD's `sysctl_register_oid` and dereferenced NULL. `shim/include/xg_bsdnames.h` renames the FreeBSD copies for every FreeBSD and Linux compilation unit, and `tools/shadow_check.py` fails the link if a new clash appears.
- No GPL-only code in the shipping build. The harness records every header each object includes and refuses (with an overlay set) to build if any compiled source or included header is GPL-only. The NvGfx fork's overlay supplies replacements for drm-kmod's few GPL-only files.

## Load and start

1. The kext is in the auxiliary kernel collection but nothing loads it at boot. Its personality matches IOResources only once the `XeGfxArm` resource exists.
2. `kmutil load -b org.watchthelight.XeGfx` runs `_start`, which allocates two locks and a watchdog thread call, registers the `debug.xegfx_*` sysctls with the kernel and publishes `XeGfxArm`. IOKit then attaches `XeGfxResident` to IOResources. Nothing touches hardware.
3. `sysctl debug.xegfx_stage=N` runs the stages up to N that have not run yet, in order, in the calling thread. Each stage arms the watchdog for `debug.xegfx_wdog` seconds and records its result (1 pass, negative failure) in `debug.xegfx_results`.

Stage 2 never maps BAR2 or touches stolen memory directly. On Meteor Lake class parts, CPU access to stolen memory through a BAR can hang the machine (i915's `Wa_22018444074`). It reads registers through an uncached BAR0 mapping only, and reads the GGTT entry that maps the firmware's framebuffer (GGTT sits at BAR0 + 8 MiB on Gen12 and later).

Stage 4 runs every SYSINIT in the kext, the way FreeBSD's module loader does: drm, TTM, dma-buf, then i915, which finds the GPU through newbus and probes it. i915's display code is off (`compat.linuxkpi.i915_disable_display=1` by default), because the firmware already lit the panel and macOS's own framebuffer driver uses it. Afterwards the stage reads the GGTT entry behind the panel's surface again and samples pipe A's frame counter. If i915 rewrote the entry or the pipe stopped, the stage fails with "display lost". Without the check this would go unnoticed: WindowServer looks fine while the screen is black.

i915's probe returns success even when the GuC firmware failed to load: it marks the GT wedged and carries on. So stage 4 then asks i915 directly (`xg_i915_state`) and fails with -5 when a GT is wedged and -62 when the GuC is not running or submission does not go through it. The answers land in the `XeGfxGt` dictionary: firmware status codes for GuC, HuC and GSC (10 is running), the GuC version, the engine count and the render node minor.

i915 6.12's `disable_display` still creates every connector and talks to the panel's AUX channel. The NvGfx overlay adds `compat.linuxkpi.i915_xegfx_no_display` (default 1), which skips the display driver entirely until XeGfx owns the panel. The build also refuses to link when any source failed to compile, so a missing object can no longer turn into a panicking stub.

drm-kmod disables the GuC on FreeBSD on purpose: its default-options code is under `#ifndef __FreeBSD__` ("GuC is known to be broken"), so `enable_guc` stays at -1 and i915 takes the no-GuC path. Meteor Lake and Arrow Lake have no execlist submission, so that path finds the WOPCM already locked and returns -EIO, and the GT is declared wedged. XeGfx's default environment sets `compat.linuxkpi.i915_enable_guc=3` (load HuC, GuC submission).

Stage 5 is the first time the GPU does work. For every engine i915 exposes, `xg_i915_exec` builds a request on the engine's kernel context, submits it and waits up to 2 s for it. A request that completes has gone through the GuC, raised an interrupt through the shim's MSI path and woken the waiter, so one pass covers all three. `XeGfxExec` holds the time each engine took, its error if any, and how many interrupts arrived while the stage ran. Stage 6 checks that the GPU writes memory the CPU can see. Every engine that can store a dword writes a known value into its own hardware status page through the GGTT, and the CPU reads it back; `XeGfxStore` lists what each engine wrote. These checks are i915 code, so they live in the NvGfx overlay (`xkpi/src/i915/xg_i915_exec.c`, MIT) and the shim only sees the plain structs in `shim/include/xg_i915.h`.

## Kernel mappings of page lists

LinuxKPI's `vmap()` reserves kernel VA with `kva_alloc()` and fills it with `pmap_qenter()`. On Tahoe the shim cannot do that second step: the reservation is a pageable buffer in IOKit's pageable submaps, and laying physical pages over it with `kIOMapOverwrite` fails (i915's first `vmap`, its GGTT scratch page, panicked on the Legion in N-221). The NvGfx overlay's `linux_page.c` maps the pages at an address IOKit picks instead (`xg_vm_vmap_pa`) and keeps a table for `vunmap()`. Stage 1 checks the path: it maps two pages, writes through the new mapping and reads through the old one, and fails with -12 rather than panicking.

## Mapping GEM objects into a process

XNU does not let a kext implement mmap(2) on a device node. Userspace maps GEM objects through two ioctls on the DRM node instead, with the layout and numbers of NullMoth's Darwin libdrm patch, so that libdrm and Mesa work unchanged: `'N' 0x40` takes the fake offset from `DRM_IOCTL_I915_GEM_MMAP_OFFSET` and returns an address in the caller, and `'N' 0x41` removes the mapping. The shim's ioctl path hands group `'N'` to a hook (`xg_fd_map_ioctl`) that stage 4 installs; the i915 side (`xkpi/src/i915/xg_i915_map.c`) looks the offset up the way `i915_gem_mmap()` does, pins the object's pages and maps them into the calling task with `IOMemoryDescriptor` (`xg_vm_x_map_user`). The pin and an object reference are held until the mapping goes away, because nothing can revoke a mapping that IOKit made. WB, WC and UC work; the GTT type is refused. One difference from mmap(2): closing the node removes its mappings.

Around a userspace check, `debug.xegfx_hold=N` arms a second watchdog that panics unless it is reset to 0 within N seconds, so a GPU deadlock in a test program also ends as a panic report and a reboot.

A whole-machine hang leaves neither a panic nor a log. With `xegfx.nvram_trace=1` in `debug.xegfx_env`, a timer writes the running stage, the breadcrumb (`xg_progress`, which names the SYSINIT entry during stage 4) and the newest console line to the NVRAM variable `xegfx-step` every 250 ms while something changed, and syncs it; the previous boot's value moves to `xegfx-step-prev`. It is off by default because it writes NVRAM.

Two switches in `debug.xegfx_env` exist only to A/B the whole-machine stall under heavy I/O (I-116). `xegfx.irqoff_check=0` turns off the interrupts-off checker and its 20 ms wakeup. `xegfx.gtawake=1` takes every GT's power reference after stage 4's GT check, as i915's `forcewake_user` debugfs file does, and `xegfx.gtawake=2` also holds forcewake on all domains, so the GT never enters RC6; both stay held until the next restart and cost iGPU power. zep245's Tiger Lake port stopped its whole-Mac freezes only at the second level. `XeGfxGt` in the registry reports `gtawake-gts`.

`tools/xgprobe.c` is the first client: it opens the render node, reads the DRM version and i915 params, queries the engines, creates a context and, on every legacy ring, runs a softpinned batch that stores a dword into a second buffer, both mapped through the ioctl. i915 refuses pread, pwrite and relocations after Tiger Lake, so the mapping is the only way to get a batch to the GPU.

## Fences without file descriptors

Mesa ANV will not start unless the kernel can export a buffer as a dma-buf and move fences in and out of it as sync_file descriptors, and Vulkan semaphores are passed around as sync_file descriptors too. A kext cannot create file descriptors. So the descriptors are made in userspace, inside libdrm's `drmIoctl()` (`userland/patches/libdrm-xegfx.patch` in NvGfx): a stand-in is the read end of a pipe, remembered with its inode so a recycled number is noticed. A dma-buf stands for a GEM handle on its DRM file; a sync_file stands for a syncobj. Two more node ioctls do the kernel half: `'N' 0x42` replaces a syncobj's fence with the buffer's pending fences, `'N' 0x43` adds a syncobj's fence to the buffer, with the same rules as Linux's `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` and `IMPORT_SYNC_FILE` (`xkpi/src/i915/xg_drm_sync.c`). Syncobj to sync_file and back is a `SYNCOBJ_TRANSFER` into a fresh syncobj. The layer only takes over a node that answers `'N' 0x42` the XeGfx way, so drm-shim and other nodes behave as before. What it cannot do yet: share a stand-in between DRM files or processes, make `poll()` wait on a sync_file, or merge sync_files.

The node also answers `DRM_IOCTL_GET_PCIINFO`, which libdrm uses on the BSDs and Darwin to enumerate devices; drm-kmod declares it but leaves it out. Mesa's own changes (`mesa-xegfx.patch`) route `intel_ioctl()` through `drmIoctl()` and map buffers through the node's ioctl instead of mmap(2).

## The firmware's console image

i915's display code stays off, so nothing tells the stolen-memory allocator that the panel still scans out the firmware's console image there; rings and contexts would land on it (VedCloud's port and nmx-intel both hit this). When stage 2 sees pipe A lit, it records the image's address and size (`boot-fb-size` in `XeGfxProbe`), and the overlay's `i915_gem_stolen.c` starts the allocator after it. In Discrete mode the pipe is off and nothing changes.

## Firmware

Gen12 and later need Intel's GuC firmware for command submission (from Alder Lake-P on, in practice also on Meteor Lake and Arrow Lake). The kext carries the blobs it needs in `Contents/Resources` under their base names (`mtl_guc_70.bin`), with Intel's `LICENSE.i915`. LinuxKPI's `request_firmware` asks for `i915/mtl_guc_70.bin`, the shim retries with the base name, and the XNU side loads it through `OSKextRequestResource`. That request only completes after `_start` has returned, which is one more reason the stages run later.

## Testing on a real machine

The Legion setup uses a LaunchDaemon (`xe-guard` in the private project repo) that runs queued experiments after login and judges them without a person watching: it takes a queue entry before touching anything, loads the kext, runs the stages, snapshots `debug.xegfx_log`, the probe dictionaries and the kernel log after each stage, and calls it a pass only if every stage passed, WindowServer kept its process, and no panic report appeared. You can do the same by hand:

```
sudo kmutil load -b org.watchthelight.XeGfx
sudo sysctl debug.xegfx_wdog=60
sudo sysctl debug.xegfx_stage=2
ioreg -l -w0 -r -c XeGfxResident | grep -A40 XeGfxProbe
sysctl -b debug.xegfx_log
```

## What is not done

- The DRM render node exists in the code but has not been exercised; ANV needs a small userspace transport shim (sync_file emulation, mapping by ioctl, PCI identity) before it can talk to it.
- The Metal side (an accelerator and the plugin's Intel path) lives in NvGfx and has not run on Intel yet.
- Display ownership (a flip-only framebuffer that adopts the firmware's mode) is a separate kext in NvGfx (NMIntelFB) and is untested.
- The xe driver (Lunar Lake, Battlemage, Panther Lake) is not built yet; it comes after i915 works.
