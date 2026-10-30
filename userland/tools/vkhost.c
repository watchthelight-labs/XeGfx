// vkhost: zero-copy IOSurface memory on ANV (X5). An IOSurface's own memory is imported with VK_EXT_external_memory_host
// (i915 userptr through XeGfx's user page holds), a compute shader writes v[i] = 3i + 7 into it, and the CPU checks the
// values through the IOSurface's base address. That is the path that would let WindowServer composite without the
// plugin's per-frame IOSurface copies (N-266: about 8 ms per surface per command buffer). Also times the import.
// <wtl> project: xegfx | author: watchthelight </wtl>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <IOSurface/IOSurfaceRef.h>
#include <CoreFoundation/CoreFoundation.h>
#include <vulkan/vulkan.h>
#include "fill_spv.h"

#define N (1024 * 1024)	/* 4 MiB of uint32 */

static double ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

static IOSurfaceRef make_surface(void)
{
    int w = 1024, h = N / 1024, bpe = 4, bpr = w * bpe;
    CFNumberRef cw = CFNumberCreate(NULL, kCFNumberIntType, &w), ch = CFNumberCreate(NULL, kCFNumberIntType, &h),
                cb = CFNumberCreate(NULL, kCFNumberIntType, &bpe), cr = CFNumberCreate(NULL, kCFNumberIntType, &bpr);
    const void *k[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow };
    const void *v[] = { cw, ch, cb, cr };
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(d);
    CFRelease(d); CFRelease(cw); CFRelease(ch); CFRelease(cb); CFRelease(cr);
    return s;
}

int main(void)
{
    const char *exts[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkhost", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    VkPhysicalDevice pd;
    uint32_t n = 1;
    if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no physical devices\n"); return 2; }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp };
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("device %s, host pointer alignment %llu\n", p2.properties.deviceName,
           (unsigned long long)hp.minImportedHostPointerAlignment);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, exts, NULL };
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) { printf("vkCreateDevice with %s: %d\n", exts[0], r); return 3; }
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    IOSurfaceRef surf = make_surface();
    if (!surf) { printf("no IOSurface\n"); return 4; }
    IOSurfaceLock(surf, 0, NULL);
    uint32_t *base = IOSurfaceGetBaseAddress(surf);
    size_t size = IOSurfaceGetAllocSize(surf);
    printf("IOSurface %u: base %p, %zu bytes, page aligned %s\n", IOSurfaceGetID(surf), (void *)base, size,
           ((uintptr_t)base & 4095) == 0 ? "yes" : "NO");
    memset(base, 0, N * 4);

    PFN_vkGetMemoryHostPointerPropertiesEXT gmhp =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mhp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    r = gmhp ? gmhp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &mhp) : VK_ERROR_EXTENSION_NOT_PRESENT;
    printf("host pointer properties: %d, memory types 0x%x\n", r, mhp.memoryTypeBits);
    if (r || !mhp.memoryTypeBits) return 5;

    VkExternalMemoryBufferCreateInfo ebi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return 6;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t type = 0;
    while (type < 32 && !((mhp.memoryTypeBits & mr.memoryTypeBits) & (1u << type))) type++;
    VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, type };
    VkDeviceMemory mem;
    double t0 = ms();
    r = vkAllocateMemory(dev, &mai, NULL, &mem);
    double timp = ms() - t0;
    printf("import %zu bytes as memory type %u: %d (%.2f ms)\n", size, type, r, timp);
    if (r) return 7;
    vkBindBufferMemory(dev, buf, mem, 0);

    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, fill_spv_len, (const uint32_t *)fill_spv };
    VkShaderModule sm;
    vkCreateShaderModule(dev, &smi, NULL, &sm);
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &pli, NULL, &pl);
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL },
        pl, VK_NULL_HANDLE, 0 };
    VkPipeline pipe;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    vkCreateDescriptorPool(dev, &dpi, NULL, &dp);
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet ds;
    vkAllocateDescriptorSets(dev, &dsa, &ds);
    VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        NULL, &dbi, NULL };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0 };
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cba, &cmd);
    VkCommandBufferBeginInfo cbb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(cmd, &cbb);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, N / 64, 1, 1);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f;
    vkCreateFence(dev, &fci, NULL, &f);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
    t0 = ms();
    r = vkQueueSubmit(q, 1, &si, f);
    if (r == VK_SUCCESS) r = vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull);
    double tsub = ms() - t0;
    int bad = 0;
    for (int i = 0; i < N; i++) bad += base[i] != (uint32_t)i * 3u + 7u;
    printf("submit/wait %d (%.2f ms): %d of %d values wrong, read through the IOSurface base address\n", r, tsub, bad, N);
    IOSurfaceUnlock(surf, 0, NULL);
    vkDeviceWaitIdle(dev);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    CFRelease(surf);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("vkhost: %s\n", r == VK_SUCCESS && bad == 0 ? "OK" : "FAILED");
    return r == VK_SUCCESS && bad == 0 ? 0 : 8;
}
