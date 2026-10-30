/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "nvrm_off.h"
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>          // clock_get_uptime/nanoseconds_to_absolutetime: the 0x980 rescan throttle
#include <libkern/OSAtomic.h>
#define FBLOG(fmt, ...) IOLog("NVRM-agdc: " fmt "\n", ##__VA_ARGS__)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMX_XEGFX
// the same AGDC for the Intel GPU over XeGfx (branch xe): IOPresentment pairs a framebuffer with its display pipe only
// through an AGDC service on the GPU, so without one WindowServer finds no pipe for the boot framebuffer (N-249).
// Own class, bundle and sysctl names, because NVRMAGDC loads on the same machine.
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxAGDC, "0.1.1", _start, _stop) }
#define NVDADeviceControl XeGfxDeviceControl
#define NVDAGDCLoader     XeGfxAGDCLoader
#define AGDC_FB_CLASS     "IONDRVFramebuffer"
#define AGDC_VENDOR       "Intel"
#define AGDC_VENDOR_ID    0x8086
#define nvrmfb_agdc xegfx_agdc
#define sysctl__debug_nvrmfb_agdc sysctl__debug_xegfx_agdc
#define nvrmfb_agdc_cmds xegfx_agdc_cmds
#define sysctl__debug_nvrmfb_agdc_cmds sysctl__debug_xegfx_agdc_cmds
#define nvrmfb_agdc_refused xegfx_agdc_refused
#define sysctl__debug_nvrmfb_agdc_refused sysctl__debug_xegfx_agdc_refused
#define nvrmfb_agdc_last_refused xegfx_agdc_last_refused
#define sysctl__debug_nvrmfb_agdc_last_refused sysctl__debug_xegfx_agdc_last_refused
#define nvrmfb_agdc_k5 xegfx_agdc_k5
#define sysctl__debug_nvrmfb_agdc_k5 sysctl__debug_xegfx_agdc_k5
#define nvrmfb_agdc_k5_link xegfx_agdc_k5_link
#define sysctl__debug_nvrmfb_agdc_k5_link sysctl__debug_xegfx_agdc_k5_link
#define nvrmfb_agdc_k5_caps xegfx_agdc_k5_caps
#define sysctl__debug_nvrmfb_agdc_k5_caps sysctl__debug_xegfx_agdc_k5_caps
#define nvrmfb_agdc_k5_why xegfx_agdc_k5_why
#define sysctl__debug_nvrmfb_agdc_k5_why sysctl__debug_xegfx_agdc_k5_why
#define nvrmfb_agdc_fbmap xegfx_agdc_fbmap
#define sysctl__debug_nvrmfb_agdc_fbmap sysctl__debug_xegfx_agdc_fbmap
#define nvrmfb_agdc_rescans xegfx_agdc_rescans
#define sysctl__debug_nvrmfb_agdc_rescans sysctl__debug_xegfx_agdc_rescans
#define nvrmfb_agdc_maxfb xegfx_agdc_maxfb
#define sysctl__debug_nvrmfb_agdc_maxfb sysctl__debug_xegfx_agdc_maxfb
#define nvrmfb_agdc_k5_asked xegfx_agdc_k5_asked
#define sysctl__debug_nvrmfb_agdc_k5_asked sysctl__debug_xegfx_agdc_k5_asked
#define nvrmfb_agdc_k5_link_mask xegfx_agdc_k5_link_mask
#define sysctl__debug_nvrmfb_agdc_k5_link_mask sysctl__debug_xegfx_agdc_k5_link_mask
#define nvrmfb_agdc_k5_caps_mask xegfx_agdc_k5_caps_mask
#define sysctl__debug_nvrmfb_agdc_k5_caps_mask sysctl__debug_xegfx_agdc_k5_caps_mask
#define nvrmfb_agdc_k5_last_refused_idx xegfx_agdc_k5_last_refused_idx
#define sysctl__debug_nvrmfb_agdc_k5_last_refused_idx sysctl__debug_xegfx_agdc_k5_last_refused_idx
#define nvrmfb_agdc_client_ack xegfx_agdc_client_ack
#define sysctl__debug_nvrmfb_agdc_client_ack sysctl__debug_xegfx_agdc_client_ack
#define nvrmfb_agdc_client_set xegfx_agdc_client_set
#define sysctl__debug_nvrmfb_agdc_client_set sysctl__debug_xegfx_agdc_client_set
#define nvrmfb_agdc_fbcapex xegfx_agdc_fbcapex
#define sysctl__debug_nvrmfb_agdc_fbcapex sysctl__debug_xegfx_agdc_fbcapex
#define nvrmfb_agdc_fbcapex_asked xegfx_agdc_fbcapex_asked
#define sysctl__debug_nvrmfb_agdc_fbcapex_asked sysctl__debug_xegfx_agdc_fbcapex_asked
#define nvrmfb_agdc_fbcapex_served xegfx_agdc_fbcapex_served
#define sysctl__debug_nvrmfb_agdc_fbcapex_served sysctl__debug_xegfx_agdc_fbcapex_served
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVRMAGDC, "0.1", _start, _stop) }
#define AGDC_FB_CLASS     "NVRMNVDAFramebuffer"
#define AGDC_VENDOR       "NVIDIA"
#define AGDC_VENDOR_ID    0x10de
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }
#include "AppleGraphicsDeviceControl_re.h"
#include "nvrm-agdc-k5.h"
#pragma pack(push, 1)
struct NVAGDCVendorInfo {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
struct NVAGDCGPUCapability {
    uint64_t portMask;
    uint64_t portFeatureA;
    uint64_t portFeatureB;
    uint64_t portFeatureC;
    uint32_t countA, countB, countC, countD;
    uint32_t maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(NVAGDCVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(NVAGDCGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");

static int    gAgdcK5 = 0;
static SInt64 gAgdcK5Link = 0, gAgdcK5Caps = 0;
static int    gAgdcK5Why = 0;
static SInt64 gAgdcCmds = 0, gAgdcUnsupported = 0;
static int    gAgdcLastRefused = 0;
static int    gAgdcLogged = 0;
// WindowServer's 4-byte "client set" (0x2001). thanhnndev's AGDC trace (nvidia-macos-driver e196ab19) saw CoreDisplay
// submit display-pipe transactions only once it was accepted; Intel-only until an NVIDIA boot shows the same.
#ifdef NMX_XEGFX
static int    gAgdcClientAck = 1;
#else
static int    gAgdcClientAck = 0;
#endif
static SInt64 gAgdcClientSet = 0;
#ifdef NMX_XEGFX
// <wtl> Switch for the extra 0x711 answers below (xegfxagdc_fbcapex=1 or debug.xegfx_agdc_fbcapex=1); off, the replies are
// the plane-scaler one alone. The masks show which types WindowServer asked for (bit t = type t) and which we served. </wtl>
static int    gAgdcFbcapex = 0;
static int    gAgdcFbcapexAsked = 0, gAgdcFbcapexServed = 0;
#endif
// Slot-table rescans done while answering 0x980 (see nvdaAgdcRescanForReply).
static SInt64 gAgdcRescans = 0;

enum { kAgdcMaxFB = 4 };
static IOService *gAgdcFB[kAgdcMaxFB];
static unsigned  gAgdcNFB = 0;
enum { kAgdcFBMapBytes = 3 + kAgdcMaxFB * 25 + 52 + 1 };
static_assert(kAgdcMaxFB >= 1 && kAgdcMaxFB <= 8, "IOPresentment's map holds 8 (kext 0x31b3), and kAgdcFBMapBytes assumes one digit for n and each slot index");
static char   gAgdcFBMap[kAgdcFBMapBytes] = "never scanned";
static int    gAgdcMaxFBOut = 0;
static int    gAgdcK5Asked = 0, gAgdcK5LinkMask = 0, gAgdcK5CapsMask = 0;
static int    gAgdcK5LastRefusedIdx = -1;

class NVDADeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NVDADeviceControl)
public:
    IOService *fPci; IOService *fFB;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDADeviceControl, AppleGraphicsDeviceControl)

static IOReturn nvdaK5Answer(IOService *fb, unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize)
{
    IOService *slot[kAgdcMaxFB]; unsigned nh = gAgdcNFB;
    if (nh == 0 || nh > kAgdcMaxFB) { slot[0] = fb; nh = 1; } else for (unsigned i = 0; i < nh; i++) slot[i] = gAgdcFB[i];
    uint32_t t[kAgdcMaxFB][NVK5_TIMING_WORDS]; struct nvk5_head h[kAgdcMaxFB];
    for (unsigned i = 0; i < nh; i++) {
        unsigned long tBytes = 0; bool have = false;
        OSObject *o = slot[i] ? slot[i]->copyProperty("NVRMTiming") : NULL; OSData *d = OSDynamicCast(OSData, o);
        if (d) { have = true; tBytes = d->getLength(); bzero(t[i], sizeof t[i]); if (tBytes == sizeof t[i]) memcpy(t[i], d->getBytesNoCopy(), sizeof t[i]); }
        OSSafeReleaseNULL(o);
#ifdef NMX_XEGFX
        // Apple's boot framebuffer has no timing record: the built-in panel's native mode, as NVRMFB publishes it for
        // the same panel in Discrete (N-182 NVRMTiming), which the firmware also sets on the iGPU
        if (!have) {
            static const uint32_t panel[NVK5_TIMING_WORDS] = { NVK5_TIMING_TAG, 782710000u, 2560, 2640, 2672, 2720,
                                                               1600, 1670, 1678, 1744, 165000, 0, 1 };
            memcpy(t[i], panel, sizeof panel); have = true; tBytes = sizeof panel;
        }
#endif
        h[i].t = have ? t[i] : NULL; h[i].tBytes = tBytes;
    }
    int why = 0; int32_t idx = -1;
    int r = nvk5_answer_heads(cmd, in, inSize, out, outSize ? *outSize : 0, h, nh, &why, &idx);
    const UInt32 bit = (idx >= 0 && idx < 32) ? (1u << idx) : 0;
    if (bit) OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5Asked);
    if (r == NVK5_R_LINK) { OSAddAtomic64(1, &gAgdcK5Link); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5LinkMask); return kIOReturnSuccess; }
    if (r == NVK5_R_CAPS) { OSAddAtomic64(1, &gAgdcK5Caps); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5CapsMask); return kIOReturnSuccess; }
    if (r == NVK5_R_UNSUPPORTED) return kIOReturnUnsupported;
    if (r == NVK5_R_NOTFOUND && idx >= 0 && (unsigned)idx < nh && !slot[idx]) why = 0x30;
    gAgdcK5Why = why; gAgdcK5LastRefusedIdx = idx;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC K5: command %#x index %d NOT answered (why %#x, %u slot(s)) [%d/40]", cmd, idx, why, nh, gAgdcLogged); }
    return r == NVK5_R_BADARG ? kIOReturnBadArgument : kIOReturnNotFound;
}

static void nvdaAgdcScan(IOService *pci);
// WindowServer reads AGDC command 0x980 when it (re)starts and builds one display pipe per entry in the reply. The
// slot table used to be scanned only once, when AGDC came up, so a framebuffer that started later -- a monitor
// plugged in after boot -- was never in the answer: WindowServer then built a single display pipe, the second
// display's composited frames had no pipe to go to, and the panel showed only its cursor while `screencapture -D 2`
// showed a full desktop (iop_ok_h0 grew, iop_ok_h1 stayed 0). Re-scan before answering, at most once a second and
// logging only when the count changes, so the reply is never stale. This runs in the vendor-call context, which
// holds no registry lock, and nvdaAgdcScan only matches services and reads their properties.
static void nvdaAgdcRescanForReply(IOService *pci)
{
    if (!pci) return;
    static uint64_t last = 0;
    uint64_t now = 0; clock_get_uptime(&now);
    uint64_t per = 0; nanoseconds_to_absolutetime(1000000000ull, &per);
    if (last && now - last < per) return;
    last = now;
    const unsigned before = gAgdcNFB;
    nvdaAgdcScan(pci);
    OSAddAtomic64(1, &gAgdcRescans);
    if (gAgdcNFB != before)
        FBLOG("AGDC: 0x980 rescan -> %u framebuffer slot(s) (was %u)", gAgdcNFB, before);
}

#ifdef NMX_XEGFX
// <wtl>
// 0x711 types 1 (colour matrix), 2 (gamma), 4 (linearization) and 8 (pipeline scaler) for framebuffer 0, entries as in
// jwx29's meteor-lake-local fork of this driver (9721b2fd82, 49051fa92b). Needs K5 and the switch; the scaler caps
// only advertise, XeGfxAccel does no scaling. Anything else goes on to nvdaK5Answer.
// </wtl>
static bool xegfxFbcapexAnswer(unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize)
{
    if (!in || !out || !outSize || inSize != sizeof(NVAGDCFBExtCap) || *outSize != sizeof(NVAGDCFBExtCap)) return false;
    const uint32_t idx = ((const NVAGDCFBExtCap *)in)->fbIndex, type = ((const NVAGDCFBExtCap *)in)->type;
    if (type < 32) OSBitOrAtomic(1u << type, (volatile UInt32 *)&gAgdcFbcapexAsked);
    if (!gAgdcK5 || !gAgdcFbcapex || idx != 0 || (type != 1 && type != 2 && type != 4 && type != 8)) return false;
    NVAGDCFBExtCap *c = (NVAGDCFBExtCap *)out; nvk5_zero(c, sizeof *c);   // in may be out: idx and type are read already
    c->fbIndex = idx; c->type = type; c->count = 1;
    uint8_t *e = c->entry[0];
    nvk5_put32(e, 0x000, 0x80004); nvk5_put32(e, 0x004, 1);   // plane 0
    if (type == 1) {
        nvk5_put32(e, 0x010, 1); nvk5_put32(e, 0x024, 0xc0000000); nvk5_put32(e, 0x028, 0x40000000);   // floats -2.0 and 2.0
    } else if (type == 2) {
        nvk5_put32(e, 0x010, 0x200); nvk5_put32(e, 0x024, 0x3f800000); nvk5_put32(e, 0x028, 1024);   // 1024 entries, max 1.0
    } else if (type == 4) {
        nvk5_put32(e, 0x000, 0x00100000); nvk5_put32(e, 0x00c, 0x41855555);   // non-atomic apply period 16.67 ms
        nvk5_put32(e, 0x010, 8); nvk5_put32(e, 0x024, 0x3f800000); nvk5_put32(e, 0x028, 1024);   // table type 8, max 1.0, 1024 entries
    } else {
        nvk5_put32(e, 0x000, 0xc5); nvk5_put32(e, 0x01c, 1);
        nvk5_put32(e, 0x024, 64); nvk5_put32(e, 0x028, 64); nvk5_put32(e, 0x02c, 4096); nvk5_put32(e, 0x030, 4096);   // 64 and 4096, as he has them
        nvk5_put32(e, 0x034, 0x301); nvk5_put32(e, 0x038, 2);
        nvk5_put32(e, 0x1a4, 1); nvk5_put32(e, 0x1ac, 64); nvk5_put32(e, 0x1b0, 64); nvk5_put32(e, 0x1b4, 4096); nvk5_put32(e, 0x1b8, 4096);
        nvk5_put32(e, 0x1bc, 0x301); nvk5_put32(e, 0x1c0, 2);
    }
    OSBitOrAtomic(1u << type, (volatile UInt32 *)&gAgdcFbcapexServed);
    return true;
}
#endif

IOReturn NVDADeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                     unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args)
{
    OSAddAtomic64(1, &gAgdcCmds);
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCVendorInfo)) return kIOReturnBadArgument;
        NVAGDCVendorInfo *v = (NVAGDCVendorInfo *)out; bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, AGDC_VENDOR, sizeof v->vendor); v->vendorID = AGDC_VENDOR_ID; v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCGPUCapability)) return kIOReturnBadArgument;
        nvdaAgdcRescanForReply(fPci);   // a framebuffer that started after boot has to be in this reply
        NVAGDCGPUCapability *c = (NVAGDCGPUCapability *)out; bzero(c, sizeof *c);
        unsigned n = gAgdcNFB;
        if (n == 0 || n > kAgdcMaxFB) {
            c->portMask = 1ull << 1;
            c->countA = c->countB = c->countC = c->countD = 1;
            c->maxFramebuffers = 1; c->pciDevice = fPci; c->framebuffer[0] = fFB;
        } else {
            for (unsigned i = 0; i < n; i++) { c->framebuffer[i] = gAgdcFB[i]; c->portMask |= 1ull << (1 + i); }
            c->countA = c->countB = c->countC = c->countD = n;
            c->maxFramebuffers = n; c->pciDevice = fPci;
        }
        gAgdcMaxFBOut = (int)c->maxFramebuffers;
        return kIOReturnSuccess;
    }
    if (cmd == 0x2001 && gAgdcClientAck) {
        if (!in || inSize != 4) return kIOReturnBadArgument;
        OSAddAtomic64(1, &gAgdcClientSet);
        return kIOReturnSuccess;
    }
#ifdef NMX_XEGFX
    if (cmd == NVK5_CMD_FBCAPEX && xegfxFbcapexAnswer(in, inSize, out, outSize)) return kIOReturnSuccess;
#endif
    if (gAgdcK5 && (cmd == NVK5_CMD_LINKCONFIG || cmd == NVK5_CMD_FBCAPEX)) {
        IOReturn k5 = nvdaK5Answer(fFB, cmd, in, inSize, out, outSize);
        if (k5 != kIOReturnUnsupported) return k5;
    }
    OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC: command %#x (in %lu bytes) -> Unsupported [%d/40]", cmd, inSize, gAgdcLogged); }
    return kIOReturnUnsupported;
}

static int gAgdcState = 0; static NVDADeviceControl *gAgdc = nullptr; static thread_call_t gAgdcCall = nullptr; static int gAgdcWant = 0;
static IOService *gAgdcFBRef = nullptr;
static void nvdaAgdcDropTable(void)
{
    unsigned n = gAgdcNFB; gAgdcNFB = 0;
    __asm__ __volatile__("" ::: "memory");
    for (unsigned i = 0; i < kAgdcMaxFB; i++) { IOService *f = gAgdcFB[i]; gAgdcFB[i] = nullptr; if (f) f->release(); }
    if (n) snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "released (the last table had %u slot(s))", n);
}
static void nvdaAgdcDropFB(void) { if (gAgdcFBRef) { gAgdcFBRef->release(); gAgdcFBRef = nullptr; } nvdaAgdcDropTable(); }
static void nvdaAgdcTargets(IOService **pci, IOService **fb)
{
    *pci = nullptr; *fb = nullptr;
    OSDictionary *m = IOService::serviceMatching(AGDC_FB_CLASS); if (!m) return;
    IOService *f = IOService::copyMatchingService(m); m->release(); if (!f) return;
    IOService *p = f->getProvider(); int hops = 0;
    while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
    if (!p || !p->metaCast("IOPCIDevice")) { f->release(); return; }
#ifdef NMX_XEGFX
    { OSData *v = OSDynamicCast(OSData, p->getProperty("vendor-id"));
      if (!v || v->getLength() < 2 || *(const uint16_t *)v->getBytesNoCopy() != AGDC_VENDOR_ID) { f->release(); return; } }
#endif
    gAgdcFBRef = f; *fb = f; *pci = p;
}

static int gAgdcScanLogged = 0;
#define SCANLOG(fmt, ...) do { if (gAgdcScanLogged < 8) { gAgdcScanLogged++; FBLOG("AGDC scan: " fmt " [%d/8]", ##__VA_ARGS__, gAgdcScanLogged); } } while (0)
static void nvdaAgdcScan(IOService *pci)
{
    nvdaAgdcDropTable(); gAgdcScanLogged = 0;
    IOService *tab[kAgdcMaxFB] = {}; unsigned seen = 0, noidx = 0, range = 0, dup = 0, other = 0, hole = 0;
    OSDictionary *m = IOService::serviceMatching(AGDC_FB_CLASS);
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr; OSSafeReleaseNULL(m);
    if (!it) { snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=0 (no iterator: the earlier one-head reply)"); FBLOG("AGDC: slot table %s", gAgdcFBMap); return; }
    OSObject *o;
    while (seen < 16 && (o = it->getNextObject())) {
        seen++;
        IOService *f = OSDynamicCast(IOService, o); if (!f) continue;
        unsigned long long rid = f->getRegistryEntryID();
        IOService *p = f->getProvider(); int hops = 0;
        while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
        if (p != pci) { other++; SCANLOG("framebuffer id %#llx is under another PCI device: not ours to report", rid); continue; }
        OSObject *io = f->copyProperty("IOFBDependentIndex"); OSNumber *num = OSDynamicCast(OSNumber, io);
        long long ix = num ? (long long)num->unsigned32BitValue() : -1; OSSafeReleaseNULL(io);
        if (ix < 0) { noidx++; SCANLOG("framebuffer id %#llx has no IOFBDependentIndex number: left out", rid); continue; }
        if (ix >= kAgdcMaxFB) { range++; SCANLOG("framebuffer id %#llx IOFBDependentIndex %lld >= %d: left out", rid, ix, (int)kAgdcMaxFB); continue; }
        if (tab[ix]) { dup++; SCANLOG("IOFBDependentIndex %lld claimed twice: id %#llx kept, id %#llx left out", ix, (unsigned long long)tab[ix]->getRegistryEntryID(), rid); continue; }
        f->retain(); tab[ix] = f;
    }
    it->release();
    unsigned n = 0; while (n < kAgdcMaxFB && tab[n]) n++;
    for (unsigned i = n; i < kAgdcMaxFB; i++) if (tab[i]) {
        hole++; SCANLOG("slot %u (id %#llx) sits beyond a hole at slot %u: left out", i, (unsigned long long)tab[i]->getRegistryEntryID(), n);
        tab[i]->release(); tab[i] = nullptr;
    }
    for (unsigned i = 0; i < n; i++) gAgdcFB[i] = tab[i];
    gAgdcNFB = n;
    int w = snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=%u", n);
    for (unsigned i = 0; i < n && w > 0 && (unsigned)w < sizeof gAgdcFBMap; i++)
        w += snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " [%u]id %#llx", i, (unsigned long long)gAgdcFB[i]->getRegistryEntryID());
    if (w > 0 && (unsigned)w < sizeof gAgdcFBMap && (noidx | range | dup | other | hole))
        snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " (left out: noidx %u range %u dup %u gpu %u hole %u)", noidx, range, dup, other, hole);
    FBLOG("AGDC: slot table %s (%u framebuffer node(s) seen)", gAgdcFBMap, seen);
}

static SInt32 gAgdcBusy = 0;
static void nvdaAgdcWorkBody(void);
static void nvdaAgdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcBusy); nvdaAgdcWorkBody(); OSDecrementAtomic(&gAgdcBusy);
}
static void nvdaAgdcWorkBody(void)
{
    if (gAgdcWant == 1 && !gAgdc) {
        IOService *pci = nullptr, *fb = nullptr; nvdaAgdcTargets(&pci, &fb);
        if (!pci || !fb) { gAgdcState = -1; FBLOG("AGDC: no PCI device or no framebuffer yet (pci %p fb %p)", pci, fb); return; }
        nvdaAgdcScan(pci);
        NVDADeviceControl *d = OSTypeAlloc(NVDADeviceControl);
        if (!d) { gAgdcState = -2; FBLOG("AGDC: alloc failed"); nvdaAgdcDropFB(); return; }
        if (!d->init(nullptr)) { gAgdcState = -3; FBLOG("AGDC: init failed"); d->release(); nvdaAgdcDropFB(); return; }
        d->fPci = pci; d->fFB = fb;
        if (!d->attach(pci)) { gAgdcState = -4; FBLOG("AGDC: attach(%s) failed", pci->getName()); d->release(); nvdaAgdcDropFB(); return; }
        if (!d->start(pci)) { gAgdcState = -5; FBLOG("AGDC: AppleGraphicsDeviceControl::start failed (vendor calls so far %lld)", gAgdcCmds); d->detach(pci); d->release(); nvdaAgdcDropFB(); return; }
        gAgdc = d; gAgdcState = 1;
        FBLOG("AGDC: UP on %s — %u framebuffer slot(s), maxFramebuffers %d; vendor calls %lld, refused %lld (last %#x)", pci->getName(), gAgdcNFB, gAgdcMaxFBOut, gAgdcCmds, gAgdcUnsupported, gAgdcLastRefused);
    } else if (gAgdcWant == 0 && gAgdc) {
        NVDADeviceControl *d = gAgdc; gAgdc = nullptr;
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous); d->release(); nvdaAgdcDropFB(); gAgdcState = 2;
        FBLOG("AGDC: terminate -> %d (a reboot is the certain way back; this is the polite one)", ok);
    }
}
static int nvrmfb_agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAgdcState; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
#ifdef NMX_XEGFX
    // a terminated client on the live iGPU is the FLR risk the XeGfx notes warn about: up only, a reboot takes it down
    if (v == 0) return EPERM;
#endif
    gAgdcWant = v;
    if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
    if (!gAgdcCall) return ENOMEM;
    thread_call_enter(gAgdcCall); return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvrmfb_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvrmfb_agdc_sysctl, "I",
            "write 1 = create our AppleGraphicsDeviceControl object, 0 = terminate it; reads the state (1 UP, <0 failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_cmds,    CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCmds,        "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcUnsupported, "AGDC vendor calls answered Unsupported");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastRefused, 0, "last AGDC command refused");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5,      CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcK5,     0, "K5: 1 = answer kAGDCLinkConfig + plane-scaler capabilities (0 after every boot)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_link, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Link,    "K5: kAGDCLinkConfig queries answered");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_caps, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Caps,    "K5: plane-scaler capability queries answered");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_why,  CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Why,  0, "K5: why the last K5 query was not answered");
SYSCTL_STRING(_debug, OID_AUTO, nvrmfb_agdc_fbmap, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFBMap, sizeof gAgdcFBMap, "the slot table our 0x980 reports: n, then [i]id per IOFBDependentIndex");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_client_ack, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcClientAck, 0, "1 = accept WindowServer's 0x2001 client set");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_client_set, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcClientSet, "0x2001 client-set calls accepted");
#ifdef NMX_XEGFX
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_fbcapex, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcFbcapex, 0, "1 = also answer 0x711 types 1, 2, 4, 8 (needs K5; boot-arg xegfxagdc_fbcapex sets it at load)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_fbcapex_asked, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcFbcapexAsked, 0, "bit t = a 0x711 request named type t");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_fbcapex_served, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcFbcapexServed, 0, "bit t = a 0x711 request for type t answered by the fbcapex switch");
#endif
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_rescans, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcRescans, "slot-table rescans done while answering 0x980 (a framebuffer that started after boot)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_maxfb,   CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcMaxFBOut, 0, "maxFramebuffers in our last 0x980 reply (0 = never asked)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_asked, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Asked, 0, "bit i = a 0x921/0x711 named index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_link_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LinkMask, 0, "bit i = kAGDCLinkConfig answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_caps_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5CapsMask, 0, "bit i = the plane-scaler capability answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_last_refused_idx, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LastRefusedIdx, 0, "the index the last BADARG/NOTFOUND refusal named (-1 none); an Unsupported 0x711 type shows in k5_asked only");
static void nvdaAgdcRemoveSysctl(void)
{
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_client_ack); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_client_set);
#ifdef NMX_XEGFX
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbcapex); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbcapex_asked);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbcapex_served);
#endif
}
static void nvdaAgdcInstallSysctl(void)
{
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_client_ack); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_client_set);
#ifdef NMX_XEGFX
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbcapex); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbcapex_asked);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbcapex_served);
#endif
}

class NVDAGDCLoader : public IOService {
    OSDeclareDefaultStructors(NVDAGDCLoader)
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDAGDCLoader, IOService)
bool NVDAGDCLoader::start(IOService *provider)
{
#ifndef NMX_XEGFX
    { if (nvrm_driver_off()) return false; }
#endif
    if (!IOService::start(provider)) return false;
#ifdef NMX_XEGFX
    { int v = 0; if (PE_parse_boot_argn("xegfxagdc_fbcapex", &v, sizeof v)) gAgdcFbcapex = v != 0; }   // <wtl> A/B switch, off by default </wtl>
#endif
    nvdaAgdcInstallSysctl(); FBLOG("loaded; idle until `sysctl debug.%s=1`", sysctl__debug_nvrmfb_agdc.oid_name); return true;
}
void NVDAGDCLoader::stop(IOService *provider)
{
    nvdaAgdcRemoveSysctl();
    if (gAgdcCall) {
        thread_call_cancel(gAgdcCall); int spins = 0;
        while (gAgdcBusy && spins++ < 200) IOSleep(10);
        if (gAgdcBusy) FBLOG("stop: the worker is STILL running after 2 s — leaving its thread call allocated rather than freeing it under it");
        else { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
    }
    if (gAgdc && !gAgdcBusy) { gAgdcWant = 0; nvdaAgdcWorkBody(); }
    IOService::stop(provider);
}
