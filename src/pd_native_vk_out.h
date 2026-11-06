// Perfect Dark native renderer ("PD Plume") on Vulkan: hands its frames to the
// SDK's Vulkan presenter (FSR, the ImGui overlays and the PC menus), as the
// Direct3D 12 path does with shared resources. Ported from GoldenEye XBLA's
// ge_native_vk_out (GOLDENEYE\GoldenEye-007-XBLA-PC-Recomp) on 2026-11-06,
// without its menu backdrop picture.
//
// The renderer (Plume) and the presenter have a VkDevice each on the same GPU;
// three frame images made on the presenter's device are exported
// (VK_KHR_external_memory_win32, enabled in the SDK's device for this) and
// imported into the renderer's. Each frame the renderer copies its picture into
// one; a worker waits for that frame's GPU work, and the presenter's refresh
// runs the output pass (gamma ramp, Vivid Colours / Black Level) from it into
// the guest output image.
//
// Kept apart from pd_native_gpu.cpp so the SDK's Vulkan headers and Plume's
// (volk) never meet: everything here is plain handles and SDK functions.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex::ui { class Presenter; }

namespace pd::native_vk {

// The renderer's Vulkan objects (VkInstance, VkPhysicalDevice, VkDevice as
// void*) and its loader entry points.
struct Renderer {
  void* instance = nullptr;
  void* physical_device = nullptr;
  void* device = nullptr;
  void* get_instance_proc_addr = nullptr;  // PFN_vkGetInstanceProcAddr
  uint32_t queue_family = 0;
};

// Whether the SDK's presenter draws with Vulkan (gpu = vulkan).
bool PresenterIsVulkan(rex::ui::Presenter* presenter);

// Whether Plume can run on Vulkan on this PC: a Vulkan driver with a GPU that
// can share images (VK_KHR_external_memory_win32). Checked at start, before the
// presenter is started on Vulkan (OnPreSetup); false (with the reason in
// `why`) keeps Plume on Direct3D 12, so a Vulkan choice never leaves the game
// without a picture.
bool VulkanUsable(std::string& why);

// Name of the presenter's GPU, so the renderer can make its device there.
std::string PresenterDeviceName(rex::ui::Presenter* presenter);

// Shared frames of width x height and the output pass (SPIR-V compute shader,
// push constants of `constants_size` bytes). False: not possible here (the
// renderer then does not start, and Xenos carries on).
bool Setup(const Renderer& renderer, rex::ui::Presenter* presenter, uint32_t width, uint32_t height,
           const std::vector<uint8_t>& output_spirv, uint32_t constants_size);

// The next shared frame, waiting until the presenter has finished reading it.
uint32_t NextFrame();

// Records into the renderer's command buffer: `src_image` (in
// TRANSFER_SRC_OPTIMAL) copied into shared frame k.
void RecordCopy(void* command_buffer, void* src_image, uint32_t k);

// Hands shared frame k to the presenter (after the renderer's GPU work for it
// is complete): the output pass with these push constants and the gamma ramp
// (768 display-ready values, or nullptr; uploaded when `ramp_version` changes).
void Present(uint32_t k, const void* constants, const uint16_t* ramp, uint32_t ramp_version);

// The same on a worker thread, so the game thread doesn't wait for its GPU
// work: `wait_gpu` (on the worker) returns once the frame's work is done, then
// the presenter is refreshed and `after` runs (may be empty). Copies
// everything it is given. Returns the hand-off's number (for WaitPresent).
uint64_t PresentAsync(uint32_t k, const void* constants, const uint16_t* ramp, uint32_t ramp_version,
                      std::function<void()> wait_gpu, std::function<void()> after);

// Waits until the worker has finished hand-off `number` and all before it
// (before the renderer reuses what that frame used).
void WaitPresent(uint64_t number);

// Hand-offs given and finished so far, and the worker's time between taking a
// frame and handing it over (GPU wait + refresh), summed in ms, for the log.
void Stats(uint64_t& queued, uint64_t& done, double& wait_ms, double& refresh_ms);

}  // namespace pd::native_vk
