// WebOfShadowsRecomp for the PS5 -- native renderer, Option A1 (plumbing proof).
//
// GOAL (A1): prove, end to end and in COMPLETE ISOLATION from rexglue's renderer,
// that we can take ONE real draw's vertices out of guest memory, lay them out as
// the game's vertex-fetch constant describes, run them through a HOST Vulkan
// pipeline on the SHARED VkDevice, and get pixels into an offscreen target we read
// back. We use our OWN trivial shaders (wos_native_shaders.h), our OWN buffers,
// image, render pass, command pool and queue submission. We touch NONE of
// rexglue's Vulkan state -- so this cannot destabilise the live emulated renderer.
// If it rasterises anything, we log a non-background pixel count. That is the A1
// signal. (A2 will swap in rexglue's real translated SPIR-V + descriptor env.)
//
// Everything is called through the device/instance FUNCTION TABLES obtained from
// the live provider: on PS5 there are no global vk* symbols to link against.
//
// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT

#include "wos_ps5_native_draw.h"

#include <cstdint>
#include <cstring>
#include <vector>

// rex/ui/vulkan/api.h MUST be the first rex header: it defines
// VK_ENABLE_BETA_EXTENSIONS, VK_NO_PROTOTYPES and VULKAN_HPP_NO_TO_STRING *before*
// pulling in the bundled vulkan-headers. Any other rex header included first can
// drag in <vulkan/vulkan.h> WITHOUT the beta macro, after which vulkan.hpp
// references provisional (AMDX/CUDA) enums that vulkan_core.h then lacks -> a wall
// of "undeclared identifier" errors. We call every entry point through the
// device/instance function tables (PS5 has no global vk* symbols), never the
// prototypes. Do NOT include the system <vulkan/vulkan.h> here.
#include <rex/ui/vulkan/api.h>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#include "wos_native_shaders.h"

namespace wos {
namespace native {
namespace {

// Small helper: find a memory type index satisfying `type_bits` with `props`.
int FindMemoryType(const VkPhysicalDeviceMemoryProperties &mp, uint32_t type_bits,
                   VkMemoryPropertyFlags props) {
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
      return (int)i;
    }
  }
  return -1;
}

// A1 renders at a small fixed size; we only need to know "did anything draw".
constexpr uint32_t kW = 256;
constexpr uint32_t kH = 256;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

}  // namespace

// One-shot: build a throwaway host pipeline and draw `vertex_count` vertices read
// from guest memory at host pointer `host_vb` with the given `stride_bytes`, then
// read back and return the count of pixels that are NOT the clear color. Returns
// -1 on any Vulkan failure (logged). Deliberately allocates and frees everything
// per call -- A1 runs once, correctness/clarity over efficiency.
long RenderOneDrawA1(const void *host_vb, uint32_t vertex_count, uint32_t stride_bytes) {
  auto *rt = rex::Runtime::instance();
  if (!rt) { REXLOG_INFO("[WoS-NATIVE-A1] no runtime"); return -1; }
  auto *igs = rt->graphics_system();
  if (!igs) { REXLOG_INFO("[WoS-NATIVE-A1] no graphics system"); return -1; }
  auto *provider = static_cast<rex::ui::vulkan::VulkanProvider *>(igs->provider());
  if (!provider) { REXLOG_INFO("[WoS-NATIVE-A1] no Vulkan provider"); return -1; }
  auto *vdev = provider->vulkan_device();
  if (!vdev) { REXLOG_INFO("[WoS-NATIVE-A1] no Vulkan device"); return -1; }

  const VkDevice dev = vdev->device();
  const auto &df = vdev->functions();              // device function table
  const auto &inf = vdev->vulkan_instance()->functions();  // instance function table

  VkPhysicalDeviceMemoryProperties mp{};
  inf.vkGetPhysicalDeviceMemoryProperties(vdev->physical_device(), &mp);

  // --- track everything we create so a single cleanup path frees it all -------
  struct Cleanup {
    const rex::ui::vulkan::VulkanDevice *vdev;
    VkDevice dev;
    std::vector<VkBuffer> buffers;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> mems;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    ~Cleanup() {
      const auto &f = vdev->functions();
      if (fence) f.vkDestroyFence(dev, fence, nullptr);
      if (pool) f.vkDestroyCommandPool(dev, pool, nullptr);
      if (fb) f.vkDestroyFramebuffer(dev, fb, nullptr);
      if (pipe) f.vkDestroyPipeline(dev, pipe, nullptr);
      if (pl) f.vkDestroyPipelineLayout(dev, pl, nullptr);
      if (rp) f.vkDestroyRenderPass(dev, rp, nullptr);
      if (vs) f.vkDestroyShaderModule(dev, vs, nullptr);
      if (fs) f.vkDestroyShaderModule(dev, fs, nullptr);
      for (auto v : views) f.vkDestroyImageView(dev, v, nullptr);
      for (auto im : images) f.vkDestroyImage(dev, im, nullptr);
      for (auto b : buffers) f.vkDestroyBuffer(dev, b, nullptr);
      for (auto m : mems) f.vkFreeMemory(dev, m, nullptr);
    }
  } cu{vdev, dev};

  auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props, VkBuffer *out_buf,
                         VkDeviceMemory *out_mem) -> bool {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (df.vkCreateBuffer(dev, &bi, nullptr, out_buf) != VK_SUCCESS) return false;
    cu.buffers.push_back(*out_buf);
    VkMemoryRequirements req{};
    df.vkGetBufferMemoryRequirements(dev, *out_buf, &req);
    int mt = FindMemoryType(mp, req.memoryTypeBits, props);
    if (mt < 0) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (df.vkAllocateMemory(dev, &ai, nullptr, out_mem) != VK_SUCCESS) return false;
    cu.mems.push_back(*out_mem);
    return df.vkBindBufferMemory(dev, *out_buf, *out_mem, 0) == VK_SUCCESS;
  };

  // --- vertex buffer: copy this draw's vertices from guest memory --------------
  // The guest bytes are big-endian (endian=2, 8-in-32). Our GLSL reads floats, so
  // byte-swap each 32-bit word on the way in. We copy vertex_count*stride bytes.
  const VkDeviceSize vb_bytes = (VkDeviceSize)vertex_count * stride_bytes;
  if (vb_bytes == 0 || (stride_bytes & 3u)) {
    REXLOG_INFO("[WoS-NATIVE-A1] bad geometry: count={} stride={}", vertex_count, stride_bytes);
    return -1;
  }
  VkBuffer vbuf; VkDeviceMemory vmem;
  if (!make_buffer(vb_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &vbuf, &vmem)) {
    REXLOG_INFO("[WoS-NATIVE-A1] vertex buffer alloc failed");
    return -1;
  }
  {
    void *mapped = nullptr;
    if (df.vkMapMemory(dev, vmem, 0, vb_bytes, 0, &mapped) != VK_SUCCESS) return -1;
    const uint32_t *src = reinterpret_cast<const uint32_t *>(host_vb);
    uint32_t *dst = reinterpret_cast<uint32_t *>(mapped);
    const VkDeviceSize words = vb_bytes / 4;
    for (VkDeviceSize i = 0; i < words; ++i) dst[i] = __builtin_bswap32(src[i]);
    df.vkUnmapMemory(dev, vmem);
  }

  // --- readback buffer (image -> buffer -> host) ------------------------------
  const VkDeviceSize rb_bytes = (VkDeviceSize)kW * kH * 4;
  VkBuffer rbuf; VkDeviceMemory rmem;
  if (!make_buffer(rb_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &rbuf, &rmem)) {
    REXLOG_INFO("[WoS-NATIVE-A1] readback buffer alloc failed");
    return -1;
  }

  // --- offscreen color image + view -------------------------------------------
  VkImage img; VkDeviceMemory imem; VkImageView view;
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kFormat;
    ii.extent = {kW, kH, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (df.vkCreateImage(dev, &ii, nullptr, &img) != VK_SUCCESS) return -1;
    cu.images.push_back(img);
    VkMemoryRequirements req{};
    df.vkGetImageMemoryRequirements(dev, img, &req);
    int mt = FindMemoryType(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return -1;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
    if (df.vkAllocateMemory(dev, &ai, nullptr, &imem) != VK_SUCCESS) return -1;
    cu.mems.push_back(imem);
    if (df.vkBindImageMemory(dev, img, imem, 0) != VK_SUCCESS) return -1;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (df.vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) return -1;
    cu.views.push_back(view);
  }

  // --- render pass (clear -> draw -> transfer src) ----------------------------
  {
    VkAttachmentDescription at{};
    at.format = kFormat; at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1; rpi.pAttachments = &at;
    rpi.subpassCount = 1; rpi.pSubpasses = &sp;
    if (df.vkCreateRenderPass(dev, &rpi, nullptr, &cu.rp) != VK_SUCCESS) return -1;
  }

  // --- shader modules (our embedded SPIR-V) -----------------------------------
  {
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = kWosNativeVertSpirv_size; si.pCode = kWosNativeVertSpirv;
    if (df.vkCreateShaderModule(dev, &si, nullptr, &cu.vs) != VK_SUCCESS) return -1;
    si.codeSize = kWosNativeFragSpirv_size; si.pCode = kWosNativeFragSpirv;
    if (df.vkCreateShaderModule(dev, &si, nullptr, &cu.fs) != VK_SUCCESS) return -1;
  }

  // --- pipeline layout (no descriptors in A1) ---------------------------------
  {
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (df.vkCreatePipelineLayout(dev, &li, nullptr, &cu.pl) != VK_SUCCESS) return -1;
  }

  // --- graphics pipeline ------------------------------------------------------
  {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = cu.vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = cu.fs; stages[1].pName = "main";

    // Vertex input: one binding = this draw's stride; two attributes per the
    // validated [WoS-NATIVE-VTX] layout for VS 1E6883FC (pos vec3 @0, extra vec4 @12).
    VkVertexInputBindingDescription bind{0, stride_bytes, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12};
    VkPipelineVertexInputStateCreateInfo vinp{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vinp.vertexBindingDescriptionCount = 1; vinp.pVertexBindingDescriptions = &bind;
    vinp.vertexAttributeDescriptionCount = 2; vinp.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)kW, (float)kH, 0, 1};
    VkRect2D sc{{0, 0}, {kW, kH}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2; pi.pStages = stages;
    pi.pVertexInputState = &vinp; pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps; pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms; pi.pColorBlendState = &cb;
    pi.layout = cu.pl; pi.renderPass = cu.rp; pi.subpass = 0;
    if (df.vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &cu.pipe) != VK_SUCCESS) {
      REXLOG_INFO("[WoS-NATIVE-A1] pipeline creation failed");
      return -1;
    }
  }

  // --- framebuffer ------------------------------------------------------------
  {
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = cu.rp; fi.attachmentCount = 1; fi.pAttachments = &view;
    fi.width = kW; fi.height = kH; fi.layers = 1;
    if (df.vkCreateFramebuffer(dev, &fi, nullptr, &cu.fb) != VK_SUCCESS) return -1;
  }

  // --- command pool + buffer --------------------------------------------------
  const uint32_t qfam = vdev->queue_family_graphics_compute();
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = qfam;
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (df.vkCreateCommandPool(dev, &ci, nullptr, &cu.pool) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cu.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    if (df.vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return -1;
  }

  // --- record -----------------------------------------------------------------
  {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    df.vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};  // black background
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = cu.rp; rbi.framebuffer = cu.fb;
    rbi.renderArea = {{0, 0}, {kW, kH}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    df.vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    df.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cu.pipe);
    VkDeviceSize off0 = 0;
    df.vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off0);
    df.vkCmdDraw(cmd, vertex_count, 1, 0, 0);
    df.vkCmdEndRenderPass(cmd);

    // image (now TRANSFER_SRC_OPTIMAL) -> readback buffer
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kW, kH, 1};
    df.vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
    df.vkEndCommandBuffer(cmd);
  }

  // --- submit on the shared graphics queue + wait on our own fence ------------
  {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (df.vkCreateFence(dev, &fi, nullptr, &cu.fence) != VK_SUCCESS) return -1;
    auto acq = vdev->AcquireQueue(qfam, 0);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkResult sr;
    {
      // AcquireQueue hands back a queue whose host access we must serialise; the
      // acquisition object holds the lock for its lifetime.
      sr = df.vkQueueSubmit(acq.queue(), 1, &si, cu.fence);
    }
    if (sr != VK_SUCCESS) { REXLOG_INFO("[WoS-NATIVE-A1] queue submit failed ({})", (int)sr); return -1; }
    df.vkWaitForFences(dev, 1, &cu.fence, VK_TRUE, UINT64_MAX);
  }

  // --- read back & count non-background pixels --------------------------------
  long drawn = -1;
  {
    void *mapped = nullptr;
    if (df.vkMapMemory(dev, rmem, 0, rb_bytes, 0, &mapped) != VK_SUCCESS) return -1;
    const uint32_t *px = reinterpret_cast<const uint32_t *>(mapped);
    long count = 0;
    // background is opaque black (0xFF000000 in RGBA8 little-endian); the shader
    // writes magenta (R=255,B=255 -> 0xFFFF00FF). Count anything non-black.
    for (uint32_t i = 0; i < kW * kH; ++i) {
      if ((px[i] & 0x00FFFFFFu) != 0) ++count;
    }
    drawn = count;
    df.vkUnmapMemory(dev, rmem);
  }
  REXLOG_INFO("[WoS-NATIVE-A1] offscreen draw OK: {} verts, stride {}B -> {} of {} px rendered",
              vertex_count, stride_bytes, drawn, kW * kH);
  return drawn;
}

}  // namespace native
}  // namespace wos
