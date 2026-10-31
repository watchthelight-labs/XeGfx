# Contributing to XeGfx

Thanks for looking. This is an experiment, so the most valuable things right now are test reports and careful review.

## Testing on your machine

What helps most is the read-only probe (stage 2) on Intel Xe hardware other than the one Arrow Lake laptop it was written on. It only reads registers. Even so, run it only if you can recover your Mac from a hang (a power button press and a normal boot are enough, because nothing loads XeGfx at boot).

1. Build with `build/build_xegfx.sh`, install with `sudo tools/install.sh` and reboot once so the auxiliary collection includes it.
2. `sudo kmutil load -b org.watchthelight.XeGfx`, then `sudo sysctl debug.xegfx_stage=2`.
3. Open an issue with your CPU, GPU device ID (`ioreg -l | grep -i '"device-id"'` on the `IGPU` or `GFX0` entry), macOS build, BIOS graphics mode, and the output of `ioreg -l -w0 -r -c XeGfxResident` and `sysctl -b debug.xegfx_log`.

Please do not run stage 4 on a machine you depend on yet. It starts i915 on your GPU.

## Code

- Public KPIs only. Anything the kext calls must come from a library it declares, never `com.apple.kpi.private`. `tools/link.sh` checks this.
- No new kernel-symbol shadows. If `tools/shadow_check.py` fails, add the name to `shim/include/xg_bsdnames.h` instead of allow-listing it.
- No GPL-only code. drm-kmod and Linux are mixed MIT, BSD and GPL. The shipping build refuses GPL-only sources and headers. If you need one of those files, write a replacement or take one with a compatible licence, and say where it came from.
- Keep the drm-kmod and LinuxKPI sources unchanged where possible. Fix things in the shim; if a third-party file must change, prefer an overlay file to a patch.
- Comments say why, briefly. History belongs in commit messages.
- Small commits, lowercase subject lines.

## Where to send changes

The shim, harness and checks are developed here and pulled into NvGfx's `xe` branch. Changes to the Metal plugin, the accelerator or the overlay go to [WTLNvidiaGfx](https://github.com/watchthelight-labs/WTLNvidiaGfx) on the `xe` branch. Open an issue first for anything larger than a fix, so we do not duplicate work.

## Questions

Open an issue, or ping `watchthelight` on Discord.
