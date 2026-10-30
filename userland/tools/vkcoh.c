// vkcoh: CPU/GPU coherency per host-visible Vulkan memory type. For each type: the CPU memsets a mapped buffer, a
// compute shader writes v[i] = 3i + 7, the CPU checks every value. Wrong values in 16-value runs are stale 64-byte
// CPU cache lines (N-235: Metal shared buffers on the 7D67 lost 17 lines).
// <wtl> project: xegfx | author: watchthelight </wtl>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "fill_spv.h"

#ifndef N
#define N 4096
#endif

static int run(VkPhysicalDevice pd, VkDevice dev, VkQueue q, uint32_t type, VkMemoryPropertyFlags fl)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer buf;
    VkMemoryRequirements mr;
    VkDeviceMemory mem;
    uint32_t *v;

    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return -1;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    if (!(mr.memoryTypeBits & (1u << type))) { vkDestroyBuffer(dev, buf, NULL); return -2; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type };
    if (vkAllocateMemory(dev, &mai, NULL, &mem)) return -3;
    vkBindBufferMemory(dev, buf, mem, 0);
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&v);
    memset(v, 0, N * 4);	/* the CPU now holds these lines */
    VkMappedMemoryRange rng = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE };
    if (!(fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) vkFlushMappedMemoryRanges(dev, 1, &rng);

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
    /* VKCOH_NOBARRIER=1: what a driver that relies on the fence alone gets (the NvGfx plugin on NVIDIA) */
    if (!getenv("VKCOH_NOBARRIER"))
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f;
    vkCreateFence(dev, &fci, NULL, &f);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
    VkResult r = vkQueueSubmit(q, 1, &si, f);
    if (r == VK_SUCCESS) r = vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull);
    if (!(fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) vkInvalidateMappedMemoryRanges(dev, 1, &rng);

    int bad = 0, lines = 0;
    for (int i = 0; i < N; i++) bad += v[i] != (uint32_t)i * 3u + 7u;
    for (int l = 0; l < N / 16; l++)
        for (int k = 0; k < 16; k++)
            if (v[l * 16 + k] != (uint32_t)(l * 16 + k) * 3u + 7u) { lines++; break; }
    printf("  type %u flags 0x%x%s%s%s: submit/wait %d, %d of %d wrong, %d of %d cache lines\n", type, fl,
           fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
           fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "",
           fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "", r, bad, N, lines, N / 16);
    vkDeviceWaitIdle(dev);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkUnmapMemory(dev, mem);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    return bad;
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkcoh", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    VkPhysicalDevice pd;
    uint32_t n = 1;
    if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no physical devices\n"); return 2; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return 1;
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int worst = 0;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags fl = mp.memoryTypes[t].propertyFlags;
        if (!(fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { printf("  type %u flags 0x%x: not host visible\n", t, fl); continue; }
        int b = run(pd, dev, q, t, fl);
        if (b > worst) worst = b;
    }
    printf("vkcoh: %s\n", worst ? "STALE LINES" : "OK");
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return worst ? 3 : 0;
}
