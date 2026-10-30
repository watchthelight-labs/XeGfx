// mtlbench: how long one compositor-sized frame takes on the Intel Metal device (XeGfxAccel through the NvGfx plugin on
// ANV). N-263/N-265: WindowServer on the Intel GPU managed about 2.5 frames a second and CoreAnimation timed out its
// fences. Each test runs on a 2560x1600 IOSurface texture, commit + waitUntilCompleted, and prints min/avg/max ms:
// clear (render pass only), blit (texture to texture), draw (full-screen quad sampling a texture, blended).
// MTLBENCH_N=iterations (default 30). MTLBENCH_PRIVATE=1: Private textures instead of IOSurfaces, which separates
// the plugin's IOSurface copies (Intel: private per process, copied out after every GPU write) from the GPU work.
// NM_TEST_PLUGIN=<NVMTLDriver build>: that plugin in this process only (as mtlprobe). The clear and blit results are
// also checked through the IOSurfaces' base addresses (surface-out, and surface-in for the CPU-filled blit source).
// MTLBENCH_PITCH=1: only the IOSurface pitch cases (pitch-wide: RGBA16Float 5632x64, row 45056; pitch-odd: R8Unorm 642x64,
// row 656, not a multiple of 32), each a CPU-filled source blitted to a second surface and compared row by row.
// Exit 0 when every command buffer completed and the pixels match.
// <wtl> project: xegfx | author: watchthelight </wtl>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <time.h>

static const NSUInteger kW = 2560, kH = 1600;

static IOSurfaceRef g_surf[2]; static int g_nsurf;

static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

// outSurf == NULL: the surface goes into g_surf (at most two, for the checks in main). Otherwise the caller owns it.
static id<MTLTexture> surface_texture(id<MTLDevice> dev, MTLTextureUsage usage, NSUInteger w, NSUInteger h, NSUInteger bpe,
                                      NSUInteger bytesPerRow, uint32_t fourcc, MTLPixelFormat mf, IOSurfaceRef *outSurf)
{
    if (!outSurf && getenv("MTLBENCH_PRIVATE") && getenv("MTLBENCH_PRIVATE")[0] == '1') {
        MTLTextureDescriptor *pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mf width:w height:h mipmapped:NO];
        pd.usage = usage;
        pd.storageMode = MTLStorageModePrivate;
        return [dev newTextureWithDescriptor:pd];
    }
    NSDictionary *p = @{(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @(bpe),
                        (id)kIOSurfaceBytesPerRow: @(bytesPerRow), (id)kIOSurfacePixelFormat: @(fourcc)};
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    if (!s) return nil;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mf width:w height:h mipmapped:NO];
    td.usage = usage;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:s plane:0];
    if (outSurf) *outSurf = s;
    else if (g_nsurf < 2) g_surf[g_nsurf++] = s; else CFRelease(s);
    return t;
}

static int report(const char *name, double *t, int n, int bad)
{
    double mn = 1e9, mx = 0, sum = 0;
    for (int i = 0; i < n; i++) { mn = t[i] < mn ? t[i] : mn; mx = t[i] > mx ? t[i] : mx; sum += t[i]; }
    printf("bench %-6s n %d  min %.2f  avg %.2f  max %.2f ms%s\n", name, n, mn, sum / n, mx,
           bad ? "  (some command buffers did not complete)" : "");
    return bad;
}

// MTLBENCH_PITCH=1 (pitch cases only): IOSurface row pitches the plugin's zero-copy path treats differently. The source is
// filled by the CPU with a per-row pattern, one blit copies it to a second surface of the same layout, and every row's
// active bytes (w * bpe, not the padding) are compared. The row is what IOSurface really gave, which can differ from the ask.
// Returns 0 OK, 5 command buffer incomplete, 6 wrong bytes, 3 no surface or texture.
static int pitch_case(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *name, NSUInteger w, NSUInteger h, NSUInteger bpe,
                      NSUInteger want_row, uint32_t fourcc, MTLPixelFormat mf, unsigned salt)
{
    IOSurfaceRef ss = NULL, ds = NULL;
    id<MTLTexture> src = surface_texture(dev, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, w, h, bpe, want_row, fourcc, mf, &ss);
    id<MTLTexture> dst = surface_texture(dev, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, w, h, bpe, want_row, fourcc, mf, &ds);
    if (!src || !dst) { printf("%s: no IOSurface texture\n", name); if (ss) CFRelease(ss); if (ds) CFRelease(ds); return 3; }
    size_t row = IOSurfaceGetBytesPerRow(ss), drow = IOSurfaceGetBytesPerRow(ds), act = (size_t)w * bpe;
    printf("%s: %lux%lu bpe %lu  bytesPerRow asked %lu got %zu (dst %zu)  row %% 32 = %zu\n", name, (unsigned long)w,
           (unsigned long)h, (unsigned long)bpe, (unsigned long)want_row, row, drow, row % 32);
    if (row != want_row) printf("%s: pitch-rounded, IOSurface changed the row from %lu to %zu; testing with %zu\n", name,
                                (unsigned long)want_row, row, row);

    IOSurfaceLock(ss, 0, NULL);
    uint8_t *sb = IOSurfaceGetBaseAddress(ss);
    for (NSUInteger y = 0; y < h; y++) for (size_t x = 0; x < act; x++) sb[y * row + x] = (uint8_t)((x * 7 + y * 13 + salt) & 0xff);
    IOSurfaceUnlock(ss, 0, NULL);
    IOSurfaceLock(ds, 0, NULL);
    memset(IOSurfaceGetBaseAddress(ds), 0, drow * h);
    IOSurfaceUnlock(ds, 0, NULL);

    double t0 = now_ms();
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(w, h, 1) toTexture:dst destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    double el = now_ms() - t0;
    int fails = cb.status != MTLCommandBufferStatusCompleted;
    report(name, &el, 1, fails);
    if (fails) printf("%s: command buffer status %ld%s%s\n", name, (long)cb.status, cb.error ? " " : "",
                      cb.error ? cb.error.localizedDescription.UTF8String : "");

    unsigned badrows = 0; long fx = -1, fy = -1;
    IOSurfaceLock(ds, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *db = IOSurfaceGetBaseAddress(ds);
    for (NSUInteger y = 0; y < h; y++) {
        int rowbad = 0;
        for (size_t x = 0; x < act; x++)
            if (db[y * drow + x] != (uint8_t)((x * 7 + y * 13 + salt) & 0xff)) {
                if (fx < 0) { fx = (long)x; fy = (long)y; }
                rowbad = 1; break;
            }
        badrows += rowbad;
    }
    IOSurfaceUnlock(ds, kIOSurfaceLockReadOnly, NULL);
    if (badrows) printf("%s: %u of %lu rows wrong, first at byte x %ld (element %ld) y %ld\n", name, badrows, (unsigned long)h,
                        fx, fx / (long)bpe, fy);
    else if (!fails) printf("%s: all %lu rows match\n", name, (unsigned long)h);
    CFRelease(ss); CFRelease(ds);
    return fails ? 5 : badrows ? 6 : 0;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        const char *plugin = getenv("NM_TEST_PLUGIN");
        if (plugin) {
            if (!dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) { printf("dlopen: %s\n", dlerror()); return 3; }
            io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
            if (!svc) { printf("no NMXAccel service\n"); return 2; }
            dev = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc],
                                                               NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
            printf("test plugin %s: %s\n", plugin, dev ? dev.name.UTF8String : "no device");
        } else
            for (id<MTLDevice> d in MTLCopyAllDevices())
                if ([d.name rangeOfString:@"Intel" options:NSCaseInsensitiveSearch].location != NSNotFound) dev = d;
        if (!dev && getenv("MTLBENCH_ANY")) dev = MTLCreateSystemDefaultDevice();   // checks the test itself on another GPU
        if (!dev) { printf("no Intel Metal device\n"); return 2; }
        printf("using %s\n", dev.name.UTF8String);
        int n = getenv("MTLBENCH_N") ? atoi(getenv("MTLBENCH_N")) : 30;
        if (n < 1 || n > 1000) n = 30;
        double *t = calloc((size_t)n, sizeof(double));
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (getenv("MTLBENCH_PITCH") && getenv("MTLBENCH_PITCH")[0] == '1') {
            int rc = pitch_case(dev, q, "pitch-wide", 5632, 64, 8, 45056, (uint32_t)'RGhA', MTLPixelFormatRGBA16Float, 0x21);
            int r2 = pitch_case(dev, q, "pitch-odd", 642, 64, 1, 656, (uint32_t)'L008', MTLPixelFormatR8Unorm, 0x5c);
            printf("metal bench pitch: %s\n", rc == 5 || r2 == 5 ? "INCOMPLETE" : rc || r2 ? "WRONG PIXELS" : "OK");
            free(t);
            return rc == 5 || r2 == 5 ? 5 : rc || r2 ? 6 : 0;   // a failed setup (3) counts as wrong too
        }
        id<MTLTexture> dst = surface_texture(dev, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, kW, kH, 4, kW * 4, (uint32_t)'BGRA', MTLPixelFormatBGRA8Unorm, NULL);
        id<MTLTexture> src = surface_texture(dev, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, kW, kH, 4, kW * 4, (uint32_t)'BGRA', MTLPixelFormatBGRA8Unorm, NULL);
        if (!dst || !src) { printf("no IOSurface texture\n"); return 3; }
        int fails = 0;

        // clear: a render pass with nothing drawn
        for (int i = 0; i < n; i++) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = dst;
            p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            p.colorAttachments[0].clearColor = MTLClearColorMake(i & 1, 0.5, 0, 1);
            double t0 = now_ms();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            t[i] = now_ms() - t0;
            fails += cb.status != MTLCommandBufferStatusCompleted;
        }
        int bad = report("clear", t, n, fails);
        int wrong = 0;
        if (g_nsurf == 2) {
            const uint32_t want = ((n - 1) & 1 ? 0xffff0000u : 0xff000000u) | 0x00008000u;   // BGRA bytes as a little-endian word
            IOSurfaceLock(g_surf[0], kIOSurfaceLockReadOnly, NULL);
            const uint8_t *b = IOSurfaceGetBaseAddress(g_surf[0]); size_t row = IOSurfaceGetBytesPerRow(g_surf[0]);
            for (int k = 0; k < 64; k++) {
                uint32_t v = *(const uint32_t *)(b + (size_t)(k * 25 % kH) * row + (size_t)(k * 39 % kW) * 4);
                int dg = (int)((v >> 8) & 0xff) - 0x80;
                wrong += (v & 0xffff00ffu) != (want & 0xffff00ffu) || dg < -1 || dg > 1;
            }
            IOSurfaceUnlock(g_surf[0], kIOSurfaceLockReadOnly, NULL);
            printf("check clear: %d of 64 wrong\n", wrong);
            // CPU pattern into the blit source (surface-in on the next use)
            IOSurfaceLock(g_surf[1], 0, NULL);
            uint8_t *sb = IOSurfaceGetBaseAddress(g_surf[1]); size_t sr = IOSurfaceGetBytesPerRow(g_surf[1]);
            for (NSUInteger y = 0; y < kH; y++) for (NSUInteger x = 0; x < kW; x++)
                *(uint32_t *)(sb + y * sr + x * 4) = 0xff000000u | (uint32_t)((x * 7) & 0xff) << 16 | (uint32_t)(y & 0xff) << 8 | (uint32_t)((x ^ y) & 0xff);
            IOSurfaceUnlock(g_surf[1], 0, NULL);
        }

        // blit: one full-panel texture copy
        fails = 0;
        for (int i = 0; i < n; i++) {
            double t0 = now_ms();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(kW, kH, 1) toTexture:dst destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            t[i] = now_ms() - t0;
            fails += cb.status != MTLCommandBufferStatusCompleted;
        }
        bad |= report("blit", t, n, fails);
        if (g_nsurf == 2) {
            int bw = 0;
            IOSurfaceLock(g_surf[0], kIOSurfaceLockReadOnly, NULL);
            const uint8_t *b = IOSurfaceGetBaseAddress(g_surf[0]); size_t row = IOSurfaceGetBytesPerRow(g_surf[0]);
            for (int k = 0; k < 256; k++) {
                NSUInteger x = (NSUInteger)(k * 997) % kW, y = (NSUInteger)(k * 613) % kH;
                uint32_t want = 0xff000000u | (uint32_t)((x * 7) & 0xff) << 16 | (uint32_t)(y & 0xff) << 8 | (uint32_t)((x ^ y) & 0xff);
                bw += *(const uint32_t *)(b + y * row + x * 4) != want;
            }
            IOSurfaceUnlock(g_surf[0], kIOSurfaceLockReadOnly, NULL);
            printf("check blit (CPU pattern in, GPU copy, read back): %d of 256 wrong\n", bw);
            wrong += bw;
        }

        // draw: a full-screen quad sampling src, alpha-blended over dst (what a compositor does per layer)
        NSString *s = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint v [[vertex_id]]) { float2 q[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };"
            " V o; o.p = float4(q[v], 0, 1); o.uv = q[v] * 0.5 + 0.5; return o; }\n"
            "fragment half4 fs(V in [[stage_in]], texture2d<half> t [[texture(0)]]) {"
            " constexpr sampler sm(filter::linear); half4 c = t.sample(sm, in.uv); c.a = 0.5h; return c; }\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:s options:nil error:&err];
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        rpd.colorAttachments[0].blendingEnabled = YES;
        rpd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        rpd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        id<MTLRenderPipelineState> ps = lib ? [dev newRenderPipelineStateWithDescriptor:rpd error:&err] : nil;
        if (!ps) { printf("draw pipeline: %s\n", err ? err.localizedDescription.UTF8String : "no library"); return 4; }
        fails = 0;
        for (int i = 0; i < n; i++) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = dst;
            p.colorAttachments[0].loadAction = MTLLoadActionLoad;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            double t0 = now_ms();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:ps];
            [re setFragmentTexture:src atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            t[i] = now_ms() - t0;
            fails += cb.status != MTLCommandBufferStatusCompleted;
        }
        bad |= report("draw", t, n, fails);
        free(t);
        printf("metal bench: %s\n", bad ? "INCOMPLETE" : wrong ? "WRONG PIXELS" : "OK");
        return bad ? 5 : wrong ? 6 : 0;
    }
}
