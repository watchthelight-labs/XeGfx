# WTLIntelGfx: Intel Xe graphics acceleration for macOS (Hackintosh)

Renamed from `XeGfx` on 2026-10-31; the driver and its kexts are still called XeGfx. Old links redirect here.

Real GPU acceleration for Intel Core Ultra graphics on a Hackintosh. XeGfx gives Meteor Lake and Arrow Lake iGPUs
Metal and Vulkan on macOS, so the desktop, the Dock's Liquid Glass and your apps draw on the Intel GPU instead of the
CPU. Apple's drivers stop at Ice Lake and WhateverGreen can't help on Xe, so without a driver like this these machines run
macOS with no acceleration at all.

## What you get

- **Metal and Vulkan 1.4** on the Intel GPU. Apps see a Metal device named "Intel(R) Graphics" and a Vulkan device from
  Mesa's ANV.
- **An accelerated desktop.** WindowServer composites the whole desktop on the Intel GPU, and the Dock renders its
  Liquid Glass effect.
- **The built-in panel at its real refresh rate.** The panel is named from its EDID and runs at 165 Hz, with HiDPI modes.
- **It shows up like a real GPU.** About This Mac says "Intel Xe Graphics", and System Information shows 24 GB of
  shared VRAM.
- **Hours of uptime.** Sessions of 1, 2, 4 and 5.6 hours under a disk and Metal load script, on macOS Tahoe 26.7.1 and
  26.7.2.

It runs today on:

- **Arrow Lake** (Core Ultra 9 275HX, `8086:7D67`): our test laptop, a Lenovo Legion 5 15IAX10, and a second 275HX
  laptop ([ArrowWhatever](https://github.com/Mountainatic/ArrowWhatever)).
- **Meteor Lake** (Core Ultra 7 155H, `8086:7D55`): a tester has Metal running on Sequoia with the XeGfx kernel driver
  unchanged and a few framebuffer and plugin tweaks of their own.

It's still a test driver. One kernel panic under heavy disk reads is open (see [Status](#status-2026-10-28)), so try it
on a Mac you can afford to reboot, not one you depend on. Downloads are on the
[releases page](https://github.com/watchthelight-labs/WTLIntelGfx/releases), and the project page is
[labs.watchthelight.org/WTLIntelGfx](https://labs.watchthelight.org/WTLIntelGfx/).

The goal is one driver for every Xe generation, Tiger Lake through Lunar Lake plus Arc discrete cards, on every macOS
from Big Sur 11 to Tahoe 26. Under the hood it runs Linux's i915 driver inside the macOS kernel ([How it works](#how-it-works)).

## Does it work on my machine?

Find your GPU's PCI ID with `ioreg -l | grep -i '"device-id"'` on macOS or `lspci -nn | grep VGA` on Linux. A report
from your machine, working or not, is the most useful thing you can send ([issue #3](https://github.com/watchthelight-labs/WTLIntelGfx/issues/3)).

| Family | Example PCI IDs (8086:) | Graphics | State |
|---|---|---|---|
| Arrow Lake-S / HX (Core Ultra 200S, 200HX) | 7D67 | Xe-LPG | **runs**: Metal, Vulkan, accelerated desktop (two laptops) |
| Meteor Lake (Core Ultra 100H, 100U) | 7D55, 7DD5, 7D45, 7D40 | Xe-LPG | **runs**: Metal on a 155H (tester report, Sequoia) |
| Arrow Lake-H / U (Core Ultra 200H, 200U) | 7D51, 7DD1, 7D41 | Xe-LPG(+) | untested; same GuC firmware as 7D67, so the closest match |
| Tiger Lake (11th gen Core) | 9A49, 9A40, 9A78, 9A60 | Xe-LP (Iris Xe, UHD) | untested; needs its own GuC firmware linked in |
| Alder Lake, Raptor Lake (12th to 14th gen mobile) | 46A6, 46A3, 46A8, 4688, A7A0, A7A8, A788 | Xe-LP (Iris Xe, UHD) | stage 2 probe passes on 46A6 and stages 1 to 3 on 46A3 (tester reports); later stages need their own GuC firmware |
| Intel Arc A-series (Alchemist) | 56A0, 56A1, 5690 | Xe-HPG | untested |
| Lunar Lake, Battlemage, Panther Lake | 64A0, E20B, E20C | Xe2 and later | needs the xe driver, not i915 ([issue #8](https://github.com/watchthelight-labs/WTLIntelGfx/issues/8)) |

If your Tiger Lake, Alder Lake or Raptor Lake laptop only needs its panel to work, look at [WhateverXe](https://github.com/b00t0x/WhateverXe) and [NootedGreen](https://github.com/sgiammori/NootedGreen) first. They run Apple's own Tiger Lake driver on your GPU and are further along on those chips. XeGfx exists because that route does not reach Meteor Lake, Arrow Lake or Arc, and because a driver built from i915 can follow Intel's Linux support to new chips.

## How it works

XeGfx is not a GPU driver written from scratch. FreeBSD's [drm-kmod](https://github.com/freebsd/drm-kmod) already runs i915 on a non-Linux kernel through LinuxKPI, a layer that implements the Linux kernel API on top of FreeBSD's. XeGfx adds a small FreeBSD-kernel-API layer for XNU underneath LinuxKPI, so drm core and i915 build as one kext with their sources almost untouched. In userspace, Mesa's ANV Vulkan driver talks to it through a patched libdrm, and a Metal-to-Vulkan plugin runs on ANV. The plugin and the Metal accelerator kext are forks of NullMoth's NVIDIA driver, carried here from [WTLNvidiaGfx](https://github.com/watchthelight-labs/WTLNvidiaGfx), so this repository holds the whole Intel driver, kernel to Metal. [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) walks through each layer.

## Status (2026-10-28)

It works, for hours at a time. On the test laptop the Intel GPU runs real work for the kernel and for user processes, Vulkan and Metal both draw correctly, and since N-285 (2026-10-24) WindowServer composites the whole desktop on the Intel GPU through Metal and keeps it on the built-in panel, with the Dock's glass effect rendering. Since 0.0.45 that desktop stays up: 1, 2, 4 and 5.6 hours on N-296 to N-299, the longer runs under a disk and Metal load script, and 0.0.46 fixed the panic the 5.6 h run hit at restart (I-121). One panic is still open: a CPU rendezvous timeout (I-116) that comes back after hours of heavy disk reads (N-304). On 2026-10-28 the same 0.0.46 build passed N-305 on macOS 26.7.2 (25G332) unchanged: stages 1-7 and WindowServer stable on the Intel GPU. The drawing glitches on that desktop (I-119) are mostly fixed in the NvGfx `xe` plugin: on N-311 the cursor no longer draws as a square and Terminal text shows everywhere, after WindowServer got framebuffer fetch. What is left is bands of backdrop colour along full-width rows while typing; the fix for that is built but not yet tested on the Intel panel.

Everything below was proven on one machine: a Lenovo Legion 5 15IAX10 with an Intel Core Ultra 9 275HX (Arrow Lake-HX). Its integrated graphics are Xe-LPG, PCI ID `8086:7D67` (GMD_ID graphics 12.70, display 14), which i915 treats as part of the Meteor Lake family. It runs macOS Tahoe 26.7.1 (25G241) and, since 2026-10-28, 26.7.2 (25G332), with the BIOS in Hybrid graphics mode, so the built-in panel is wired to the Intel GPU. The boot IDs (N-xx) are test boots on that laptop.

| Milestone | Done when | State |
|---|---|---|
| X1 compiles | every drm core and i915 source compiles for the kernel target | done, all 418 sources |
| X2 links | one kext links against public KPIs only, no stubs on reachable paths | done |
| X3 loads | loads next to the firmware framebuffer and probes the GPU | done, N-219 |
| X4 GT up | GuC loaded, a request completes, render node exists | done, N-229 |
| X5 ANV | a Vulkan device, a compute pipeline and an offscreen draw | done, N-234 (2026-10-22) |
| X5b Metal | apps get a Metal device on the Intel GPU; compute and a draw come back right | done, N-235 to N-238 (2026-10-22) |
| X6 desktop | WindowServer composites on the Intel GPU and the panel shows it | working, N-285 (2026-10-24); up for hours since N-296, one panic open (I-116) |
| X7 other laptops | builds and boots on other Xe machines, with docs | testers wanted |

What each step showed on the 7D67:

- **GT up (N-229).** The GuC firmware `mtl_guc_70.bin` (version 70.53.0) loads, with GuC submission, SLPC and RC enabled. A kernel request completed on each of the render, copy and compute engines (rcs0, bcs0, ccs0), and each one wrote a dword that the CPU read back.
- **Userspace (N-230).** A user process opened `/dev/dri/renderD128`, created a context, mapped buffers and ran execbuffer on the render and copy engines, and the results read back correctly.
- **libdrm (N-233).** `drmGetDevices2` lists the GPU as `0000:00:02.0 8086:7d67`, driver i915 1.6.0.
- **Vulkan through Mesa ANV (N-234).** ANV creates a Vulkan 1.4 device (API 1.4.363), "Intel(R) Graphics (ARL)", and a compute pipeline on it. A draw test (`tools/vkdraw` on NvGfx `xe`, commit `9c9cbc3`) cleared a 64x64 image to red in a render pass, drew a green triangle into it (exactly 512 green pixels), copied the image to a host buffer, and the CPU read back the right pixels.
- **Metal (N-235 to N-238).** XeGfx's stage 7 hands the GPU to XeGfxAccel, NullMoth's NMXAccel accelerator built over XeGfx (`kexts/XeGfxAccel`). The Metal plugin (`plugin/`) then runs on ANV. Metal shows one device, "Intel(R) Graphics (ARL)". An MSL compute kernel fills 1024 values correctly, and a render pipeline draws a green triangle on red that reads back exactly. Getting there took three fixes. Render nodes are world-accessible, as on Linux. GFP_ATOMIC allocations get a reserve, because i915 allocates under spinlocks. Host-visible BOs use the GPU's snooping write-back PAT, because XNU keeps RAM write-back whatever cache mode an IOKit mapping asks for, and write-combining BOs lost cache lines to CPU write-backs.
- **Desktop (N-245 to N-260).** WindowServer can't open `/dev/dri` from inside its sandbox, so libdrm now sends its ioctls through the accelerator's IOKit user client when the node is refused (0.0.28). With that, a restarted WindowServer composites on the Intel GPU through Metal. On N-258 it stayed up through the settle, put 425 frames on the panel and kept the desktop for two hours before a kernel panic (one CPU stopped answering). On N-260 the machine froze about 20 s in, with no panic report. Frames reach the panel through a copy into the firmware's framebuffer, because macOS does not yet send display-pipe transactions to this GPU, so the desktop is slow even when it runs. 
- **Accelerated desktop (N-285 to N-289, 2026-10-24).** WindowServer now runs on the Intel GPU and stays up: on N-285 it passed the guard's settle window and served the desktop for about four minutes, with 3660 display transactions and no failed frames. The fix that got it there was in the Metal plugin: WindowServer hands it some buffers that are not all writable memory, and importing those as host pointers lost the device about 20 s in, so the plugin now copies them instead. The Dock renders its Liquid Glass effect, which needs working GPU compositing. XeGfxIntelFB 0.5 sets the GPU's model name, so About This Mac and System Information show "Intel Xe Graphics".
- **What still breaks.** One panic is understood and fixed: a lock that sleeps was taken inside an RCU read section on the mmap path WindowServer uses (0.0.42 takes the locks in the other order). The other is not: 15 to 45 seconds into some runs one CPU keeps interrupts off for over half a second and macOS panics because that CPU does not answer a rendezvous. GPU interrupts keep arriving and are handled right up to the end. 0.0.43 adds a watchdog that names that CPU and what it was running before macOS gives up. Turning the NVRAM trace off made it much rarer (on this firmware its NVRAM writes run in SMM, where a CPU cannot answer the rendezvous), but it came back on N-304 after 4 h 19 min under heavy disk reads with the trace off, so that was not the whole cause.

The open problems are tracked in [WTLNvidiaGfx #12](https://github.com/watchthelight-labs/WTLNvidiaGfx/issues/12) and [#18](https://github.com/watchthelight-labs/WTLNvidiaGfx/issues/18).

The source is at 0.0.46 ([CHANGELOG](CHANGELOG.md)), which is also the newest booted version. With the NVRAM trace off (`xegfx.nvram_trace=0`) the Intel desktop ran one hour on N-296, then two hours (N-297), four hours (N-298) and 5.6 hours (N-299) while a load script wrote about 225 GB to disk and ran Metal draws every 22 seconds. N-304 then hit I-116 after 4 h 19 min while another process read the kernel collections over and over, so the stall is rarer, not gone. 0.0.46 fixes the panic that N-299's restart hit (I-121). On the Intel GPU, System Information now shows 24 GB of shared VRAM, and the panel is named from its EDID and runs at 165 Hz with the HiDPI mode. The newest test build is the [v0.0.46 prerelease](https://github.com/watchthelight-labs/WTLIntelGfx/releases/tag/v0.0.46). Desktop runs need the accelerator and plugin from this repository as well.

[VedCloud's Meteor Lake port](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) got to a GPU-composited desktop first: it runs an i915-derived driver with Metal and Vulkan on a Dell Latitude 5350 under macOS 15. XeGfx takes a different road (unchanged drm-kmod sources over its own shim) and targets Tahoe as well.

### Problems worth knowing about

- drm-kmod never turns the GuC on in FreeBSD builds, and Meteor Lake and Arrow Lake cannot submit work without it, so XeGfx sets `enable_guc=3` by default.
- `OSKextRequestResource` does not serve the `Resources` of a kext that lives in the auxiliary kernel collection, so the GuC firmware is linked into the kext binary. That is also why other generations need their own firmware linked in before they can be tried.
- XNU devfs clone nodes give every `stat` and `open` its own minor number, so libdrm could not match an open file to its `/dev/dri` node. The patched libdrm matches nodes by slot instead.
- WindowServer's sandbox refuses `/dev/dri`, which is why the desktop path goes through IOKit instead.

## Built on, and learned from

XeGfx is built on other people's work. Thanks to all of them.

**Code XeGfx builds or ships** (their licences apply to their files):

| Project | What we use |
|---|---|
| [freebsd/drm-kmod](https://github.com/freebsd/drm-kmod) | drm core, TTM, dma-buf and i915 sources, compiled unchanged |
| [FreeBSD LinuxKPI](https://github.com/freebsd/freebsd-src/tree/main/sys/compat/linuxkpi) and [FreeBSD kernel](https://github.com/freebsd/freebsd-src) | the Linux kernel API layer, plus `subr_eventhandler.c` and FreeBSD's headers |
| [Linux](https://github.com/torvalds/linux) i915 and DRM | the driver drm-kmod tracks; the `sync_file` uapi header |
| [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) | Intel's GuC firmware (`LICENSE.i915`) |
| [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) | `plugin/`, `kexts/XeGfxAccel`, `kexts/XeGfxAGDC` and `kexts/NMIntelFB` are forks of it; its old AMD/Intel port (commit `bf2dee3`) supplies the GPL-free dma-buf, sync_file and fence-unwrap replacements and the Mesa/libdrm Darwin patches used in NvGfx's `xe` branch |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) ANV and [libdrm](https://gitlab.freedesktop.org/mesa/drm) | the Vulkan userspace |
| [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) | the Metal-to-SPIR-V translator in `translator/`; its coverage table and refusals list, and its Arrow Lake ANV validation profile |
| [watchthelight-labs/kcabi](https://github.com/watchthelight-labs/WTLKcAbi) | kernel collection link and ABI checks |

**Projects we studied** (ideas, findings and warnings only; no code copied):

| Project | What we learned |
|---|---|
| [b00t0x/WhateverXe](https://github.com/b00t0x/WhateverXe), [WhateverXe-Graphics](https://github.com/b00t0x/WhateverXe-Graphics), [b00t0x/OpenCore-Legacy-Patcher](https://github.com/b00t0x/OpenCore-Legacy-Patcher) | Xe-LP on Apple's Tiger Lake driver across Big Sur to Tahoe; forcing BAR0 MMIO to strong uncached; stolen-memory decoding; adopting the firmware's pipes without a modeset; since 0.4.0 Tiger Lake laptop panels too, by reading panel facts from the firmware's VBT and keeping the eDP scrambler and framing bits in step with the panel during link training; shipping the graphics stack as a signed /Library package instead of System patches; 0.6.0 (October 2026) adds Alder Lake and Raptor Lake laptops, defers framebuffer opens until AGDC registration, and states Xe-LPG is not planned; Graphics 0.3.0 converts shaders to AIR with the system compiler |
| [sgiammori/NootedGreen](https://github.com/sgiammori/NootedGreen) | how far the Apple TGL donor approach reaches on Alder Lake and Raptor Lake |
| [JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe) | keeping the firmware's eDP mode, the boot-programmable mode trap in WindowServer, the GGTT[0] machine-check hazard, Tahoe IOSurface offsets; holding the boot plane dark until WindowServer owns the display, a scheduler fix for a context-status event that was being dropped, and blit paths that fail on wide or oddly pitched IOSurface planes; later, render-engine workarounds applied after every engine start, power rerouting so hybrid P-cores get correct HWP units, a per-command-buffer leak that grew WindowServer past 10 GB, and eDP backlight over DPCD AUX |
| [m4j2rpf766-crypto/IntelIGPU-Tahoe](https://github.com/m4j2rpf766-crypto/IntelIGPU-Tahoe) | handing the panel over from IONDRVFramebuffer on Tahoe; isolating the iGPU at boot |
| [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) (earlier: [VedCloud2/CoreUltraGraphics](https://github.com/VedCloud2/CoreUltraGraphics)) | the first i915-on-XNU port to drive real Intel hardware: an i915-derived kernel driver on LinuxKPI, Mesa ANV and a NullMoth-derived Metal plugin, with Metal and Vulkan working on a Meteor Lake Dell Latitude 5350 under macOS 15; GuC submission with Linux's own MTL firmware, a GPU-composited desktop, and two lessons for our LinuxKPI: a lost wake-up that looked like GuC timeouts, and starting the stolen-memory allocator after the firmware's console image; on 2026-10-23 they confirmed that ANV's sync_file probe can be passed from userspace with stand-in descriptors, the route XeGfx 0.0.12 takes |
| [kitsuneislife/nmx-intel](https://github.com/kitsuneislife/nmx-intel) | i915 through NullMoth's port lighting a Raptor Lake laptop panel on macOS, and a framebuffer that copies frames composited on another GPU to the Intel panel; its pitfalls notes taught us to reserve the boot framebuffer's stolen pages and GGTT range before i915 allocates, and to give the framebuffer an `IOFBDependentID` |
| [IT-EXPRESS-Bayern/LunarBridge-Hackintosh](https://github.com/IT-EXPRESS-Bayern/LunarBridge-Hackintosh) | Lunar Lake (Xe2) groundwork on Tahoe |
| [wallali77666-cyber/iris-xe-macos-driver](https://github.com/wallali77666-cyber/iris-xe-macos-driver) | a NullMoth-style stack aimed at Tiger Lake |
| [zep245/IrisXe-macos-driver](https://github.com/zep245/IrisXe-macos-driver) | i915 and Mesa ANV running on Tiger Lake under Sequoia through NullMoth's Intel path; sandboxed apps see no GPU because the sandbox refuses `opendir(/dev/dri)`, and the in-kernel LinuxKPI panics it ran into (thread_call limits, `cond_resched` inside `ggtt_pin`, TLB flush timeouts); in 1.0.0 (October 2026) scaled desktops through i915's panel fitter by giving a modeset the panel's timing with a smaller source, the hardware cursor plane, page flips instead of copies, NVRAM breadcrumbs that made each flip take about 700 ms, and the plugin's one-time copy of small no-copy textures that hid typed letters; in 1.0.1 (October 2026) Tahoe icons back on the GPU after finding that buffer textures were refused on ANV's memory types (the same cause as our I-120, found independently), and per-function constant spaces for linked visible functions |
| [CENSEDAXIS20025/ASUS-FX608LP-Hackintosh-Multiboot](https://github.com/CENSEDAXIS20025/ASUS-FX608LP-Hackintosh-Multiboot) | the same Core Ultra 9 275HX, 7D67 iGPU and RTX 5070 Laptop running macOS on the RTX with the iGPU blocked and the MUX set to discrete only |
| [Mountainatic/ArrowWhatever](https://github.com/Mountainatic/ArrowWhatever) | the same Core Ultra 9 275HX and 7D67 iGPU with an RTX 5080 Laptop on XeGfx 0.0.44, with ten patches of their own: a pin versus last-unwire race in userptr buffers, GGTT zero-copy scanout with a flip deferred until the GPU is idle (about 60 fps at 2560x1600, up from about 15), and the panel timing read from BAR0. Their userptr panic and fix (patch 0001) is the same bug we hit as I-121 and fixed in 0.0.46, found independently. Patch 0009 lets WindowServer submit without waiting and flips only once the engines are idle, which took the desktop to about 60 fps. We have not taken a patch yet |
| [victorwitkamp/NootedGreen](https://github.com/victorwitkamp/NootedGreen) (branch `raptor-lake-hx`) | a NootedGreen fork for Raptor Lake-HX that ports i915 forcewake, pipe VDSC and the render engine workarounds; its owner reports Metal on a ROG Strix G814JI |
| [samranahmad09/WhateverXe-RaptorLakeP](https://github.com/samranahmad09/WhateverXe-RaptorLakeP) | checking a bring-up by capturing i915's registers on Linux next to a macOS readout, and a blitter ring self-test |
| [lutzbichler/drm-kmod](https://github.com/lutzbichler/drm-kmod) (xe patches) and [drm-kmod #316](https://github.com/freebsd/drm-kmod/issues/316) | the state of the xe driver outside Linux |
| [drm-kmod #315](https://github.com/freebsd/drm-kmod/issues/315), [#418](https://github.com/freebsd/drm-kmod/pull/418), [#468](https://github.com/freebsd/drm-kmod/pull/468) | LinuxKPI bugs FreeBSD already fixed for Meteor Lake and Arc, to carry over |
| [openbsd/src](https://github.com/openbsd/src) inteldrm | attaching i915 in stages and deferring the heavy probe |
| [acidanthera/WhateverGreen](https://github.com/acidanthera/WhateverGreen) and [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) | the Intel framebuffer background every Hackintosh graphics project builds on |

**Testers** (reports from machines other than ours; every result counts, working or not):

| Who | Machine | What they reported |
|---|---|---|
| [zsct648gn6-dev](https://github.com/zsct648gn6-dev) | Lenovo Legion Y9000P 2022, i7-12700H, `46A6` | stage 2 probe passes on 0.0.46 ([#3](https://github.com/watchthelight-labs/WTLIntelGfx/issues/3#issuecomment-6538047492)) |
| [TrDiTu](https://github.com/TrDiTu) | HP Victus 15, i7-12650H, `46A3` | stages 1 to 3 pass; stage 4 panicked in the PCH lookup, fixed in 0.0.47 ([#3](https://github.com/watchthelight-labs/WTLIntelGfx/issues/3#issuecomment-6529881723)) |
| [ductruongvu23](https://github.com/ductruongvu23) | Gigabyte Gaming A16, i7-13620H, `A7A8` | an iGPU in a discrete-mode MUX shows up as class 0380, which the probe missed until 0.0.47 |
| [UNDERDOG07](https://github.com/UNDERDOG07) | Lenovo Legion Y7000P IRX10, i7-14650HX, `A788` | device data for the 32 EU Raptor Lake-HX part, class 0380 in discrete mode |
| [HAOoool](https://github.com/HAOoool) | Dell G15 5511, i7-11800H, `9A60` | Tiger Lake-H device data; discrete mode removes the iGPU from ACPI and PCI |
| [559911](https://github.com/559911) | Mechrevo GM6AR57C, i7-12800HX, `4688` | Alder Lake-HX device data, with WhateverXe 0.6.1 for comparison |
| [Mochen-wu](https://github.com/Mochen-wu) | ROG Strix G16 G614JVR, i9-14900HX, `A788` | Raptor Lake-HX device data on Tahoe 26.7.2 |
| [phmthnh](https://github.com/phmthnh) | Acer Nitro AN515-58, i5-12500H, `46A6` | Linux topology, stolen memory and firmware versions |

## How it stays safe to test

A broken GPU driver can stop a machine before anything reaches the disk, so the kext is built to be harmless until asked:

- It never loads at boot. Its only IOKit personality waits for a resource the kext publishes itself, after `kmutil load -b org.watchthelight.XeGfx`. If a test crashes the Mac, it reboots into a system where XeGfx is not loaded.
- It runs in stages, one sysctl write at a time:

  | `debug.xegfx_stage` | What runs |
  |---|---|
  | 1 | the shim's own state (clock, threads, VM, tunables, task queues) |
  | 2 | a read-only look at the GPU: IDs, BAR sizes, GMD_ID, stolen-memory base, pipe A registers, the GGTT entry behind the panel, PCH watchdog registers, published as the `XeGfxProbe` registry dictionary |
  | 3 | newbus over every PCI device, the DMA layer |
  | 4 | the drm and i915 init routines, i915 display disabled, then a check that the panel still scans out the same memory and pipe A still counts frames |

- A stage that runs longer than `debug.xegfx_wdog` seconds panics on purpose, so a stall becomes a panic report and a reboot instead of a frozen screen.
- `debug.xegfx_log` returns the last 64 KiB of the driver's own console output, and `debug.xegfx_results` the result of each stage.
- `debug.xegfx_env` sets tunables for the next stage (`compat.linuxkpi.i915_enable_guc=3` and the like), so one boot can try several settings.
- The link fails if the kext redefines any symbol the kernel exports (`tools/shadow_check.py`). That bug class killed the first two hardware tests.

## Building

You need the Xcode Command Line Tools (clang and the macOS SDK; the scripts are tested with `SDKROOT` set to the 15.5 SDK) and Python 3. Third-party sources are not in the repository:

- `~/src/xe-src`: sparse clones of `linux` (the Linux kernel), `freebsd` (LinuxKPI lives in `sys/compat/linuxkpi`), `drm-kmod` (commit `c16de0f`, Linux 6.14 level) and `linux-firmware` (`mtl_guc_70.bin` and `LICENSE.i915`, linked into the kext)
- `~/src/deps/open-gpu-${NVRM_TAG:-615.78.08}`: NVIDIA's open-gpu-kernel-modules, whose headers XeGfxAccel compiles against
- `~/src/deps/Vulkan-Headers`: Khronos Vulkan headers for the plugin
- rustup stable for the translator, and the Mesa and libdrm trees `userland/BUILD.md` lists

```
build/build_xegfx.sh          # XeGfx.kext into out/: objects, licence gate, link, KPI / shadow / kcabi checks
build/build_xegfxaccel.sh     # XeGfxAccel.kext, the Metal accelerator
build/build_xegfxintelfb.sh   # XeGfxIntelFB.kext
build/build_xegfxagdc.sh      # XeGfxAGDC.kext
build/build_nmintelfb.sh      # NMIntelFB.kext, the boot framebuffer on the iGPU
build/build_plugin.sh         # the Metal plugin, NVMTLDriver (RELEASE=1 for a release build)
build/build_xlate.sh          # libnvmtl_translate.dylib, Metal AIR to SPIR-V
userland/scripts/build_*.sh   # libdrm, Mesa ANV and the Vulkan loader
sudo tools/install.sh         # into the auxiliary kernel collection (XEGFX_MODE=uninstall to remove)
```

Kexts and the plugin land in `~/src/XeGfx-build` (`OUTROOT`, `XEGFX_OUT`). `build/xeaccel-pkg` and `build/xeintelfb-pkg` hold install scripts that swap one kext in the auxiliary collection and keep a backup. `tools/kpi_split.py` and the link check use [WTLKcAbi](https://github.com/watchthelight-labs/WTLKcAbi), expected in `~/src/kcabi`.

## Layout

| Path | What it is |
|---|---|
| `shim/src/fbsd` | The FreeBSD kernel API on XNU: threads, locks, sleep queues, malloc and UMA, VM pages and objects, newbus and PCI, bus_dma, sysctl, taskqueues, callouts, firmware loading |
| `shim/src/xnu` | The C++ side that talks to IOKit: kext entry and stages, PCI devices, interrupts, DMA, devfs nodes |
| `shim/include` | XNU overrides of FreeBSD and LinuxKPI headers; `xg_bsdnames.h` renames FreeBSD functions that share a name with a kernel export |
| `xkpi/` | Overlay on drm-kmod: replacements for its GPL-only files, LinuxKPI fixes, and the i915 glue the stages call (`xg_i915_*`) |
| `harness/score.py` | Compiles every drm-kmod source for the XNU kernel target; `--obj` builds the objects and runs the licence gate |
| `kexts/XeGfxAccel` | The Metal accelerator (IOAccelerator family) WindowServer and Metal talk to, and the copy of finished frames to the panel |
| `kexts/XeGfxIntelFB`, `kexts/NMIntelFB` | The framebuffer kexts for the built-in panel on the iGPU |
| `kexts/XeGfxAGDC` | The AppleGraphicsDeviceControl service IOPresentment needs before WindowServer gives the framebuffer a display pipe |
| `plugin/` | The Metal driver bundle: Metal API on Vulkan (ANV) |
| `translator/` | The shader translator, Metal AIR to SPIR-V (a metal2vulkan fork) |
| `userland/` | Patches and build scripts for libdrm, Mesa ANV and the Vulkan loader on macOS, and small Vulkan and Metal test tools |
| `tools/` | `link.sh`, `kpi_check.py` (public KPIs only), `shadow_check.py`, `install.sh`, `igfx/` (panel and EDID checks) |
| `build/` | The build scripts above |
| `docs/` | One design note per shim area (`xg_*.md`) and [HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) |

Until 2026-10-30 everything outside `shim/`, `harness/`, `tools/` and `docs/` lived in the NvGfx fork (branches `xe` and `xe-2.6.2`); its history is there. Fixes to the shared Metal plugin are ported between the two repositories.

## Helping

Testers with any Intel Xe machine are the most useful thing right now, and so is anyone who knows XNU, IOKit or i915 internals. See [CONTRIBUTING.md](CONTRIBUTING.md) and the open issues.

## License

Mixed, by folder. The kernel shim, the build scripts and the tools are under the BSD 2-Clause license (`LICENSE`). The Metal plugin, the accelerator, XeGfxAGDC and NMIntelFB are forks of NullMoth's driver and stay under PolyForm Noncommercial 1.0.0 (`licenses/`): no commercial use. The translator is LGPL-3.0. `NOTICE` has the full table.
