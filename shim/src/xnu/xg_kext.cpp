/*
 * XeGfx kext entry. The module loads idle on IOResources and does nothing until a stage is asked for:
 *   sysctl debug.xegfx_stage=N runs every stage up to N that has not run yet (1 shim, 2 read-only GPU probe,
 *   3 bus and DMA, 4 drm/i915 SYSINITs and a GT health check, 5 one request on every engine,
 *   6 every engine writes a dword the CPU reads back, 7 set nmxgpu-claimed for the Metal accelerator). debug.xegfx_env sets tunables for the stages (overrides boot-args).
 * Nothing loads it at boot: its one personality waits for the XeGfxArm resource, which _start publishes after a
 * manual `kmutil load -b org.watchthelight.XeGfx`. There is no PCI personality: the shim finds the GPU in the registry. A stage that runs longer than
 * debug.xegfx_wdog seconds panics on purpose, so a stall becomes a panic report and an automatic reboot.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <architecture/i386/pio.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <mach/kmod.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <string.h>
#include "xg_i915.h"
#include "xg_fd.h"
#include "xg_vmap.h"

extern "C" {
void xg_start_stage(int stage);
int xg_drm_attached(void);
int xg_pci_xnu_start(void *iopcidevice);
void xg_newbus_dump(void);
void xg_unimpl(const char *name) __attribute__((__noreturn__));
kern_return_t _start(kmod_info_t *ki, void *d);
kern_return_t _stop(kmod_info_t *ki, void *d);
KMOD_EXPLICIT_DECL(org.watchthelight.XeGfx, "0.0.32", _start, _stop)
char xg_env[512];
int xg_progress;	/* last breadcrumb a running stage reached; the guard journals it */
extern unsigned int xg_pci_irqs;
extern "C" unsigned int xg_pci_handled;
extern "C" {
struct xg_irqoff { volatile unsigned long long t0; volatile unsigned long pc; volatile int what; };
extern struct xg_irqoff xg_irqoff[64];
extern int xg_spin_panic;
}
static thread_call_t gIrqoffCall;
static volatile UInt32 gIrqoffArmed;
static vm_address_t gKextBase;

/*
 * N-287..N-289 died in XNU's rendezvous timeout: one CPU kept interrupts off for half a second and the report never
 * says which. This runs on a CPU that still answers, every 20 ms, and names the slot first (100 ms).
 */
static void
xg_irqoff_check(thread_call_param_t, thread_call_param_t)
{
	uint64_t now = mach_absolute_time(), ns;

	for (int c = 0; c < 64; c++) {
		uint64_t t0 = xg_irqoff[c].t0;
		if (t0 == 0 || t0 > now)
			continue;
		absolutetime_to_nanoseconds(now - t0, &ns);
		if (ns > 100000000ull && xg_spin_panic)
			panic("xegfx: cpu %d has had interrupts off for %llu ms in %s, pc %#lx (XeGfx base %#lx, offset %#lx)",
			    c, ns / 1000000, xg_irqoff[c].what == 2 ? "the interrupt filter" : "a spin section",
			    xg_irqoff[c].pc, (unsigned long)gKextBase, xg_irqoff[c].pc - (unsigned long)gKextBase);
	}
	uint64_t dl;
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &dl);
	thread_call_enter_delayed(gIrqoffCall, dl);
}
extern int xg_spin_panic;
extern unsigned long long xg_spin_max_us;
extern unsigned int xg_spin_long;
/* the firmware's console image, when pipe A scans it out at probe; i915's stolen allocator starts after it */
unsigned long long xg_boot_fb_phys, xg_boot_fb_size;
}

class XeGfxResident : public IOService {
	OSDeclareDefaultStructors(XeGfxResident)
public:
	bool start(IOService *provider) override;
	using IOService::callPlatformFunction;
	IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
};

static XeGfxResident *gResident;
static int gStage;	/* highest stage done */
static int gRunning;	/* stage in progress, 0 when idle */
static int gResult[8];	/* per stage: 0 not run, 1 pass, negative errno-style failure */
static int gWdog = 60;
static thread_call_t gWdogCall;
static thread_call_t gHoldCall;	/* armed around userspace checks: a GPU deadlock there panics too */
static int gHold;
/*
 * NVRAM trace (I-104: a stage 4 hang left no log at all): with xegfx.nvram_trace=1 in debug.xegfx_env, every 250 ms
 * while a stage runs, "xegfx-step" gets the stage, the breadcrumb and the newest console line, synced at once.
 */
static thread_call_t gTraceCall;
static bool gTraceOn;
static char gLast[600];	/* the newest console lines, oldest dropped first */
static unsigned gLastSeq, gTraceSeq;
static char gAccel[96];	/* XeGfxAccel's breadcrumb (xgTrace), NVRAM xegfx-accel */
static unsigned gAccelSeq, gAccelTraceSeq;
static int gTraceProgress;
static void xg_trace_write(void);

/* strstr is not in the public kext KPIs */
/* strstr is not in the public kext KPIs */
static const char *
strstr_kpi(const char *h, const char *n)
{
	size_t l = strlen(n);

	for (; *h; h++)
		if (strncmp(h, n, l) == 0)
			return h;
	return nullptr;
}

static bool
xg_env_has(const char *kv)
{
	size_t n = strlen(kv);

	for (const char *p = xg_env; *p; p++)
		if (strncmp(p, kv, n) == 0)
			return true;
	return false;
}
static IOLock *gStageLock;
static uint64_t gSurfPte;
static bool gPipeLit;	/* pipe A was scanning out at probe time (Hybrid); in Discrete mode the NVIDIA GPU has the panel */
static char gLog[256 * 1024];	/* last 256 KiB of the shim's console output, for sysctl debug.xegfx_log */
static size_t gLogLen;
static IOSimpleLock *gLogLock;
static IOLock *gSnapLock;	/* GGTT entry behind PLANE_SURF_1_A at probe time, 0 if unknown */

bool
XeGfxResident::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;
	gResident = this;
	registerService();
	return true;
}

/* "xgDrmIoctl" (minor, cmd, user arg, int *errno): DRM for WindowServer, through XeGfxAccel's user client */
IOReturn
XeGfxResident::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
	if (name && name->isEqualTo("xgDrmIoctl") && p4) {
		*(int *)p4 = xg_fd_remote_ioctl((int)(uintptr_t)p1, (unsigned long)p2, (unsigned long long)(uintptr_t)p3);
		return kIOReturnSuccess;
	}
	/* "xgFlipInit" (surface bytes, slots wanted, u64 *ggtt out, unsigned *count out) / "xgFlipMap" (slot, phys page
	 * array, npages, surface bytes) / "xgFlipUnmap" (slot, surface bytes): zero-copy scanout; the accelerator writes
	 * PLANE_SURF, this side owns the GGTT slots and PTEs. Idle until XeGfxAccel runs with xezcflip=1. */
	if (name && name->isEqualTo("xgFlipInit") && p3 && p4)
		return xg_flip_init((unsigned long long)(uintptr_t)p1, (unsigned int)(uintptr_t)p2, (unsigned long long *)p3,
		    (unsigned int *)p4) == 0 ? kIOReturnSuccess : kIOReturnError;
	if (name && name->isEqualTo("xgFlipMap") && p2)
		return xg_flip_map((unsigned int)(uintptr_t)p1, (unsigned long long)(uintptr_t)p4, (const unsigned long long *)p2,
		    (unsigned int)(uintptr_t)p3) == 0 ? kIOReturnSuccess : kIOReturnError;
	if (name && name->isEqualTo("xgFlipUnmap"))
		return xg_flip_unmap((unsigned int)(uintptr_t)p1, (unsigned long long)(uintptr_t)p2) == 0 ? kIOReturnSuccess : kIOReturnError;
	/* "xgTrace" (const char *line): XeGfxAccel's state, stored with the console trace (thread context only) */
	if (name && name->isEqualTo("xgTrace") && p1 && gLogLock) {
		IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
		strlcpy(gAccel, (const char *)p1, sizeof(gAccel));
		gAccelSeq++;
		IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
		return kIOReturnSuccess;
	}
	return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
}
OSDefineMetaClassAndStructors(XeGfxResident, IOService)

static void
xg_wdog_fire(thread_call_param_t, thread_call_param_t)
{
	/* the panic report is the only record a stall leaves, so it carries the end of the driver's log (N-224) */
	static char tail[3072];
	size_t n = MIN(gLogLen, sizeof(tail) - 1);

	if (!gRunning)
		return;
	memcpy(tail, gLog + gLogLen - n, n);	/* no lock: the stalled thread may hold it; a torn line is fine here */
	tail[n] = 0;
	panic("XeGfx: stage %d stalled for %d s at progress %d\n--- last driver log ---\n%s", gRunning, gWdog,
	    xg_progress, tail);
}

/* ---- stage 2: read-only look at the iGPU and the PCH watchdog, published as XeGfxProbe ---- */

static void
xg_put(OSDictionary *d, const char *k, uint64_t v, int bits = 32)
{
	OSNumber *n = OSNumber::withNumber(v, bits);

	if (n) {
		d->setObject(k, n);
		n->release();
	}
}

static IOPCIDevice *
xg_find_pci(uint16_t vendor, uint8_t cls, uint8_t sub)
{
	IOPCIDevice *found = nullptr;
	OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
	OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;

	OSSafeReleaseNULL(m);
	if (!it)
		return nullptr;
	while (OSObject *o = it->getNextObject()) {
		IOPCIDevice *p = OSDynamicCast(IOPCIDevice, o);
		if (p && p->configRead16(kIOPCIConfigVendorID) == vendor &&
		    p->configRead8(kIOPCIConfigClassCode + 2) == cls && p->configRead8(kIOPCIConfigClassCode + 1) == sub) {
			found = p;
			found->retain();
			break;
		}
	}
	it->release();
	return found;
}

static int
xg_probe(void)
{
	OSDictionary *d = OSDictionary::withCapacity(32);
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00), *smb = xg_find_pci(0x8086, 0x0c, 0x05);
	int rc = 1;

	if (!d)
		return -12;
	if (!gpu) {
		rc = -19;
		goto out;
	}
	xg_progress = 21;
	xg_put(d, "device-id", gpu->configRead16(kIOPCIConfigDeviceID), 16);
	xg_put(d, "revision", gpu->configRead8(kIOPCIConfigRevisionID), 8);
	xg_put(d, "subsystem", gpu->configRead32(kIOPCIConfigSubSystemVendorID));
	xg_put(d, "command", gpu->configRead16(kIOPCIConfigCommand), 16);
	xg_put(d, "ggc", gpu->configRead16(0x50), 16);
	for (int bar = 0; bar < 2; bar++) {
		IODeviceMemory *m = gpu->getDeviceMemoryWithRegister((UInt8)(kIOPCIConfigBaseAddress0 + bar * 8));
		char k[16];
		snprintf(k, sizeof(k), "bar%d-base", bar * 2);
		xg_put(d, k, m ? m->getPhysicalAddress() : 0, 64);
		snprintf(k, sizeof(k), "bar%d-size", bar * 2);
		xg_put(d, k, m ? m->getLength() : 0, 64);
	}
	/* register reads only, uncached BAR0 map; never BAR2 or stolen memory (Wa_22018444074) */
	if (gpu->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace) {
		IOMemoryMap *mm = gpu->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
		if (mm) {
			volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
			static const struct { const char *k; uint32_t off; } regs[] = {
				{ "gmd-id-graphics", 0xd8c }, { "gmd-id-display", 0x510a0 },
				{ "dsm-base-lo", 0x1080c0 }, { "dsm-base-hi", 0x1080c4 },
				{ "pcode-stolen-access", 0x138914 },
				{ "transconf-a", 0x70008 }, { "pipe-frmcount-a", 0x70040 },
				{ "plane-ctl-1-a", 0x70180 }, { "plane-stride-1-a", 0x70188 }, { "plane-size-1-a", 0x70190 },
				{ "plane-surf-1-a", 0x7019c },
			};
			xg_progress = 22;
			for (auto &e : regs)
				xg_put(d, e.k, r[e.off / 4]);
			gPipeLit = (r[0x70008 / 4] & 0x80000000u) != 0;
			xg_put(d, "pipe-a-lit", gPipeLit, 8);
			if (mm->getLength() >= 16ull << 20) {
				uint32_t surf = r[0x7019c / 4] & ~0xfffu;
				volatile uint64_t *gtt = (volatile uint64_t *)(mm->getVirtualAddress() + (8ull << 20));
				gSurfPte = gtt[surf >> 12];
				xg_put(d, "ggtt-pte-surf", gSurfPte, 64);
				if (gPipeLit && (gSurfPte & 1)) {
					/* linear stride is in 64-byte units; PLANE_SIZE holds height - 1 in bits 31:16 */
					xg_boot_fb_phys = gSurfPte & 0x3ffffffff000ull;	/* GEN12_GGTT_PTE_ADDR_MASK */
					xg_boot_fb_size = (uint64_t)(r[0x70188 / 4] & 0x7ff) * 64 * ((r[0x70190 / 4] >> 16) + 1);
					xg_put(d, "boot-fb-size", xg_boot_fb_size, 64);
				}
			}
			mm->release();
		} else {
			rc = -5;
		}
	}
	xg_progress = 23;
	/* PCH TCO watchdog (SMBus function, TCOBASE at config 0x50, TCOCTL 0x54), I/O port reads only */
	if (smb) {
		uint16_t tcobase = smb->configRead16(0x50) & 0xffe0;
		uint32_t tcoctl = smb->configRead32(0x54);
		xg_put(d, "tco-base", tcobase, 16);
		xg_put(d, "tco-ctl", tcoctl);
		if (tcobase && (tcoctl & (1 << 8))) {
			xg_put(d, "tco-rld", inw(tcobase + 0x0), 16);
			xg_put(d, "tco1-sts", inw(tcobase + 0x4), 16);
			xg_put(d, "tco2-sts", inw(tcobase + 0x6), 16);
			xg_put(d, "tco1-cnt", inw(tcobase + 0x8), 16);
			xg_put(d, "tco2-cnt", inw(tcobase + 0xa), 16);
			xg_put(d, "tco-tmr", inw(tcobase + 0x12), 16);
		}
	}
out:
	xg_put(d, "result", (uint32_t)rc);
	if (gResident)
		gResident->setProperty("XeGfxProbe", d);
	d->release();
	OSSafeReleaseNULL(gpu);
	OSSafeReleaseNULL(smb);
	return rc;
}

static uint32_t gBootSurf;	/* PLANE_SURF_1_A after i915 ran: the boot surface's GGTT offset */

/* after i915 has run: the panel must still scan out the boot surface through the same GGTT entry, and pipe A must
 * still count frames; otherwise the desktop is black even though WindowServer looks fine */
static int
xg_display_check(void)
{
	OSDictionary *d = OSDictionary::withCapacity(8);
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);
	IOMemoryMap *mm = gpu ? gpu->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache) : nullptr;
	int rc = -6;

	if (!gPipeLit && d) {
		/* nothing of ours on the panel to lose; say so instead of failing on a stopped frame counter */
		xg_put(d, "skipped-pipe-a-off", 1, 8);
		xg_put(d, "result", 1);
		if (gResident)
			gResident->setProperty("XeGfxPost", d);
		rc = 1;
	} else if (mm && d && mm->getLength() >= 16ull << 20) {
		volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
		uint32_t surf = r[0x7019c / 4] & ~0xfffu, f0 = r[0x70040 / 4];
		gBootSurf = surf;
		uint64_t pte = ((volatile uint64_t *)(mm->getVirtualAddress() + (8ull << 20)))[surf >> 12];
		IOSleep(100);
		uint32_t f1 = r[0x70040 / 4];
		xg_put(d, "plane-surf-1-a", surf);
		xg_put(d, "ggtt-pte-surf", pte, 64);
		xg_put(d, "frmcount-delta", f1 - f0);
		rc = (gSurfPte && pte != gSurfPte) ? -71 : (f1 == f0 ? -70 : 1);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxPost", d);
	}
	OSSafeReleaseNULL(d);
	OSSafeReleaseNULL(mm);
	OSSafeReleaseNULL(gpu);
	return rc;
}

/* i915 probe succeeds even when GuC loading failed and the GT was wedged; fail stage 4 on that instead */
static int
xg_gt_check(void)
{
	struct xg_i915_state st;
	OSDictionary *d = OSDictionary::withCapacity(10);
	int rc = xg_i915_state(&st) == 0 ? 1 : -19;

	if (rc == 1 && st.wedged)
		rc = -5;
	else if (rc == 1 && (st.guc_status != 10 || !st.guc_submission))
		rc = -62;	/* 10 is INTEL_UC_FIRMWARE_RUNNING */
	/* xegfx.gtawake=1|2: zep245's whole-Mac freezes stopped only with forcewake held on all domains (I-116) */
	int awake = rc != 1 ? 0 : xg_i915_gtawake(xg_env_has("xegfx.gtawake=2") ? 2 : xg_env_has("xegfx.gtawake=1") ? 1 : 0);
	if (d) {
		xg_put(d, "gts", st.gts, 8);
		xg_put(d, "wedged", st.wedged, 8);
		xg_put(d, "guc-fw-status", (uint32_t)st.guc_status);
		xg_put(d, "guc-version", st.guc_ver);
		xg_put(d, "guc-submission", st.guc_submission, 8);
		xg_put(d, "huc-fw-status", (uint32_t)st.huc_status);
		xg_put(d, "gsc-fw-status", (uint32_t)st.gsc_status);
		xg_put(d, "engines", st.engines, 8);
		xg_put(d, "render-minor", (uint32_t)st.render_minor);
		xg_put(d, "gtawake-gts", (uint32_t)awake);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxGt", d);
		d->release();
	}
	return rc;
}

/*
 * stage 7: hand the GPU to the Metal accelerator (XeGfxAccel, NMXAccel built over XeGfx). Its personality matches
 * only an IOPCIDevice with nmxgpu-claimed, so nothing matched the live iGPU at boot; registerService re-runs matching
 * on the nub, which adds the accelerator in its own match category and leaves every existing client alone.
 */
static int
xg_accel_claim(void)
{
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);

	if (gpu == nullptr)
		return -19;
	gpu->setProperty("nmxgpu-claimed", kOSBooleanTrue);
	gpu->registerService();
	gpu->release();
	return 1;
}

/* stage 5: a kernel request per engine proves GuC submission, the MSI path and the breadcrumb wake together */
static int
xg_exec_check(void)
{
	static struct xg_i915_exec ex;
	unsigned int irq0 = xg_pci_irqs;
	OSDictionary *d = OSDictionary::withCapacity(XG_I915_MAX_ENGINES + 4);
	int rc = xg_i915_exec(&ex, 2000) == 0 ? 1 : -19;

	if (rc == 1 && (ex.engines == 0 || ex.done != ex.engines))
		rc = -62;
	if (d) {
		for (int i = 0; i < ex.engines; i++) {
			char k[24];
			snprintf(k, sizeof(k), "%s-ns", ex.name[i]);
			xg_put(d, k, (uint64_t)ex.ns[i], 64);
			snprintf(k, sizeof(k), "%s-err", ex.name[i]);
			xg_put(d, k, (uint32_t)ex.err[i]);
		}
		xg_put(d, "engines", ex.engines, 8);
		xg_put(d, "done", ex.done, 8);
		xg_put(d, "irqs", xg_pci_irqs - irq0);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxExec", d);
		d->release();
	}
	return rc;
}

/* stage 6: the GPU writes memory the CPU then reads, through the GGTT, on every engine that can */
static int
xg_store_check(void)
{
	static struct xg_i915_store sd;
	OSDictionary *d = OSDictionary::withCapacity(2 * XG_I915_MAX_ENGINES + 3);
	int rc = xg_i915_store(&sd, 2000) == 0 ? 1 : -19;

	if (rc == 1 && (sd.engines == 0 || sd.ok != sd.engines))
		rc = -5;
	if (d) {
		for (int i = 0; i < sd.engines; i++) {
			char k[24];
			snprintf(k, sizeof(k), "%s-got", sd.name[i]);
			xg_put(d, k, sd.got[i]);
			snprintf(k, sizeof(k), "%s-err", sd.name[i]);
			xg_put(d, k, (uint32_t)sd.err[i]);
		}
		xg_put(d, "engines", (uint32_t)sd.engines, 8);
		xg_put(d, "ok", (uint32_t)sd.ok, 8);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxStore", d);
		d->release();
	}
	return rc;
}

static int
xg_run_stage(int s)
{
	int rc = 1;

	gRunning = s;
	xg_progress = s * 10;
	gTraceOn = xg_env_has("xegfx.nvram_trace=1");
	xg_spin_panic = !xg_env_has("xegfx.spin_panic=0");
	if (gTraceOn) {
		xg_trace_write();
		thread_call_enter(gTraceCall);
	}
	/* xegfx.irqoff_check=0 turns the checker off: N-290 passed with it, is its 20 ms wakeup the fix? */
	if (gIrqoffCall && !xg_env_has("xegfx.irqoff_check=0") && OSCompareAndSwap(0, 1, &gIrqoffArmed))
		xg_irqoff_check(nullptr, nullptr);
	if (gWdogCall && gWdog > 0) {
		uint64_t dl;
		clock_interval_to_deadline(gWdog, NSEC_PER_SEC, &dl);
		thread_call_enter_delayed(gWdogCall, dl);
	}
	if (s == 2) {
		rc = xg_probe();
	} else {
		xg_start_stage(s);
		if (s == 1 && xg_vm_vmap_selftest() != 0)
			rc = -12;	/* the vmap path i915 needs from its first GGTT setup on (N-221) */
		if (s == 3) {
			/* every IOPCIDevice into newbus (config reads only), then vgapci on the Intel display function so
			 * stage 4's drm drivers have a device to probe; the old PCI personality used to trigger this */
			IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);
			int err = gpu ? xg_pci_xnu_start(gpu) : ENODEV;
			OSSafeReleaseNULL(gpu);
			rc = err ? -err : 1;
			xg_newbus_dump();
		}
		if (s == 4) {
			rc = xg_display_check();
			if (rc == 1 && !xg_drm_attached())
				rc = -19;	/* the init routines ran but no drm driver bound the GPU */
			if (rc == 1) {
				xg_i915_map_init();
				rc = xg_gt_check();
			}
			if (rc == 1 && gPipeLit && gBootSurf && xg_boot_fb_size) {
				int err = xg_i915_reserve_bootfb(gBootSurf, xg_boot_fb_size);

				IOLog("XeGfx: boot surface GGTT 0x%x + %llu reserved: %d\n", gBootSurf, xg_boot_fb_size, err);
				/* -ENOSPC at or above GUC_GGTT_TOP is i915's own GuC-top reservation, which keeps it free (N-256) */
				if (err == -28 && gBootSurf < 0xfee00000u)
					rc = -28;	/* something else sits there: compositing into it would overwrite it */
			}
		}
	}
	if (s == 5)
		rc = xg_exec_check();
	if (s == 6)
		rc = xg_store_check();
	if (s == 7)
		rc = xg_accel_claim();
	if (gWdogCall)
		thread_call_cancel(gWdogCall);
	if (gTraceOn) {
		gTraceOn = false;
		thread_call_cancel(gTraceCall);
		xg_trace_write();	/* the stage's end state, s0 */
	}
	gRunning = 0;
	return rc;
}

static int
xegfx_stage_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gStage, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr)
		return err;
	if (v < 0 || v > 7)
		return EINVAL;
	IOLockLock(gStageLock);
	while (gStage < v) {
		int s = gStage + 1, rc = xg_run_stage(s);
		gResult[s] = rc;
		if (gResident)
			gResident->setProperty("XeGfxStage", (unsigned long long)s, 32);
		if (rc < 0)
			break;
		gStage = s;
	}
	IOLockUnlock(gStageLock);
	return 0;
}

static int
xegfx_results_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	return SYSCTL_OUT(req, gResult, sizeof(gResult));
}

static void
xg_trace_write(void)
{
	static bool kept;
	char b[sizeof(gLast) + 40], line[sizeof(gLast)], accel[sizeof(gAccel)];
	IORegistryEntry *o;
	IOInterruptState is;
	bool accelNew;

	is = IOSimpleLockLockDisableInterrupt(gLogLock);
	memcpy(line, gLast, sizeof(line));
	memcpy(accel, gAccel, sizeof(accel));
	accelNew = gAccelSeq != gAccelTraceSeq;
	gAccelTraceSeq = gAccelSeq;
	gTraceSeq = gLastSeq;
	gTraceProgress = xg_progress;
	IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	for (char *p = line; *p; p++)
		if (*p == '\n' || *p == '\r')
			*p = '|';
	snprintf(b, sizeof(b), "s%d p%d %s", gRunning, xg_progress, line);
	if ((o = IORegistryEntry::fromPath("/options", gIODTPlane)) == nullptr)
		return;
	if (!kept) {
		/* the last boot's trace survives one more boot, for whoever reads it after a recovery */
		kept = true;
		if (OSObject *old = o->copyProperty("xegfx-step")) {
			o->setProperty("xegfx-step-prev", old);
			old->release();
		}
	}
	if (OSData *d = OSData::withBytes(b, (unsigned)strlen(b))) {
		o->setProperty("xegfx-step", d);
		d->release();
	}
	if (accelNew && accel[0]) {
		snprintf(b, sizeof(b), "spin %llu us/%u %s", xg_spin_max_us, xg_spin_long, accel);
		if (OSData *d = OSData::withBytes(b, (unsigned)strlen(b))) {
			o->setProperty("xegfx-accel", d);
			d->release();
		}
	}
	if (IODTNVRAM *nv = OSDynamicCast(IODTNVRAM, o))
		nv->sync();
	o->release();
}

static void
xg_trace_fire(thread_call_param_t, thread_call_param_t)
{
	uint64_t dl;

	if (!gTraceOn)
		return;
	if (gLastSeq != gTraceSeq || xg_progress != gTraceProgress || gAccelSeq != gAccelTraceSeq)
		xg_trace_write();
	clock_interval_to_deadline(250, NSEC_PER_MSEC, &dl);
	thread_call_enter_delayed(gTraceCall, dl);
}

static void
xg_hold_fire(thread_call_param_t, thread_call_param_t)
{
	panic("XeGfx: a userspace check did not finish in %d s", gHold);
}

/* write N > 0 to panic unless 0 is written within N seconds; the guard brackets its post command with it */
static int
xegfx_hold_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gHold, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr)
		return err;
	if (v < 0)
		return EINVAL;
	thread_call_cancel(gHoldCall);
	gHold = v;
	if (v > 0) {
		uint64_t dl;
		clock_interval_to_deadline((uint32_t)v, NSEC_PER_SEC, &dl);
		thread_call_enter_delayed(gHoldCall, dl);
	}
	return 0;
}

/* 1 = keep the NVRAM trace running outside the stages too (the guard's WindowServer step: N-251/N-255 froze there) */
static int
xegfx_trace_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gTraceOn ? 1 : 0, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr)
		return err;
	if (v && !gTraceOn && gTraceCall) {
		gTraceOn = true;
		xg_trace_write();
		thread_call_enter(gTraceCall);
	} else if (!v && gTraceOn) {
		gTraceOn = false;
		thread_call_cancel(gTraceCall);
	}
	return 0;
}
static int
xegfx_spin_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[120];

	snprintf(b, sizeof(b), "max_us %llu long %u panic %d irqs %u handled %u", xg_spin_max_us, xg_spin_long, xg_spin_panic,
	    xg_pci_irqs, xg_pci_handled);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_spin, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_spin_sysctl, "A",
    "the longest lock spin seen, spins over 10 ms, whether a spin past its limit panics");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_trace, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_trace_sysctl,
    "I", "1 = write the newest console line to NVRAM xegfx-step whenever it changes, outside the stages too");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_stage, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_stage_sysctl,
    "I", "highest XeGfx stage done; write N to run stages up to N (1 shim, 2 probe, 3 bus, 4 drm/i915, 5 exec, 6 store, 7 Metal accelerator)");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_results, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_results_sysctl, "S", "per-stage result: 0 not run, 1 pass, negative failure");
static int
xegfx_fd_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[192];

	xg_fd_diag(b, sizeof(b));
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
extern "C" void xg_mem_diag(char *buf, unsigned long len);
static int
xegfx_mem_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char b[192];

	xg_mem_diag(b, sizeof(b));
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_mem, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_mem_sysctl, "A",
    "XeGfx M_NOWAIT reserve per size class and its misses");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_fd, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_fd_sysctl, "A",
    "XeGfx node instances by state and the fd lock holder");
SYSCTL_INT(_debug, OID_AUTO, xegfx_running, CTLFLAG_RD | CTLFLAG_LOCKED, &gRunning, 0, "stage in progress");
SYSCTL_INT(_debug, OID_AUTO, xegfx_progress, CTLFLAG_RD | CTLFLAG_LOCKED, &xg_progress, 0, "last breadcrumb");
SYSCTL_INT(_debug, OID_AUTO, xegfx_wdog, CTLFLAG_RW | CTLFLAG_LOCKED, &gWdog, 0, "stage stall panic, seconds");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_hold, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_hold_sysctl,
    "I", "panic unless reset to 0 within this many seconds (0 disarms)");
static int xegfx_log_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *);
SYSCTL_PROC(_debug, OID_AUTO, xegfx_log, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_log_sysctl,
    "S", "the shim's console output (last 256 KiB)");
SYSCTL_STRING(_debug, OID_AUTO, xegfx_env, CTLFLAG_RW | CTLFLAG_LOCKED, xg_env, sizeof(xg_env),
    "tunables for the stages, name=value pairs");

static struct sysctl_oid *const gOids[] = {
	&sysctl__debug_xegfx_stage, &sysctl__debug_xegfx_results, &sysctl__debug_xegfx_fd, &sysctl__debug_xegfx_mem, &sysctl__debug_xegfx_running,
	&sysctl__debug_xegfx_progress, &sysctl__debug_xegfx_wdog, &sysctl__debug_xegfx_env, &sysctl__debug_xegfx_log,
	&sysctl__debug_xegfx_hold,
	&sysctl__debug_xegfx_trace,
	&sysctl__debug_xegfx_spin,
};

/* Linux's struct va_format, the argument of "%pV" */
struct xg_va_format {
	const char	*fmt;
	va_list		*va;
};

/*
 * vsnprintf that also expands Linux's "%pV" (a nested format and its va_list), which neither XNU's nor FreeBSD's
 * printf knows; drm prints most of its messages through it (N-225 logged "MCR Steering: 0x...V"). Every other
 * conversion goes to vsnprintf one at a time with its argument taken at the right width.
 */
/* a Linux pointer extension: %p followed by a letter */
static bool
xg_has_pext(const char *f)
{
	for (; *f; f++)
		if (f[0] == '%' && f[1] == 'p' && ((f[2] >= 'A' && f[2] <= 'Z') || (f[2] >= 'a' && f[2] <= 'z')))
			return true;
	return false;
}

static int
xg_fmt(char *out, size_t len, const char *fmt, va_list *ap)
{
	size_t o = 0;

	if (!xg_has_pext(fmt))
		return vsnprintf(out, len, fmt, *ap);
	for (const char *p = fmt; *p && o + 1 < len; ) {
		if (*p != '%') {
			out[o++] = *p++;
			continue;
		}
		const char *q = p + 1;
		char spec[32];
		int lng = 0, n = 0;

		while (*q && strchr("-+ #0", *q))
			q++;
		while (*q == '*' || (*q >= '0' && *q <= '9') || *q == '.')
			q++;
		while (*q && strchr("hlqjzt", *q)) {
			if (*q == 'l' || *q == 'q' || *q == 'j' || *q == 'z' || *q == 't')
				lng++;
			q++;
		}
		if (!*q)
			break;
		if (q[0] == 'p' && q[1] == 'e') {	/* an ERR_PTR, printed as its errno */
			n = snprintf(out + o, len - o, "%ld", (long)(intptr_t)va_arg(*ap, void *));
			p = q + 2;
		} else if (q[0] == 'p' && ((q[1] >= 'A' && q[1] <= 'Z') || (q[1] >= 'a' && q[1] <= 'z')) && q[1] != 'V') {
			/* other Linux pointer extensions (%ps, %ph, %pI4...): the pointer, and skip the letters */
			n = snprintf(out + o, len - o, "%p", va_arg(*ap, void *));
			p = q + 1;
			while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
				p++;
		} else if (q[0] == 'p' && q[1] == 'V') {
			struct xg_va_format *vf = va_arg(*ap, struct xg_va_format *);
			va_list inner;
			va_copy(inner, *vf->va);
			n = xg_fmt(out + o, len - o, vf->fmt, &inner);
			va_end(inner);
			p = q + 2;
		} else {
			size_t sl = MIN((size_t)(q - p + 1), sizeof(spec) - 1);
			memcpy(spec, p, sl);
			spec[sl] = 0;
			if (strchr(spec, '*'))	/* width or precision from the arguments: rare in drm, print it plainly */
				n = snprintf(out + o, len - o, "%s", spec);
			else if (*q == '%')
				n = snprintf(out + o, len - o, "%%");
			else if (*q == 's')
				n = snprintf(out + o, len - o, spec, va_arg(*ap, const char *));
			else if (*q == 'p')
				n = snprintf(out + o, len - o, spec, va_arg(*ap, void *));
			else if (*q == 'c')
				n = snprintf(out + o, len - o, spec, va_arg(*ap, int));
			else if (lng)
				n = snprintf(out + o, len - o, spec, va_arg(*ap, long long));
			else
				n = snprintf(out + o, len - o, spec, va_arg(*ap, int));
			p = q + 1;
		}
		if (n > 0)
			o = MIN(o + (size_t)n, len - 1);
	}
	out[o] = 0;
	return (int)o;
}

/* vsnprintf for the FreeBSD and Linux code (xg_bsdnames.h). Like vsnprintf it returns the full length even when
 * out is NULL or short, since drm sizes buffers with snprintf(NULL, 0, ...) */
extern "C" int
xg_vsnprintf(char *out, size_t len, const char *fmt, va_list ap)
{
	static char scratch[2048];
	va_list aq;
	int n;

	va_copy(aq, ap);
	if (!xg_has_pext(fmt)) {
		n = vsnprintf(out, len, fmt, aq);
	} else if (out == nullptr || len == 0) {
		n = xg_fmt(scratch, sizeof(scratch), fmt, &aq);	/* length only; racy scratch is fine for a count */
	} else {
		n = xg_fmt(out, len, fmt, &aq);
	}
	va_end(aq);
	return n;
}

extern "C" int
xg_snprintf(char *out, size_t len, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = xg_vsnprintf(out, len, fmt, ap);
	va_end(ap);
	return n;
}

extern "C" int
xg_vprintf(const char *fmt, va_list ap)
{
	char buf[512];
	va_list aq;
	int n;

	va_copy(aq, ap);
	n = xg_fmt(buf, sizeof(buf), fmt, &aq);
	va_end(aq);
	if (gLogLock && n > 0) {
		size_t l = MIN((size_t)n, sizeof(buf) - 1);
		IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
		if (gLogLen + l > sizeof(gLog)) {
			size_t drop = gLogLen + l - sizeof(gLog) + sizeof(gLog) / 4;
			memmove(gLog, gLog + drop, gLogLen - drop);
			gLogLen -= drop;
		}
		memcpy(gLog + gLogLen, buf, l);
		gLogLen += l;
		if (l > 1) {
			size_t have = strlen(gLast), c = MIN(l, sizeof(gLast) - 1);
			if (have + c >= sizeof(gLast)) {
				size_t drop = have + c - (sizeof(gLast) - 1);
				memmove(gLast, gLast + drop, have - drop + 1);
				have -= drop;
			}
			memcpy(gLast + have, buf, c);
			gLast[have + c] = 0;
			gLastSeq++;
		}
		IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	}
	return printf("%s", buf);
}

extern "C" int
xg_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = xg_vprintf(fmt, ap);
	va_end(ap);
	return n;
}

static int
xegfx_log_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	static char snap[sizeof(gLog)];
	size_t n;

	IOLockLock(gSnapLock);	/* one reader at a time owns the snapshot; not the stage lock, a stage holds that */
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
	n = gLogLen;
	memcpy(snap, gLog, n);
	IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	int err = SYSCTL_OUT(req, snap, n);
	IOLockUnlock(gSnapLock);
	return err;
}

extern "C" void
xg_unimpl(const char *name)
{
	panic("XeGfx: %s is not implemented yet", name);
}

extern "C" kern_return_t
_start(kmod_info_t *ki, void *)
{
	gKextBase = ki ? ki->address : 0;
	gIrqoffCall = thread_call_allocate(xg_irqoff_check, nullptr);
	gStageLock = IOLockAlloc();
	gLogLock = IOSimpleLockAlloc();
	gSnapLock = IOLockAlloc();
	gWdogCall = thread_call_allocate(xg_wdog_fire, nullptr);
	gHoldCall = thread_call_allocate(xg_hold_fire, nullptr);
	gTraceCall = thread_call_allocate(xg_trace_fire, nullptr);
	if (!gStageLock || !gWdogCall || !gHoldCall || !gTraceCall || !gLogLock || !gSnapLock)
		return KERN_FAILURE;
	/*
	 * i915 stays off the display until XeGfx owns it, and gets the GuC explicitly: drm-kmod leaves enable_guc at -1 on
	 * FreeBSD ("GuC is known to be broken"), which on Meteor/Arrow Lake means no submission at all and a wedged GT
	 * (Legion N-225 to N-227). The guard can override both per experiment.
	 */
	strlcpy(xg_env, "compat.linuxkpi.i915_disable_display=1 compat.linuxkpi.i915_enable_guc=3", sizeof(xg_env));
	for (auto *o : gOids)
		sysctl_register_oid(o);
	IOService::publishResource("XeGfxArm");
	return KERN_SUCCESS;
}

extern "C" kern_return_t
_stop(kmod_info_t *, void *)
{
	if (gStage > 0)
		return KERN_FAILURE;	/* no teardown yet */
	for (auto *o : gOids)
		sysctl_unregister_oid(o);
	thread_call_free(gWdogCall);
	thread_call_cancel(gHoldCall);
	thread_call_free(gHoldCall);
	thread_call_cancel(gTraceCall);
	thread_call_free(gTraceCall);
	IOLockFree(gStageLock);
	IOSimpleLockFree(gLogLock);
	IOLockFree(gSnapLock);
	return KERN_SUCCESS;
}
