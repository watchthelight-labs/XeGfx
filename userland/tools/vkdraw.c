// vkdraw: renders one green triangle on a red clear into a 64x64 image, copies it to a host buffer and checks pixels.
// The first draw test for ANV on XeGfx: a render pass, a graphics pipeline, a queue submit, a fence wait and a
// readback. Prints each step and exits 0 only when the centre is green and a corner is red.
// <wtl> project: xegfx | author: watchthelight </wtl>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "tri_spv.h"

#define W 64
#define H 64
#define CHECK(x) do { VkResult r_ = (x); printf("  %-28s %d\n", #x, r_); if (r_ != VK_SUCCESS) return 1; } while (0)

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;

    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkShaderModule shader(VkDevice dev, const unsigned char *code, unsigned int len)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, len, (const uint32_t *)code };
    VkShaderModule m = VK_NULL_HANDLE;

    printf("  vkCreateShaderModule         %d\n", vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkdraw", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t n = 1;

    CHECK(vkCreateInstance(&ici, NULL, &inst));
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (n == 0) { printf("no physical devices\n"); return 2; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, 0, 0, &q);

    // colour target in device memory
    VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
        { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img;
    CHECK(vkCreateImage(dev, &imci, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
        mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
    CHECK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
        VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view;
    CHECK(vkCreateImageView(dev, &vci, NULL, &view));

    // readback buffer the CPU maps
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mem_type(pd, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &bmai, NULL, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

    // render pass: clear, draw, end in TRANSFER_SRC for the copy
    VkAttachmentDescription att = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL };
    VkSubpassDependency dep = { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0 };
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1 };
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb));

    // pipeline
    VkShaderModule vs = shader(dev, tri_vert_spv, tri_vert_spv_len), fs = shader(dev, tri_frag_spv, tri_frag_spv_len);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0,
        VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1 };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
        VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
        VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL,
        &vps, &rs, &ms, NULL, &cb, NULL, pl, rp, 0, VK_NULL_HANDLE, -1 };
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    // record: clear red, draw green, copy to the buffer
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0 };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    VkClearValue clear = { .color = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, { { 0, 0 }, { W, H } }, 1, &clear };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy reg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fnci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fnci, NULL, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));

    uint8_t *px;
    CHECK(vkMapMemory(dev, bmem, 0, W * H * 4, 0, (void **)&px));
    const uint8_t *c = px + ((H / 2) * W + W / 2) * 4, *k = px;
    printf("centre  %3u %3u %3u %3u (want 0 255 0 255)\n", c[0], c[1], c[2], c[3]);
    printf("corner  %3u %3u %3u %3u (want 255 0 0 255)\n", k[0], k[1], k[2], k[3]);
    int green = 0;
    for (int i = 0; i < W * H; i++)
        green += px[i * 4 + 1] == 255 && px[i * 4] == 0;
    printf("green pixels %d of %d (a triangle of half the width and height covers about 512)\n", green, W * H);
    int ok = c[0] == 0 && c[1] == 255 && k[0] == 255 && k[1] == 0 && green > 300 && green < 800;
    printf("draw: %s\n", ok ? "OK" : "WRONG");
    vkUnmapMemory(dev, bmem);
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return ok ? 0 : 3;
}
