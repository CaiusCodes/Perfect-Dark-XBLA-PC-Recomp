// Perfect Dark native renderer on Vulkan -> the SDK's Vulkan presenter. See
// pd_native_vk_out.h. (GoldenEye's version also drew its menu backdrop picture
// here; binding 1 of the output pass, its picture, is a 1x1 stand-in.)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pd_native_vk_out.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace pd::native_vk {
namespace {

using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;
namespace util = rex::ui::vulkan::util;

constexpr uint32_t kFrames = 3;
constexpr VkFormat kFrameFormat = VK_FORMAT_B8G8R8A8_UNORM;  // the renderer's scene format
constexpr VkExternalMemoryHandleTypeFlagBits kHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

// The renderer's device functions (loaded through its own loader entry).
struct RendererFns {
  PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
};

struct State {
  bool ready = false;
  VulkanPresenter* presenter = nullptr;
  const VulkanDevice* vd = nullptr;
  VkDevice dev = VK_NULL_HANDLE;
  RendererFns r;
  VkDevice rdev = VK_NULL_HANDLE;
  uint32_t rfamily = 0;
  uint32_t width = 0, height = 0;
  // Shared frames: on the presenter's device (exported) and the renderer's.
  Image frame[kFrames];
  VkImage rframe[kFrames] = {};
  VkDeviceMemory rmemory[kFrames] = {};
  bool rwritten[kFrames] = {};
  int read_by[kFrames] = {-1, -1, -1};  // the command slot whose fence ends the presenter's read
  uint32_t next = 0;
  // Output pass.
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet sets[kFrames] = {};
  uint32_t constants_size = 0;
  // Command buffers of the presenter's queue, one per slot.
  VkCommandPool cmd_pool[kFrames] = {};
  VkCommandBuffer cmd[kFrames] = {};
  VkFence fence[kFrames] = {};
  bool fence_pending[kFrames] = {};
  uint32_t slot = 0;
  // Gamma ramp and a 1x1 stand-in, with their uploads.
  Image ramp;
  bool ramp_written = false;
  uint32_t ramp_version = 0;
  VkBuffer ramp_staging[kFrames] = {};
  VkDeviceMemory ramp_staging_memory[kFrames] = {};
  uint32_t ramp_staging_type[kFrames] = {};
  void* ramp_mapped[kFrames] = {};
  Image dummy;
  bool dummy_ready = false;
} s;

VkImageMemoryBarrier barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                             VkAccessFlags dst_access, uint32_t src_family = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t dst_family = VK_QUEUE_FAMILY_IGNORED) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = src_family;
  b.dstQueueFamilyIndex = dst_family;
  b.image = image;
  b.subresourceRange = util::InitializeSubresourceRange();
  return b;
}

VkImageCreateInfo image_info(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {w, h, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return info;
}

bool make_view(Image& img, VkFormat format) {
  VkImageViewCreateInfo v{};
  v.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  v.image = img.image;
  v.viewType = VK_IMAGE_VIEW_TYPE_2D;
  v.format = format;
  v.subresourceRange = util::InitializeSubresourceRange();
  return s.vd->functions().vkCreateImageView(s.dev, &v, nullptr, &img.view) == VK_SUCCESS;
}

// A sampled image on the presenter's device (ramp, stand-in).
bool make_texture(Image& img, VkFormat format, uint32_t w, uint32_t h) {
  const VkImageCreateInfo info = image_info(format, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  if (!util::CreateDedicatedAllocationImage(s.vd, info, util::MemoryPurpose::kDeviceLocal, img.image, img.memory))
    return false;
  img.width = w;
  img.height = h;
  return make_view(img, format);
}

// A frame shared with the renderer: made here, exported, imported there.
bool make_shared_frame(uint32_t k) {
  const auto& dfn = s.vd->functions();
  Image& img = s.frame[k];
  VkExternalMemoryImageCreateInfo external{};
  external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  external.handleTypes = kHandleType;
  VkImageCreateInfo info =
      image_info(kFrameFormat, s.width, s.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  info.pNext = &external;
  if (dfn.vkCreateImage(s.dev, &info, nullptr, &img.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  dfn.vkGetImageMemoryRequirements(s.dev, img.image, &req);
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = img.image;
  VkExportMemoryAllocateInfo export_info{};
  export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  export_info.pNext = &dedicated;
  export_info.handleTypes = kHandleType;
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &export_info;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = util::ChooseMemoryType(s.vd->memory_types(), req.memoryTypeBits,
                                                 util::MemoryPurpose::kDeviceLocal);
  if (alloc.memoryTypeIndex == UINT32_MAX || dfn.vkAllocateMemory(s.dev, &alloc, nullptr, &img.memory) != VK_SUCCESS ||
      dfn.vkBindImageMemory(s.dev, img.image, img.memory, 0) != VK_SUCCESS || !make_view(img, kFrameFormat))
    return false;
  img.width = s.width;
  img.height = s.height;
  auto get_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
      s.vd->vulkan_instance()->functions().vkGetDeviceProcAddr(s.dev, "vkGetMemoryWin32HandleKHR"));
  if (!get_handle) return false;
  VkMemoryGetWin32HandleInfoKHR get{};
  get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
  get.memory = img.memory;
  get.handleType = kHandleType;
  HANDLE handle = nullptr;
  if (get_handle(s.dev, &get, &handle) != VK_SUCCESS || !handle) return false;

  // The renderer's side: the same image description, on the imported memory.
  bool ok = s.r.vkCreateImage(s.rdev, &info, nullptr, &s.rframe[k]) == VK_SUCCESS;
  VkMemoryRequirements rreq{};
  if (ok) s.r.vkGetImageMemoryRequirements(s.rdev, s.rframe[k], &rreq);
  ok = ok && rreq.size <= req.size;
  VkImportMemoryWin32HandleInfoKHR import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
  import.handleType = kHandleType;
  import.handle = handle;
  VkMemoryDedicatedAllocateInfo rdedicated{};
  rdedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  rdedicated.image = s.rframe[k];
  import.pNext = &rdedicated;
  VkMemoryAllocateInfo ralloc{};
  ralloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ralloc.pNext = &import;
  ralloc.allocationSize = req.size;
  ralloc.memoryTypeIndex = UINT32_MAX;
  for (uint32_t t = 0; t < 32; ++t) {
    if (rreq.memoryTypeBits & (1u << t)) {
      ralloc.memoryTypeIndex = t;
      break;
    }
  }
  ok = ok && ralloc.memoryTypeIndex != UINT32_MAX &&
       s.r.vkAllocateMemory(s.rdev, &ralloc, nullptr, &s.rmemory[k]) == VK_SUCCESS &&
       s.r.vkBindImageMemory(s.rdev, s.rframe[k], s.rmemory[k], 0) == VK_SUCCESS;
  CloseHandle(handle);
  return ok;
}

constexpr VkDescriptorType kBindingTypes[5] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};

bool make_pipeline(const std::vector<uint8_t>& spirv) {
  const auto& dfn = s.vd->functions();
  VkDescriptorSetLayoutBinding b[5]{};
  for (uint32_t i = 0; i < 5; ++i) {
    b[i].binding = i;
    b[i].descriptorType = kBindingTypes[i];
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dsl{};
  dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dsl.bindingCount = 5;
  dsl.pBindings = b;
  if (dfn.vkCreateDescriptorSetLayout(s.dev, &dsl, nullptr, &s.set_layout) != VK_SUCCESS) return false;
  VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, s.constants_size};
  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &s.set_layout;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &pc;
  if (dfn.vkCreatePipelineLayout(s.dev, &pl, nullptr, &s.layout) != VK_SUCCESS) return false;
  const VkShaderModule module = util::CreateShaderModule(
      s.vd, reinterpret_cast<const uint32_t*>(spirv.data()), spirv.size());
  if (module == VK_NULL_HANDLE) return false;
  VkComputePipelineCreateInfo cp{};
  cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cp.stage.module = module;
  cp.stage.pName = "main";
  cp.layout = s.layout;
  const bool ok = dfn.vkCreateComputePipelines(s.dev, VK_NULL_HANDLE, 1, &cp, nullptr, &s.pipeline) == VK_SUCCESS;
  dfn.vkDestroyShaderModule(s.dev, module, nullptr);
  if (!ok) return false;
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = 16.0f;
  if (dfn.vkCreateSampler(s.dev, &si, nullptr, &s.sampler) != VK_SUCCESS) return false;
  VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kFrames},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER, kFrames},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFrames}};
  VkDescriptorPoolCreateInfo dp{};
  dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dp.maxSets = kFrames;
  dp.poolSizeCount = 3;
  dp.pPoolSizes = sizes;
  if (dfn.vkCreateDescriptorPool(s.dev, &dp, nullptr, &s.pool) != VK_SUCCESS) return false;
  VkDescriptorSetLayout layouts[kFrames] = {s.set_layout, s.set_layout, s.set_layout};
  VkDescriptorSetAllocateInfo da{};
  da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  da.descriptorPool = s.pool;
  da.descriptorSetCount = kFrames;
  da.pSetLayouts = layouts;
  return dfn.vkAllocateDescriptorSets(s.dev, &da, s.sets) == VK_SUCCESS;
}

bool make_commands() {
  const auto& dfn = s.vd->functions();
  for (uint32_t i = 0; i < kFrames; ++i) {
    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cp.queueFamilyIndex = s.vd->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(s.dev, &cp, nullptr, &s.cmd_pool[i]) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = s.cmd_pool[i];
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(s.dev, &ca, &s.cmd[i]) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (dfn.vkCreateFence(s.dev, &fi, nullptr, &s.fence[i]) != VK_SUCCESS) return false;
  }
  return true;
}

void wait_slot(uint32_t i) {
  if (!s.fence_pending[i]) return;
  const auto& dfn = s.vd->functions();
  dfn.vkWaitForFences(s.dev, 1, &s.fence[i], VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(s.dev, 1, &s.fence[i]);
  s.fence_pending[i] = false;
}

// An upload buffer of `size` bytes, filled with `data` if given (and left
// mapped if `keep_mapped`).
bool staging(VkDeviceSize size, const void* data, VkBuffer& buffer, VkDeviceMemory& memory, uint32_t* type_out = nullptr,
             void** keep_mapped = nullptr) {
  uint32_t type = 0;
  VkDeviceSize memory_size = 0;
  if (!util::CreateDedicatedAllocationBuffer(s.vd, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, util::MemoryPurpose::kUpload,
                                             buffer, memory, &type, &memory_size))
    return false;
  void* mapped = nullptr;
  if (s.vd->functions().vkMapMemory(s.dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
  if (data) {
    std::memcpy(mapped, data, size_t(size));
    util::FlushMappedMemoryRange(s.vd, memory, type, 0, memory_size, size);
  }
  if (type_out) *type_out = type;
  if (keep_mapped) *keep_mapped = mapped;
  else s.vd->functions().vkUnmapMemory(s.dev, memory);
  return true;
}

void copy_buffer_to_image(VkCommandBuffer c, VkBuffer buffer, const Image& img, VkImageLayout from) {
  const auto& dfn = s.vd->functions();
  VkImageMemoryBarrier b = barrier(img.image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                   VK_ACCESS_TRANSFER_WRITE_BIT);
  dfn.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {img.width, img.height, 1};
  dfn.vkCmdCopyBufferToImage(c, buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  b = barrier(img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  dfn.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
}

double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// The worker for PresentAsync, and the lock the shared state is used under.
namespace {
struct Job {
  uint32_t k = 0;
  std::vector<uint8_t> constants;
  std::vector<uint16_t> ramp;
  uint32_t ramp_version = 0;
  std::function<void()> wait_gpu, after;
};
std::mutex g_lock;
std::mutex g_jobs_lock;
std::condition_variable g_jobs_cv, g_idle_cv;
std::deque<Job> g_jobs;
uint64_t g_queued = 0, g_done = 0;  // hand-offs given / finished
double g_wait_ms = 0, g_refresh_ms = 0;
bool g_worker_started = false;

void worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(g_jobs_lock);
      g_jobs_cv.wait(lock, [] { return !g_jobs.empty(); });
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    const double t0 = now_ms();
    job.wait_gpu();
    const double t1 = now_ms();
    Present(job.k, job.constants.data(), job.ramp.empty() ? nullptr : job.ramp.data(), job.ramp_version);
    if (job.after) job.after();
    const double t2 = now_ms();
    {
      std::lock_guard lock(g_jobs_lock);
      ++g_done;
      g_wait_ms += t1 - t0;
      g_refresh_ms += t2 - t1;
    }
    g_idle_cv.notify_all();
  }
}
}  // namespace

bool PresenterIsVulkan(rex::ui::Presenter* presenter) {
  return presenter && dynamic_cast<VulkanPresenter*>(presenter) != nullptr;
}

bool VulkanUsable(std::string& why) {
  HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (!loader) {
    why = "no Vulkan driver (vulkan-1.dll not found)";
    return false;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
  auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
  bool usable = false;
  VkInstance instance = VK_NULL_HANDLE;
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  if (!create || create(&ci, nullptr, &instance) != VK_SUCCESS) {
    why = "the Vulkan driver could not start";
  } else {
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    uint32_t count = 0;
    std::vector<VkPhysicalDevice> devices;
    if (enumerate && enumerate(instance, &count, nullptr) == VK_SUCCESS && count) {
      devices.resize(count);
      if (enumerate(instance, &count, devices.data()) != VK_SUCCESS) devices.clear();
      devices.resize(count);
    }
    why = devices.empty() ? "no Vulkan graphics card" : "no graphics card that can share images with the presenter";
    for (VkPhysicalDevice device : devices) {
      uint32_t n = 0;
      if (!extensions || extensions(device, nullptr, &n, nullptr) != VK_SUCCESS) continue;
      std::vector<VkExtensionProperties> props(n);
      if (extensions(device, nullptr, &n, props.data()) != VK_SUCCESS) continue;
      for (const VkExtensionProperties& p : props)
        if (std::strcmp(p.extensionName, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0) usable = true;
      if (usable) break;
    }
    if (usable) why.clear();
    if (destroy) destroy(instance, nullptr);
  }
  FreeLibrary(loader);
  return usable;
}

std::string PresenterDeviceName(rex::ui::Presenter* presenter) {
  auto* p = dynamic_cast<VulkanPresenter*>(presenter);
  if (!p || !p->vulkan_device()) return {};
  VkPhysicalDeviceProperties props{};
  p->vulkan_device()->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(p->vulkan_device()->physical_device(),
                                                                                    &props);
  return props.deviceName;
}

bool Setup(const Renderer& renderer, rex::ui::Presenter* presenter, uint32_t width, uint32_t height,
           const std::vector<uint8_t>& output_spirv, uint32_t constants_size) {
  s = State{};
  s.presenter = dynamic_cast<VulkanPresenter*>(presenter);
  s.vd = s.presenter ? s.presenter->vulkan_device() : nullptr;
  if (!s.vd || !s.vd->extensions().ext_KHR_external_memory_win32) {
    REXKRNL_WARN("PDNATIVE Vulkan: the presenter's device cannot share images (no VK_KHR_external_memory_win32)");
    return false;
  }
  s.dev = s.vd->device();
  s.width = width;
  s.height = height;
  s.constants_size = constants_size;
  // The renderer's functions, through its own loader entry.
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(renderer.get_instance_proc_addr);
  VkInstance rinstance = static_cast<VkInstance>(renderer.instance);
  s.rdev = static_cast<VkDevice>(renderer.device);
  s.rfamily = renderer.queue_family;
  s.r.vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(rinstance, "vkGetDeviceProcAddr"));
  if (!s.r.vkGetDeviceProcAddr) return false;
#define PD_LOAD(name) s.r.name = reinterpret_cast<PFN_##name>(s.r.vkGetDeviceProcAddr(s.rdev, #name))
  PD_LOAD(vkCreateImage);
  PD_LOAD(vkGetImageMemoryRequirements);
  PD_LOAD(vkAllocateMemory);
  PD_LOAD(vkBindImageMemory);
  PD_LOAD(vkCmdPipelineBarrier);
  PD_LOAD(vkCmdCopyImage);
#undef PD_LOAD
  if (!s.r.vkCreateImage || !s.r.vkCmdCopyImage) return false;
  for (uint32_t k = 0; k < kFrames; ++k) {
    if (!make_shared_frame(k)) {
      REXKRNL_WARN("PDNATIVE Vulkan: sharing frame images with the presenter failed");
      return false;
    }
  }
  if (!make_pipeline(output_spirv) || !make_commands() || !make_texture(s.dummy, VK_FORMAT_R8G8B8A8_UNORM, 1, 1) ||
      !make_texture(s.ramp, VK_FORMAT_R16G16B16A16_UNORM, 256, 1)) {
    REXKRNL_WARN("PDNATIVE Vulkan: the output pass could not be set up");
    return false;
  }
  for (uint32_t i = 0; i < kFrames; ++i)
    if (!staging(256 * 8, nullptr, s.ramp_staging[i], s.ramp_staging_memory[i], &s.ramp_staging_type[i],
                 &s.ramp_mapped[i]))
      return false;
  s.ready = true;
  REXKRNL_INFO("PDNATIVE Vulkan frames go to the SDK presenter ({}x{})", width, height);
  return true;
}

uint32_t NextFrame() {
  std::lock_guard lock(g_lock);
  const uint32_t k = s.next;
  s.next = (k + 1) % kFrames;
  if (s.read_by[k] >= 0) {
    wait_slot(uint32_t(s.read_by[k]));
    s.read_by[k] = -1;
  }
  return k;
}

void RecordCopy(void* command_buffer, void* src_image, uint32_t k) {
  VkCommandBuffer c = static_cast<VkCommandBuffer>(command_buffer);
  // Taken back from the presenter's device (or first use), written, handed over.
  VkImageMemoryBarrier b = s.rwritten[k]
                               ? barrier(s.rframe[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_EXTERNAL, s.rfamily)
                               : barrier(s.rframe[k], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                         VK_ACCESS_TRANSFER_WRITE_BIT);
  s.r.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {s.width, s.height, 1};
  s.r.vkCmdCopyImage(c, static_cast<VkImage>(src_image), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.rframe[k],
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  b = barrier(s.rframe[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
              s.rfamily, VK_QUEUE_FAMILY_EXTERNAL);
  s.r.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
  s.rwritten[k] = true;
}

void Present(uint32_t k, const void* constants, const uint16_t* ramp, uint32_t ramp_version) {
  std::lock_guard lock(g_lock);
  if (!s.ready) return;
  std::vector<uint8_t> pc(static_cast<const uint8_t*>(constants), static_cast<const uint8_t*>(constants) + s.constants_size);
  s.presenter->RefreshGuestOutput(s.width, s.height, s.width, s.height,
                                  [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
    auto& c = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
    context.SetIs8bpc(true);
    const auto& dfn = s.vd->functions();
    const uint32_t i = s.slot;
    s.slot = (i + 1) % kFrames;
    wait_slot(i);
    dfn.vkResetCommandPool(s.dev, s.cmd_pool[i], 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cb = s.cmd[i];
    dfn.vkBeginCommandBuffer(cb, &bi);
    if (!s.dummy_ready) {  // a single black texel, uploaded once
      static const uint8_t texel[4] = {0, 0, 0, 255};
      VkBuffer buf;
      VkDeviceMemory mem;
      if (staging(4, texel, buf, mem)) copy_buffer_to_image(cb, buf, s.dummy, VK_IMAGE_LAYOUT_UNDEFINED);
      s.dummy_ready = true;  // (the 4-byte upload buffer is kept)
    }
    if (ramp && ramp_version != s.ramp_version) {
      uint16_t* m = static_cast<uint16_t*>(s.ramp_mapped[i]);
      for (int e = 0; e < 256; ++e) {
        m[e * 4 + 0] = ramp[e];
        m[e * 4 + 1] = ramp[256 + e];
        m[e * 4 + 2] = ramp[512 + e];
        m[e * 4 + 3] = 0xFFFF;
      }
      util::FlushMappedMemoryRange(s.vd, s.ramp_staging_memory[i], s.ramp_staging_type[i]);
      copy_buffer_to_image(cb, s.ramp_staging[i], s.ramp,
                           s.ramp_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED);
      s.ramp_written = true;
      s.ramp_version = ramp_version;
    }
    // Shared frame k from the renderer; the guest output image for writing.
    VkImageMemoryBarrier in[2] = {
        barrier(s.frame[k].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, s.vd->queue_family_graphics_compute()),
        barrier(c.image(),
                c.image_ever_written_previously() ? VulkanPresenter::kGuestOutputInternalLayout
                                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, c.image_ever_written_previously() ? VulkanPresenter::kGuestOutputInternalAccessMask : 0,
                VK_ACCESS_SHADER_WRITE_BIT)};
    dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VulkanPresenter::kGuestOutputInternalStageMask,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, in);
    VkDescriptorImageInfo infos[5] = {
        {VK_NULL_HANDLE, s.frame[k].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, s.dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, s.ramp_written ? s.ramp.view : s.dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
        {VK_NULL_HANDLE, c.image_view(), VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = s.sets[i];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = kBindingTypes[b];
      writes[b].pImageInfo = &infos[b];
    }
    dfn.vkUpdateDescriptorSets(s.dev, 5, writes, 0, nullptr);
    dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &s.sets[i], 0, nullptr);
    dfn.vkCmdPushConstants(cb, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, s.constants_size, pc.data());
    dfn.vkCmdDispatch(cb, (s.width + 7) / 8, (s.height + 7) / 8, 1);
    VkImageMemoryBarrier out[2] = {
        barrier(s.frame[k].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_SHADER_READ_BIT, 0, s.vd->queue_family_graphics_compute(), VK_QUEUE_FAMILY_EXTERNAL),
        barrier(c.image(), VK_IMAGE_LAYOUT_GENERAL, VulkanPresenter::kGuestOutputInternalLayout,
                VK_ACCESS_SHADER_WRITE_BIT, VulkanPresenter::kGuestOutputInternalAccessMask)};
    dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VulkanPresenter::kGuestOutputInternalStageMask | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, out);
    dfn.vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    {
      auto queue = s.vd->AcquireQueue(s.vd->queue_family_graphics_compute(), 0);
      dfn.vkQueueSubmit(queue.queue(), 1, &si, s.fence[i]);
    }
    s.fence_pending[i] = true;
    s.read_by[k] = int(i);
    return true;
  });
}

uint64_t PresentAsync(uint32_t k, const void* constants, const uint16_t* ramp, uint32_t ramp_version,
                      std::function<void()> wait_gpu, std::function<void()> after) {
  uint64_t number;
  Job job;
  job.k = k;
  job.constants.assign(static_cast<const uint8_t*>(constants), static_cast<const uint8_t*>(constants) + s.constants_size);
  if (ramp) job.ramp.assign(ramp, ramp + 768);
  job.ramp_version = ramp_version;
  job.wait_gpu = std::move(wait_gpu);
  job.after = std::move(after);
  {
    std::lock_guard lock(g_jobs_lock);
    if (!g_worker_started) {
      g_worker_started = true;
      std::thread(worker).detach();
    }
    g_jobs.push_back(std::move(job));
    number = ++g_queued;
  }
  g_jobs_cv.notify_one();
  return number;
}

void WaitPresent(uint64_t number) {
  std::unique_lock lock(g_jobs_lock);
  g_idle_cv.wait(lock, [number] { return g_done >= number; });
}

void Stats(uint64_t& queued, uint64_t& done, double& wait_ms, double& refresh_ms) {
  std::lock_guard lock(g_jobs_lock);
  queued = g_queued;
  done = g_done;
  wait_ms = g_wait_ms;
  refresh_ms = g_refresh_ms;
}

}  // namespace pd::native_vk
