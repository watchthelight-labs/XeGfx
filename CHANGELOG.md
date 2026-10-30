# Changelog

Versions are the kext's `CFBundleVersion`. "Booted" means the version ran on the test laptop (Legion 5 15IAX10, Arrow Lake iGPU `8086:7D67`); "built" means it compiled and has not been run. Prereleases with a binary: v0.0.27, v0.0.35, v0.0.36, v0.0.38, v0.0.45 and v0.0.46.

## Unreleased (plugin, booted: N-319, N-320)

- Zero-copy flip of the Intel panel frame, off unless the boot-arg `xezcflip=1` is set (built, not booted). XeGfx reserves three GGTT
  slots below the GuC top and exposes `xgFlipInit/xgFlipMap/xgFlipUnmap` on XeGfxResident; XeGfxAccel then maps a finished frame's
  pages into a free slot, writes PLANE_SURF, waits until PLANE_SURFLIVE shows the new address (40 ms at most) and only then releases
  the older frames. The wait for the frame to be complete is the copy path's per-surface one (`nvSurfWaitReady`,
  `nvAccelSurfCopyReady`), not a global GPU-idle poll. Any frame that does not fit (other pitch, no slot, refusal) is copied after the
  plane is put back on the boot scanout. `debug.nvaccel_iop_zcflip` switches it live; counters `debug.nvaccel_iop_zc_*`. Based on
  Mountainatic/ArrowWhatever patches 0008/0009 (PolyForm Noncommercial). Not done: their deferred worker that arms the plane off
  WindowServer's thread (the thing that took them from 15 to 60 fps); the flip here is armed synchronously on the display thread.

- Horizontal bands across the desktop while typing or scrolling are gone (I-119). An app's render pass or IOSurface write
  on ANV could still be running when WindowServer composited the surface, so a half-drawn frame reached the screen.
  On Intel, every process except WindowServer now waits at commit for command buffers that render or write an IOSurface.
  Writing 64 to `/Library/GPUBundles/nvmtl/xe-live` turns this off live for A/B tests. N-320: no bands with it on,
  bands within seconds with it off. mtlbench under load stays at 4.6 to 5.0 ms per frame.
- WindowServer waiting for its own command buffers (N-318) and a GPU barrier at the start of each command buffer
  (N-317) did not help; the barrier is still in, `NVMTL_XE_CB_BARRIER=0` turns it off.

## 0.0.46 (booted: N-300 to N-302)

- A userptr page that is being freed is never wired again (I-121). `vm_page_unwire` dropped the last wiring before `vm_page_free` took the run out of the table, so a new hold on the same page could take the count from 0 to 1 in between and `vm_page_free` panicked with "freeing wired page". It happened at the restart that ended N-299 after 5.6 hours under load. The restart that ended N-300, with the desktop live under the same load, did not panic.

The NvGfx `xe` side of the same runs:

- XeGfxIntelFB 0.6 publishes `VRAM,totalMB` on the GPU (3/4 of RAM above 4 GB, the size ANV gives its heap), so System Information shows "VRAM (Dynamic, Max): 24576 MB" on a 32 GB machine instead of the 15 MB firmware framebuffer (N-301). `xeintelfbvram=<MB>` overrides it.
- XeGfxIntelFB 0.7 reads the panel's EDID over eDP AUX A and measures the refresh rate from the pipe's frame counter. The panel gets its real name, size and 165 Hz, macOS applies the same display override it uses on the NVIDIA GPU, and the HiDPI 1280x800 mode is offered (N-302). Scaled sizes larger than half the panel still need the pipe scaler, which is not done yet.
- XeGfxIntelFB 0.8 names the GPU by generation ("Intel Arrow Lake Xe-LPG" for 7D67), and the Metal device on Intel uses the same name (built, not booted).
- Plugin: `NVMTL_XE_UNIFIED=1` in nvrm610.conf makes the Intel device report unified memory. Off by default; N-302 runs WindowServer with it on.
- Plugin: heap textures keep their swizzle and buffers record their memory type, which fixed the flat yellow Dock symbols (I-120). N-298 ran four hours of desktop under load with it.

## 0.0.45 (booted: N-295, N-296)

N-295 (`gtawake=2`) froze 16 minutes into the Intel desktop with the same rendezvous panic, so forcewake is not the I-116 cause. N-296 ran the same build with `xegfx.nvram_trace=0` and the Intel desktop stayed up for the full hour. The trace's NVRAM writes run in SMM on this firmware, where a CPU cannot answer the rendezvous interrupt, so the trace is now the main suspect. Leave it off for desktop runs.


- `xegfx.gtawake=1` holds every GT's wakeref, `=2` also takes forcewake on all domains, so the GT never enters RC6. Off by default. It mirrors what stopped the whole-Mac freezes on another Xe port (I-116). The implementation of `xg_i915_gtawake` is in the NvGfx fork's `gpu/xkpi` overlay.
- `XeGfxGt` in the registry reports `gtawake-gts`.

## 0.0.44 (built, not booted)

- `xegfx.irqoff_check=0` turns off the interrupts-off checker and its 20 ms wakeup, to A/B it against the I-116 freeze. The checker stays on by default.

## 0.0.43 (booted: N-290)

- The interrupts-off watchdog names the CPU and the caller before macOS hits its rendezvous timeout (I-116). On N-290 the Intel desktop stayed up for the 900 s test, the first run to do so since I-116 appeared.

## 0.0.42 (booted: N-289, crashed at about 15 to 20 s with the I-116 rendezvous panic)

- Reverts the blanket rwlock spin from 0.0.40, which caused rendezvous panics (N-287, N-288).
- `xg_map_mmap` takes the lookup lock before `rcu_read_lock` instead, which fixes the sleeping-lock-in-RCU panic on WindowServer's mmap path.

## 0.0.41

- Interrupt and handler counts in `debug.xegfx_spin`, for the N-287 stall.
- XeGfxIntelFB 0.5 names the iGPU so About This Mac shows "Intel Xe Graphics".

## 0.0.40

- rwlocks spin inside critical sections, after WindowServer's mmap-offset lookup blocked under `rcu_read_lock` and panicked (N-286). Reverted in 0.0.42.

## 0.0.39

- Logs why a userptr hold fails (WindowServer execbuffer EFAULT, N-283).
- XeGfxIntelFB 0.3 clears the guard with a zero count.

## 0.0.38 and earlier

See the git history. 0.0.38 rebases syncobj wait deadlines from Darwin's CLOCK_MONOTONIC onto the shim's ktime; 0.0.37 returns `ETIME` for LinuxKPI's `ETIMEDOUT` so Mesa sees `VK_TIMEOUT`.
