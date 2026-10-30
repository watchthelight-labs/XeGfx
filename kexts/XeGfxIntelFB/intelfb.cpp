// XeGfxIntelFramebuffer: Apple's IONDRVFramebuffer under a class name with "Intel" in it. CoreDisplay picks the GPU
// vendor from the IOFramebuffer class name, and IONDRVFramebuffer reads as no vendor, which turns IOPresentment off
// for WindowServer (I-108). Lives in the AuxKC because IONDRVSupport is in the system collection.
// <wtl> fork: nvgfx | author: watchthelight </wtl>
#include <IOKit/ndrvsupport/IONDRVFramebuffer.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <mach/mach_time.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#define XIFBLOG(fmt, ...) IOLog("XeGfxIntelFB: " fmt "\n", ##__VA_ARGS__)

// Boot guard as in NMIntelFB: each start counts in NVRAM, 120 s of uptime after start clears it, and two counted starts
// in a row make probe decline so IONDRVFramebuffer drives the panel again. Boot-arg -xeintelfboff declines always.
#define XIFB_GUARD_VAR "xeintelfb-starts"
// About This Mac shows "model" followed by the VRAM size; name the chip generation and graphics architecture
static const char *xifbModelName(uint16_t dev)
{
    switch (dev) {
    case 0x7d67: case 0x7d41: return "Intel Arrow Lake Xe-LPG";
    case 0x7d51: case 0x7dd1: return "Intel Arrow Lake Xe-LPG+";
    case 0x7d40: case 0x7d45: case 0x7d55: case 0x7dd5: return "Intel Meteor Lake Xe-LPG";
    default: return "Intel Xe Graphics";
    }
}
static unsigned xifbGuardStarts(IORegistryEntry *o)
{
    OSObject *v = o->copyProperty(XIFB_GUARD_VAR); OSData *d = OSDynamicCast(OSData, v);
    unsigned n = (d && d->getLength() >= 1) ? *(const unsigned char *)d->getBytesNoCopy() : 0;
    if (v) v->release();
    return n;
}
static bool xifbGuardTripped()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return false;
    bool t = xifbGuardStarts(o) >= 2; o->release(); return t;
}
static void xifbGuardCount()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    unsigned char n = (unsigned char)(xifbGuardStarts(o) + 1);
    if (OSData *d = OSData::withBytes(&n, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard: start %u without WindowServer so far (off after 2)", n); d->release(); }
    o->release();
}
static void xifbGuardClear()
{
    static bool done; if (done) return; done = true;
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    // removeProperty did not reach NVRAM (N-283); a zero count does, the same way the count itself is written
    unsigned char z = 0;
    if (OSData *d = OSData::withBytes(&z, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard cleared: still up 120 s after start"); d->release(); }
    o->release();
}

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; this build does not)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxIntelFB, "0.8", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }


// The panel's EDID over the eDP AUX channel (AUX A, display version 14 layout), which the firmware left powered for
// its own modeset. Without it macOS sees "Unknown Display" and none of the panel's overrides (name, HiDPI sizes) apply.
#define XIFB_AUX_CTL  0x64010
#define XIFB_AUX_DATA 0x64014
static volatile uint32_t *gRegs;
static uint32_t xr(uint32_t o) { return gRegs[o / 4]; }
static void xw(uint32_t o, uint32_t v) { gRegs[o / 4] = v; }
static int xifbAux(const uint8_t *tx, int ntx, uint8_t *rx, int nrx)
{
    for (int i = 0; i < 100 && (xr(XIFB_AUX_CTL) & (1u << 31)); i++) IODelay(100);
    if (xr(XIFB_AUX_CTL) & (1u << 31)) return -1;
    for (int i = 0; i < ntx; i += 4) {
        uint32_t v = 0;
        for (int j = 0; j < 4 && i + j < ntx; j++) v |= (uint32_t)tx[i + j] << (24 - 8 * j);
        xw(XIFB_AUX_DATA + i, v);
    }
    // send busy, done, irq, timeout/receive error clears, max timeout, size, sync pulses 18 and 32, power request kept
    uint32_t ctl = (1u << 31) | (1u << 30) | (1u << 29) | (1u << 28) | (3u << 26) | (1u << 25) | ((uint32_t)ntx << 20) |
                   ((18u - 1) << 5) | (32u - 1) | (1u << 19);
    xw(XIFB_AUX_CTL, ctl);
    uint32_t st = 0;
    for (int i = 0; i < 200; i++) { st = xr(XIFB_AUX_CTL); if (!(st & (1u << 31))) break; IODelay(50); }
    xw(XIFB_AUX_CTL, st | (1u << 30) | (1u << 28) | (1u << 25));
    if ((st & (1u << 31)) || (st & ((1u << 28) | (1u << 25)))) return -1;
    int n = (st >> 20) & 0x1f; if (n > nrx) n = nrx;
    for (int i = 0; i < n; i++) rx[i] = (uint8_t)(xr(XIFB_AUX_DATA + (i & ~3)) >> (24 - 8 * (i & 3)));
    return n;
}
// one I2C-over-AUX transfer to the EDID address 0x50, retried while the sink defers
static int xifbI2c(uint8_t req, const uint8_t *data, int nd, uint8_t *rx, int nrx)
{
    uint8_t tx[20] = { (uint8_t)(req << 4), 0, 0x50, (uint8_t)(nd ? nd - 1 : (nrx > 1 ? nrx - 2 : 0)) };
    int ntx = (nd || nrx > 1) ? 4 : 3;
    for (int i = 0; i < nd; i++) tx[ntx++] = data[i];
    for (int t = 0; t < 7; t++) {
        int n = xifbAux(tx, ntx, rx, nrx);
        if (n >= 1 && !(rx[0] & 0xf0)) return n;
        IODelay(500);
    }
    return -1;
}
static bool xifbReadEdid(uint8_t *edid, int *len)
{
    uint8_t off = 0, rx[17];
    if (xifbI2c(0x4, &off, 1, rx, 1) < 1) return false;    // write, middle of transaction: offset 0
    int want = 128;
    for (int pos = 0; pos < want; pos += 16) {
        if (xifbI2c(0x5, nullptr, 0, rx, 17) != 17) return false;
        bcopy(rx + 1, edid + pos, 16);
        if (pos == 112) want = edid[126] ? 256 : 128;       // base block plus at most one extension (no segment pointer)
    }
    xifbI2c(0x1, nullptr, 0, rx, 1);                        // stop
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (memcmp(edid, hdr, 8)) return false;
    for (int b = 0; b < want; b += 128) { uint8_t c = 0; for (int i = 0; i < 128; i++) c += edid[b + i]; if (c) return false; }
    *len = want;
    return true;
}
// pipe A's frame counter over about half a second: the refresh rate the firmware's mode runs at, in 16.16
static uint32_t xifbMeasureHz()
{
    const uint32_t fc = 0x70040;
    uint32_t a = xr(fc), b = a;
    for (int i = 0; i < 100 && (b = xr(fc)) == a; i++) IODelay(500);
    if (b == a) return 0;
    uint64_t t0 = mach_absolute_time(); IOSleep(500);
    uint32_t c = xr(fc); for (int i = 0; i < 100 && xr(fc) == c; i++) IODelay(100);
    uint64_t t1 = mach_absolute_time(), ns; absolutetime_to_nanoseconds(t1 - t0, &ns);
    uint32_t frames = xr(fc) - b;
    return ns ? (uint32_t)(((uint64_t)frames << 16) * 1000000000ull / ns) : 0;
}
// the EDID detailed timing nearest the measured rate, so the reported refresh is the panel's exact one
static uint32_t xifbSnapHz(const uint8_t *e, int len, uint32_t meas)
{
    uint32_t best = meas, bd = ~0u;
    auto dtd = [&](const uint8_t *d) {
        uint32_t clk = (d[0] | d[1] << 8) * 10000u; if (!clk) return;
        uint32_t ht = d[2] | (d[4] & 0xf0) << 4, hb = d[3] | (d[4] & 0x0f) << 8;
        uint32_t vt = d[5] | (d[7] & 0xf0) << 4, vb = d[6] | (d[7] & 0x0f) << 8;
        uint64_t tot = (uint64_t)(ht + hb) * (vt + vb); if (!tot) return;
        uint32_t hz = (uint32_t)(((uint64_t)clk << 16) / tot), d0 = hz > meas ? hz - meas : meas - hz;
        if (d0 < bd) { bd = d0; best = hz; }
    };
    for (int i = 0; i < 4; i++) dtd(e + 54 + 18 * i);
    if (len >= 256 && e[128] == 0x02) for (int o = e[130]; o >= 4 && o + 18 <= 127; o += 18) dtd(e + 128 + o);
    return (meas && bd > meas / 50) ? meas : best;    // more than 2 % off: keep the measurement
}

class XeGfxIntelFramebuffer : public IONDRVFramebuffer {
    OSDeclareDefaultStructors(XeGfxIntelFramebuffer)
    uint8_t fEdid[256]; int fEdidLen = 0; uint32_t fHz16 = 0;
public:
    IOService *probe(IOService *provider, SInt32 *score) override {
        int off = 0;
        if (PE_parse_boot_argn("-xeintelfboff", &off, sizeof off)) { XIFBLOG("off by boot-arg"); return nullptr; }
        if (xifbGuardTripped()) { XIFBLOG("boot guard tripped (two starts without WindowServer): declining"); return nullptr; }
        return IONDRVFramebuffer::probe(provider, score);
    }
    bool start(IOService *provider) override {
        xifbGuardCount();
        // System Profiler names the GPU from the PCI device's "model"; without it About This Mac shows a bare "Display"
        if (!provider->getProperty("model")) {
            IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
            const char *model = xifbModelName(pci ? pci->configRead16(kIOPCIConfigDeviceID) : 0);
            if (OSData *m = OSData::withBytes(model, (unsigned)strlen(model) + 1)) { provider->setProperty("model", m); m->release(); }
        }
        // System Profiler shows VRAM,totalMB of an Intel PCI device as "VRAM (Dynamic, Max)", else the 15 MB GOP
        // framebuffer. Same size ANV gives its heap: 3/4 of RAM above 4 GB, half below. xeintelfbvram=<MB> overrides.
        if (!provider->getProperty("VRAM,totalMB")) {
            uint32_t mb = 0;
            if (!PE_parse_boot_argn("xeintelfbvram", &mb, sizeof mb)) {
                uint64_t ram = 0; size_t len = sizeof ram;
                if (!sysctlbyname("hw.memsize", &ram, &len, nullptr, 0) && ram)
                    mb = (uint32_t)((ram <= (4ull << 30) ? ram / 2 : ram / 4 * 3) >> 20);
            }
            if (mb) { provider->setProperty("VRAM,totalMB", (unsigned long long)mb, 32); XIFBLOG("VRAM,totalMB %u", mb); }
        }
        // the guard is for boots that hang: still running 120 s after start clears it. IOGraphics keeps the
        // WindowServer-active attribute and WindowServer's connection skips newUserClient here (N-282 to N-284)
        static thread_call_t tc;
        if (!tc) tc = thread_call_allocate([](thread_call_param_t, thread_call_param_t) { xifbGuardClear(); }, nullptr);
        if (tc) { uint64_t dl; clock_interval_to_deadline(120, kSecondScale, &dl); thread_call_enter_delayed(tc, dl); }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            int off = 0;
            if (!PE_parse_boot_argn("-xeintelfbnoedid", &off, sizeof off))
                if (IOMemoryMap *m = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
                    gRegs = (volatile uint32_t *)m->getVirtualAddress();
                    if (xifbReadEdid(fEdid, &fEdidLen))
                        XIFBLOG("EDID %d B, maker %02x%02x product %02x%02x, %ux%u mm", fEdidLen, fEdid[8], fEdid[9], fEdid[11],
                                fEdid[10], fEdid[21] * 10, fEdid[22] * 10);
                    else { fEdidLen = 0; XIFBLOG("EDID: AUX A read failed, the display stays unnamed"); }
                    uint32_t meas = xifbMeasureHz();
                    fHz16 = fEdidLen ? xifbSnapHz(fEdid, fEdidLen, meas) : meas;
                    XIFBLOG("refresh measured %u.%02u Hz, reported %u.%02u Hz", meas >> 16, ((meas & 0xffff) * 100) >> 16,
                            fHz16 >> 16, ((fHz16 & 0xffff) * 100) >> 16);
                    gRegs = nullptr; m->release();
                }
        }
        return IONDRVFramebuffer::start(provider);
    }
    bool hasDDCConnect(IOIndex ci) override { return fEdidLen ? true : IONDRVFramebuffer::hasDDCConnect(ci); }
    IOReturn getDDCBlock(IOIndex ci, UInt32 block, IOSelect type, IOOptionBits opt, UInt8 *data, IOByteCount *len) override {
        if (!fEdidLen) return IONDRVFramebuffer::getDDCBlock(ci, block, type, opt, data, len);
        if (!data || !len || block < 1 || (int)(block * 128) > fEdidLen) return kIOReturnUnsupported;
        IOByteCount n = *len < 128 ? *len : 128; bcopy(fEdid + (block - 1) * 128, data, n); *len = n;
        return kIOReturnSuccess;
    }
    IOReturn getAttributeForConnection(IOIndex ci, IOSelect attr, uintptr_t *value) override {
        if (fEdidLen && attr == kConnectionSupportsHLDDCSense) return kIOReturnSuccess;
        return IONDRVFramebuffer::getAttributeForConnection(ci, attr, value);
    }
    // the panel's size and refresh, and ValidForHiRes so the 2x sizes reach Displays settings (I-059 on NVRMFB)
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        IOReturn r = IONDRVFramebuffer::getInformationForDisplayMode(mode, info);
        if (r != kIOReturnSuccess || !info) return r;
        if (fHz16) info->refreshRate = fHz16;
        if (fEdidLen && fEdid[21] && fEdid[22]) { info->imageWidth = fEdid[21] * 10; info->imageHeight = fEdid[22] * 10; }
        info->flags |= kDisplayModeValidForHiResFlag;
        return r;
    }
};

OSDefineMetaClassAndStructors(XeGfxIntelFramebuffer, IONDRVFramebuffer)
