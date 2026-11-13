// Perfect Dark native renderer ("PD Plume"), ported from GoldenEye XBLA's
// (GOLDENEYE\goldeneye-recomp, branch native-renderer, src/ge_native_gpu.cpp)
// on 2026-11-02. Research build: branch native-renderer, gpu_native off by
// default.
//
// Unlike GoldenEye, Perfect Dark keeps the SDK's Xenos graphics system: its
// vblank is the game's clock (fixed_guest_vblank, the network clock sync) and
// its presenter shows the frames. Once the native D3D12 device is up, Swap
// puts the guest D3D in its own "GPU hung" mode (device+10941 bit 2, the same
// bit in the June 2009 XDK: D3D_CBlocker_Check returns at once and the ring is
// no longer kicked), so the Xenos command processor goes idle and every GPU
// wait in the game returns at once; the native Plume backend draws from the
// D3D calls instead and hands each frame to the presenter. If the native
// device cannot start, hung mode is never set and Xenos carries on as usual.
// Every hook runs the original guest function (__imp__sub_*) too.
//
// Shaders come from a local cache of XenosRecomp output (<exe>\native_shaders\
// <hash>.vs|ps.final.dxil, hash = FNV-1a of the shader container), translated
// on this PC from the player's own files; nothing is shipped.
//
// Device offsets and function addresses differ from GoldenEye's (August 2007
// XDK): see native\checkpoint1\offset_map.txt and d3d_functions_pd.toml.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "generated/default/perfect_dark_init.h"  // PPCContext, generated function declarations
#include "pd_native_shaders.h"
#include "pd_native_vk_out.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/api.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/image_decode.h>
#include <rex/filesystem.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/kernel/xboxkrnl/video.h>

#include "plume_render_interface.h"
#include "plume_d3d12.h"
#include "plume_vulkan.h"

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <fstream>
#include <iterator>

#define XXH_INLINE_ALL
#include <xxhash.h>

#if __has_include("renderdoc_app.h")  // test frame captures (renderdoc_tick)
#include "renderdoc_app.h"
#define PD_HAS_RENDERDOC 1
#else
#define PD_HAS_RENDERDOC 0
#endif

// On by default since 0.9.0a; the Renderer row is gone since 0.9.1 (Classic
// stays available by setting this to false).
REXCVAR_DEFINE_BOOL(gpu_native, true, "GPU",
                    "Renderer: Plume, drawing the game's D3D calls natively with Direct3D 12 or Vulkan "
                    "(gpu_native_api; true), or Classic, the emulated Xenos GPU (false). Read once at start");
// The presenter's API follows this (perfect_dark_app.h OnPreSetup sets gpu):
// Plume shares its frames with the SDK presenter's device, so both must match.
REXCVAR_DEFINE_STRING(gpu_native_api, "auto", "GPU",
                      "Plume renderer API: auto (Direct3D 12), d3d12 or vulkan. Read once at start");
REXCVAR_DECLARE(bool, gpu_native_translate);  // pd_native_shaders.cpp
REXCVAR_DEFINE_BOOL(pd_test_pipe, false, "PerfectDark",
                    "Native renderer research: run test commands from test_cmd.txt beside the exe "
                    "(perfect_dark_app.h TickTestPipe; injected input only)");
REXCVAR_DEFINE_INT32(pd_test_rng_seed, 0, "PerfectDark",
                     "Native renderer research: every seed the game sets (boot, match start) becomes this "
                     "value, so a scenario picks the same spawns on both renderers (0 = off)");
REXCVAR_DEFINE_STRING(pd_test_cheats, "", "PerfectDark",
                      "Native renderer research: cheat ids to switch on at each stage's start, as the enabled "
                      "masks' bits in hex, \"<ids 0-31>[:<ids 32+>]\" (empty = off)");
REXCVAR_DEFINE_STRING(gpu_native_skip_ps, "", "GPU",
                      "Native renderer test: skip draws whose pixel shader has this cache hash (16 hex digits)");
REXCVAR_DEFINE_STRING(gpu_native_hide_vs, "", "GPU",
                      "Native renderer test: hide draws whose vertex shader has this cache hash (16 hex digits)");
REXCVAR_DEFINE_STRING(gpu_native_hide, "", "GPU",
                      "Native renderer test: hide draws ""a-b"" of each frame, or triangles ""d:a-b"" of draw d");
REXCVAR_DEFINE_BOOL(gpu_native_debug_layer, false, "GPU",
                    "Native renderer test: enable the D3D12 debug layer and log its messages");
REXCVAR_DEFINE_INT32(gpu_native_trace_frame, 0, "GPU",
                     "Native renderer test: log every draw, clear, resolve and target change of this frame (0 = off)");
REXCVAR_DEFINE_INT32(gpu_native_trace_count, 1, "GPU",
                     "Native renderer test: how many frames gpu_native_trace_frame traces, from that frame on");
REXCVAR_DEFINE_BOOL(gpu_native_dump_textures, false, "GPU",
                    "Native renderer test: save every decoded texture as a BMP in native_textures (game content: keep local)");
REXCVAR_DEFINE_BOOL(gpu_native_trace_new_resolves, false, "GPU",
                    "Native renderer test: trace the rest of the frame in which a new resolve destination appears");
REXCVAR_DEFINE_BOOL(gpu_native_prefer_decl, true, "GPU",
                    "Native renderer: lay out vertices from the bound declaration (as D3D patches the shader's "
                    "fetches) rather than the shader's own compiled-in fetches");
REXCVAR_DEFINE_BOOL(gpu_native_half_pixel, true, "GPU",
                    "Native renderer: shift geometry half a pixel to match the 360's pixel centres");
REXCVAR_DEFINE_BOOL(gpu_native_fill_half_pixel, true, "GPU",
                    "Native renderer: at a Render Scale above 1x, fill the columns and rows the half-pixel shift "
                    "leaves uncovered at the left and top of each resolve (as the classic renderer does)");
REXCVAR_DEFINE_BOOL(gpu_native_all_mips, false, "GPU",
                    "Native renderer test: build a full mip chain for every texture");
// 4 by default: the game draws its scene 4x multisampled, and the Xenos path
// keeps that (native at 1x showed stair-stepped edges Xenos doesn't have).
REXCVAR_DEFINE_INT32(gpu_native_msaa, 4, "GPU",
                     "Native renderer: scene MSAA samples, 4 (as the game asks, and as the Xenos renderer draws), "
                     "2 or 1 (off, the lightest); applied at once");
REXCVAR_DEFINE_BOOL(gpu_native_log_creation, false, "GPU",
                    "Native renderer test: log every pipeline and texture created");
REXCVAR_DEFINE_BOOL(gpu_native_bc_textures, true, "GPU",
                    "Native renderer: keep DXT textures compressed (BC1-3) with the game's own mips");
REXCVAR_DEFINE_BOOL(gpu_native_rdc_capture, false, "GPU",
                    "Test: capture the next frame with RenderDoc when it is attached");
REXCVAR_DEFINE_BOOL(gpu_native_depth_flip_vs, true, "GPU",
                    "Native renderer: draw the game's backwards depth range with a forward viewport and Z flipped "
                    "in the vertex shader, as the classic renderer does (off: a reversed viewport, which Direct3D 12 "
                    "does not officially allow)");
REXCVAR_DEFINE_BOOL(gpu_native_clip_planes, true, "GPU",
                    "Native renderer: honour the game's user clip planes (an open sliding door is cut at its wall "
                    "with one; issue #25)");
REXCVAR_DEFINE_BOOL(gpu_native_depth_copy, true, "GPU",
                    "Native renderer: copy the scene depth for depth resolves (off: the last clear value)");
REXCVAR_DEFINE_BOOL(gpu_native_skip_resolves, false, "GPU",
                    "Native renderer test: ignore resolves (wrong picture; for timing only)");
REXCVAR_DEFINE_INT32(gpu_native_profile_frame, 0, "GPU",
                     "Native renderer test: GPU-time every draw, clear and resolve of this frame and log the costliest");
REXCVAR_DEFINE_DOUBLE(gpu_native_long_frame_ms, 25.0, "GPU",
                      "Native renderer test: with gpu_native_frame_stats, log frames longer than this");
REXCVAR_DEFINE_BOOL(gpu_native_warm_shaders, true, "GPU",
                    "Native renderer: translate a level's shaders while it loads, not at their first draw");
REXCVAR_DEFINE_BOOL(gpu_native_stage_textures, true, "GPU",
                    "Native renderer: start the texture cache afresh on each new stage (level or front end)");
REXCVAR_DEFINE_BOOL(gpu_native_async_textures, true, "GPU",
                    "Native renderer: prepare new textures past ~1 ms a frame on a worker thread (in level)");
// Off by default here: Perfect Dark keeps the Xenos vblank, which already
// reports every vblank to the presenter (GoldenEye had none to anchor to).
REXCVAR_DEFINE_BOOL(gpu_native_pace, false, "GPU",
                    "Native renderer: present each frame at a fixed moment after the frame's usual arrival");
REXCVAR_DEFINE_INT32(gpu_native_pace_margin_us, 2000, "GPU",
                     "Native renderer: how long after a frame's usual arrival it is presented (paced presents)");
REXCVAR_DEFINE_INT32(gpu_native_frame_stats, 0, "GPU",
                     "Log guest frame times (Swap to Swap) every this many frames, native or Xenos (0 = off)");
REXCVAR_DEFINE_INT32(gpu_native_debug_clear, 0, "GPU",
                     "Native renderer test: clear to this 0xRRGGBB instead of the game's colour (0 = off)");

namespace pd {
// Texture Filtering: sampler_slot applies this anisotropy (GoldenEye's
// anisotropic_override setting; 3 = 4x) to linear, mipmapped sampling.
std::atomic<int> g_native_aniso{3};
int NativeAnisotropy() { return g_native_aniso.load(std::memory_order_relaxed); }
// Frames the native renderer has shown (the Xenos command processor's count
// stays at 0 under it): the FPS counter's source (perfect_dark_app.h).
std::atomic<uint64_t> g_native_frames{0};
uint64_t NativeFramesShown() { return g_native_frames.load(std::memory_order_relaxed); }
// The Render Scale this run (pd_render_scale -> resolution_scale, set in OnPreSetup).
int ActiveRenderScale() {
  const std::string v = rex::cvar::GetFlagByName("resolution_scale");
  const int s = v.empty() ? 1 : std::atoi(v.c_str());
  return s >= 1 && s <= 3 ? s : 1;
}
}  // namespace pd

namespace plume {
std::unique_ptr<RenderInterface> CreateD3D12Interface();
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;

namespace {

// --- guest memory -----------------------------------------------------------

inline uint8_t* gmem() { return rex::system::kernel_state()->memory()->virtual_membase(); }
inline uint32_t g32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, gmem() + a, 4);
  return __builtin_bswap32(v);
}
inline uint16_t g16(uint32_t a) {
  uint16_t v;
  std::memcpy(&v, gmem() + a, 2);
  return __builtin_bswap16(v);
}
// Physical memory at 0xE0000000+ is seen 4 KB further on than the raw guest
// address reads (see ge_native_probe.cpp); shader microcode pointers need it.
inline uint32_t gpu_view(uint32_t a) { return a >= 0xE0000000u ? a + 0x1000 : a; }

uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
  return h;
}

void create_scene_targets();
RenderSampleCounts pick_msaa();

// Device fields (offsets into the guest D3D device; see native/d3d_functions.toml).
constexpr uint32_t kDevVsConsts = 1920;     // 256 float4, big-endian
constexpr uint32_t kDevPsConsts = 6016;     // 256 float4
constexpr uint32_t kDevVsBools = 2528 * 4;  // dword index 2528
constexpr uint32_t kDevPsBools = 2532 * 4;
constexpr uint32_t kDevVertexDecl = 11992;
constexpr uint32_t kDevPixelShader = 12868;
constexpr uint32_t kDevVertexShader = 12872;
constexpr uint32_t kDevViewport = 12824;    // X, Y, W, H, MinZ, MaxZ (floats)
// PA_SC_WINDOW_SCISSOR_TL / _BR as the guest D3D writes them (x | y << 16): the
// viewport, cut to the scissor rect while ScissorTestEnable (dev+12032) is on.
constexpr uint32_t kDevWindowScissorTL = 10436;
constexpr uint32_t kDevWindowScissorBR = 10440;
// The device's register shadows are the GPU packets back to back: values
// 0x2100-0x2114 at 10444 (PA_CL_VPORT_XSCALE 0x210F at 10504), program
// 0x2180-0x2184, control 0x2200-0x220B at 10548 (RB_DEPTHCONTROL; PA_CL_VTE_CNTL
// 0x2206 at 10572), tessellator 0x2280-0x2294, misc from 0x2300 at 10680. The
// same offsets as GoldenEye's device: checked in Perfect Dark with the trace
// (vport scale 640 / -360 on full-screen draws, vtx_cntl 0 or 1).
constexpr uint32_t kDevVportXScale = 10504;  // PA_CL_VPORT_XSCALE (float), YSCALE at +8
constexpr uint32_t kDevVtxCntl = 10688;      // PA_SU_VTX_CNTL (0x2302): bit 0 = pixel centre
constexpr uint32_t kDevClipCntl = 10564;     // PA_CL_CLIP_CNTL (0x2204)
// The user clip planes (PA_CL_UCP_0..5), 6 x 4 floats: D3DDevice_SetClipPlane
// (sub_82337698) stores plane n at (642 + n) * 16.
constexpr uint32_t kDevUserClipPlanes = 642 * 16;

// --- native device ----------------------------------------------------------

// Perfect Dark's stage number (perfect_dark_app.h kStageNum); 90 = the title.
constexpr uint32_t kStageNum = 0x826492E0u;

constexpr uint32_t kTextureSlots = 1024;
constexpr uint32_t kSamplerSlots = 64;  // sampler descriptors (set 3); 0 = the default sampler
constexpr uint32_t kFrames = 3;  // frames in flight: a GPU clock stall doesn't block the game thread
constexpr uint64_t kUploadSize = 288ull << 20;  // system memory; a level load fills most of a 96 MB third
constexpr uint64_t kSlotUpload = kUploadSize / kFrames;
constexpr uint64_t kSlotConsts = 8ull << 20;        // shader constants per frame (~8 KB a draw; Dam ~5.3 MB)
// The game draws for a 1280x720 EDRAM frame (guest pixels), or 1920x1080 when
// the video mode is exactly 1920x1080 (its own rule, at 0x82262980; the
// Resolution row sets the mode). The scene targets are that times the Render
// Scale (pd::ActiveRenderScale); both are set at device start. Issue #24: a
// fixed 1280x720 here showed only the top-left of a 1920x1080 frame.
uint32_t kGuestW = 1280, kGuestH = 720;
// The native renderer's API this run: Vulkan when the SDK's presenter is
// (gpu_native_api = vulkan makes OnPreSetup start it on Vulkan).
bool g_vulkan = false;
uint32_t g_scene_scale = 1, g_scene_w = kGuestW, g_scene_h = kGuestH;
constexpr RenderFormat kSceneFormat = RenderFormat::B8G8R8A8_UNORM;

// A vertex input. For declarations: stream = D3D stream, type = D3DDECLTYPE.
// For a shader's own vertex fetches: stream = vertex fetch constant slot, type
// = Xenos format | signed << 8 | integer << 9 (the same bits D3DDECLTYPE uses).
constexpr uint16_t kIdentitySwizzle = 0x688;  // vfetch dst swizzle x,y,z,w (3 bits each)

// Vulkan matches vertex inputs by location: XenosRecomp's [[vk::location]] for
// a D3D declaration usage (USAGE_LOCATIONS in shader_recompiler.cpp).
uint32_t vk_location(uint32_t usage, uint32_t index) {
  switch (usage) {
    case 0: return index == 1 ? 15 : 0;                        // Position
    case 1: return 10;                                         // BlendWeight
    case 2: return 9;                                          // BlendIndices
    case 3: return 1;                                          // Normal
    case 5: return index < 4 ? 4 + index : 12 + (index - 4);   // TexCoord
    case 6: return 2;                                          // Tangent
    case 7: return 3;                                          // Binormal
    case 10: return index == 1 ? 11 : 8;                       // Color
    default: return 0;
  }
}

struct Element {
  uint16_t stream, offset;
  uint32_t type;
  uint8_t usage, index;
  int8_t exp = 0;  // vfetch exponent adjust: values scale by 2^exp
  // Destination swizzle D3D patched into the live vfetch (e.g. BGRA for a
  // D3DCOLOR) where the shader was compiled from an identity template:
  // applied while decoding, since the translated shader doesn't know it.
  uint16_t swizzle = kIdentitySwizzle;
};

struct HostShader {
  std::unique_ptr<RenderShader> shader;
  std::vector<Element> inputs;                  // vertex shaders: from the microcode
  std::vector<uint8_t> container;               // vertex shaders: for re-parsing live microcode
  uint32_t cb_bytes = 4096;                     // constant-buffer bytes the shader reads (<hash>.cbsize)
  std::unordered_map<uint16_t, uint32_t> strides;  // fetch slot -> stride in bytes
  uint64_t hash = 0;
  bool missing = false;
};

struct TextureJob;
struct ResolvedTexture {
  std::unique_ptr<RenderTexture> texture;
  std::unique_ptr<RenderTextureView> view;  // cube maps: the cube view in the descriptor
  uint32_t w = 0, h = 0;                     // colour resolves: the copy's size
  // Colour resolves sampled through a guest texture whose fetch swizzle forces
  // a component to 0 or 1 (X8R8G8B8: alpha 1): a view with that mapping each.
  std::unordered_map<uint16_t, std::pair<std::unique_ptr<RenderTextureView>, uint32_t>> swizzled;
  uint32_t slot = 0;
  std::shared_ptr<TextureJob> job;  // being prepared on the texture worker
  // Textures outside the CAFF heap can be rewritten by the game (classic Dam's
  // 4x4 sky colour ramp): their bytes are re-hashed once a frame when used.
  bool dynamic = false;
  uint32_t addr = 0, span = 0;
  uint64_t content = 0, checked_frame = 0;
  // Full-size depth resolve not copied yet (copy_pending_depth), and the last
  // frame a draw sampled this depth texture (0: never).
  bool depth_pending = false;
  uint64_t depth_used_frame = 0;
};

// A piece of a pooled GPU buffer (vertex or index cache entries). Each committed
// buffer costs 64 KB at least and ~0.5 ms to create: Dam's level load made 500
// of them in one frame.
struct GpuAlloc {
  RenderBuffer* buffer = nullptr;
  uint64_t offset = 0;
  uint32_t size_class = 0;  // capacity = class_bytes(size_class)
  explicit operator bool() const { return buffer != nullptr; }
};

// Size classes: four per doubling from 256 bytes (at most ~19% rounding,
// against up to 2x with powers of two).
constexpr uint32_t kSizeClasses = 128;
inline uint64_t class_bytes(uint32_t c) {
  const uint64_t base = 256ull << (c / 4);
  return base + (base / 4) * (c % 4);
}

struct BufferPool {
  uint64_t chunk = 32ull << 20;
  RenderBufferFlags flags = RenderBufferFlag::NONE;
  std::vector<std::unique_ptr<RenderBuffer>> chunks;
  uint64_t bump = UINT64_MAX;  // into chunks.back(); MAX = no chunk yet
  std::vector<GpuAlloc> free_lists[kSizeClasses];
};

struct Native {
  std::unique_ptr<RenderInterface> api;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  // Two frames in flight: frame N records into slot N % 2 while the GPU may
  // still run frame N-1. A slot's list, fence and half of the upload ring are
  // reused only after waiting for its fence.
  std::unique_ptr<RenderCommandList> cmds[kFrames];
  std::unique_ptr<RenderCommandFence> fences[kFrames];
  RenderCommandList* cmd = nullptr;
  RenderCommandFence* fence = nullptr;
  uint32_t slot = 0;
  bool pending[kFrames] = {};  // a submission signalling that slot's fence not waited for yet (Plume's
                         // wait blocks on an event: never wait without one, or twice)
  struct Retired { uint64_t frame; BufferPool* pool; GpuAlloc alloc; };
  std::vector<Retired> retired;  // returned to their pool 2 frames later
  struct RetiredTexture { uint64_t frame; std::unique_ptr<RenderTexture> texture; uint32_t slot; };
  std::vector<RetiredTexture> retired_textures;  // re-uploaded dynamic textures: freed kFrames frames later
  std::vector<uint32_t> free_slots;
  BufferPool vertex_pool, index_pool;
  std::unique_ptr<RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release;
  std::vector<std::unique_ptr<RenderFramebuffer>> swap_framebuffers;

  // Presenter output: finished frames go to the SDK
  // presenter, which has a D3D12 device of its own (window, ImGui overlays,
  // FSR), through textures and fences shared between the two devices. Ours:
  // copy frame k into tex[k], signal `ready`. The presenter's queue: wait for
  // `ready`, convert tex[k] into its guest output, signal `done`; tex[k] is
  // written again only after that.
  struct Out {
    rex::ui::Presenter* presenter = nullptr;
    ID3D12Device* pdev = nullptr;  // the presenter's device and direct queue
    ID3D12CommandQueue* pqueue = nullptr;
    ID3D12Resource* tex[3] = {};   // shared frames, opened by our device ...
    ID3D12Resource* ptex[3] = {};  // ... and by the presenter's
    ID3D12Fence* ready = nullptr;  // created by ours
    ID3D12Fence* ready_p = nullptr;
    ID3D12Fence* done_p = nullptr;  // created by the presenter's
    ID3D12Fence* done = nullptr;
    uint64_t ready_value = 0, done_value = 0;
    uint64_t tex_ready[3] = {}, tex_done[3] = {}, list_done[3] = {};
    ID3D12CommandAllocator* alloc[3] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT heap_step = 0;
    uint32_t next = 0, next_list = 0;
    // The game's gamma ramp as a 256 x 1 texture (one upload buffer per list).
    ID3D12Resource* ramp = nullptr;
    ID3D12Resource* ramp_upload[3] = {};
    uint32_t ramp_version = 0;
    bool ramp_ready = false;
    HANDLE event = nullptr;
    // Vulkan: frames go through pd_native_vk_out.cpp instead of the above.
    bool vk = false;
    uint64_t vk_handoff[kFrames] = {};  // per frame slot: its hand-off's number (pd::native_vk::WaitPresent)
  } out;
  // D3DDevice_SetGammaRamp: the display gamma ramp the Xenos path applies to
  // every frame on the way out (its 256-entry table, for 8_8_8_8 front buffers).
  uint16_t gamma_ramp[256 * 3] = {};  // R, G, B; 16-bit values
  uint32_t gamma_ramp_version = 0;

  std::unique_ptr<RenderPipelineLayout> layout;
  std::unique_ptr<RenderDescriptorSet> textures;
  std::unique_ptr<RenderDescriptorSet> samplers;
  std::unique_ptr<RenderSampler> default_sampler;
  std::unique_ptr<RenderTexture> white;
  // 1x1 white cube map: what TextureCube descriptor indices point at when the
  // sampler has no cube texture (slot 0 is the 2D white).
  std::unique_ptr<RenderTexture> white_cube;
  std::unique_ptr<RenderTextureView> white_cube_view;
  uint32_t white_cube_slot = 0;
  std::unique_ptr<RenderBuffer> upload;
  // Shader constants: written to a per-slot upload range, copied in one go into
  // a GPU-local buffer by a short list submitted just before the frame's list
  // (the draws' root CBVs point at the GPU-local copy, not at system memory).
  std::unique_ptr<RenderBuffer> const_upload;
  uint8_t* const_ptr = nullptr;
  std::unique_ptr<RenderBuffer> const_gpu[kFrames];
  std::unique_ptr<RenderCommandList> const_lists[kFrames];
  uint64_t const_used = 0;
  std::unordered_map<uint64_t, uint64_t> const_dedup;  // block hash -> offset, this submission
  std::unique_ptr<RenderQueryPool> timestamps;  // GPU time per frame: [0] frame start, [1] end
  // One-frame profile (gpu_native_profile_frame): a timestamp after every
  // operation; the time since the previous one is that operation's.
  std::unique_ptr<RenderQueryPool> prof_pool;
  std::vector<std::string> prof_labels;
  bool prof_active = false;
  double gpu_ms_sum = 0, gpu_ms_max = 0, copy_ms_sum = 0;
  uint32_t gpu_frames = 0;
  uint8_t* upload_ptr = nullptr;
  uint64_t upload_used = 0;  // within the current slot's half

  RenderMultisampling msaa;  // scene sample count (gpu_native_msaa)
  // Render targets. The game's scene target (bound with a depth surface) is
  // the MSAA scene above; a target bound without depth (the post-process pass
  // into 400076A0) gets a single-sample colour target of its own, per guest
  // surface. bind_target() makes the bound one match what the game has set.
  struct ColorTarget {
    std::unique_ptr<RenderTexture> color;
    std::unique_ptr<RenderFramebuffer> fb;
  };
  std::unordered_map<uint32_t, ColorTarget> color_targets;
  uint32_t guest_rt = 0, guest_depth = 0, front_buffer = 0, scene_surface = 0;
  RenderFramebuffer* bound_fb = nullptr;
  RenderTexture* bound_color = nullptr;
  bool bound_scene = true;
  uint32_t last_resolve_dest = 0;
  bool draws_since_resolve = true;
  bool last_draw_half_pixel = false;  // the last draw was moved half a pixel (set_common_state)
  // One column and one row of the scene's size: native_resolve_copy's half-pixel
  // fill copies through them (a texture can't be copied onto itself).
  std::unique_ptr<RenderTexture> gap_column, gap_row;
  uint32_t gap_w = 0, gap_h = 0;
  // Scene depth since its last clear: the clear value, and whether a draw has
  // written depth since (then a depth resolve would need the real contents).
  float depth_clear = 1.0f;
  bool depth_written = true;
  // A resolve's clears (flags 0x100 / 0x200) waiting for the scene to be used
  // again (apply_pending_clears): Perfect Dark resolves the scene with them,
  // then binds a second target on the same EDRAM and resolves that to the
  // front buffer with nothing drawn, so that copy has to see the picture.
  bool pending_clear_color = false, pending_clear_depth = false;
  RenderColor pending_color;
  float pending_depth = 1.0f;
  // Bumped by every depth-writing draw and depth clear: a full depth resolve
  // whose destination already holds this generation is skipped (the game
  // resolves the same depth more than once a frame).
  uint64_t depth_gen = 1;
  uint32_t depth_copies = 0;  // full depth copies, for the 600-frame log line
  std::vector<uint32_t> depth_pending;  // destinations with ResolvedTexture::depth_pending
  // Depth resolve pass (native_depth_resolve): sample 0 of the scene depth
  // into an R32F texture per destination.
  std::unique_ptr<RenderPipelineLayout> depth_layout;
  std::unique_ptr<RenderShader> depth_shader;
  std::unique_ptr<RenderPipeline> depth_pipe;
  std::unique_ptr<RenderTextureView> depth_view;  // R32_FLOAT view of the typeless scene depth
  bool depth_pass_failed = false;
  std::unordered_map<uint32_t, std::unique_ptr<RenderDescriptorSet>> depth_sets;
  std::unique_ptr<RenderTexture> scene;
  std::unique_ptr<RenderTexture> scene_depth;
  std::unique_ptr<RenderFramebuffer> scene_fb;

  std::unordered_map<uint32_t, HostShader> shaders;            // guest shader object
  std::unordered_map<uint32_t, uint32_t> created_blobs;        // object -> container (boot shaders)
  // Shaders a level registered in place (warm_registered_shaders): object ->
  // vertex?, the hash its bytes had last frame and the frame it was registered.
  struct WarmEntry { bool vertex; uint64_t last_hash; uint64_t frame; };
  std::unordered_map<uint32_t, WarmEntry> warm;
  uint32_t warm_requested = 0;
  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> pipelines;
  std::unordered_map<uint32_t, ResolvedTexture> resolved;      // guest texture -> host copy
  // Resolve destinations by their memory (fetch constant base address): another
  // texture object made over the same memory samples the resolved picture too.
  // In hung mode nothing writes resolves to guest memory, so decoding such a
  // texture from memory gave black (the Perfect Menu's blurred background).
  std::unordered_map<uint32_t, uint32_t> resolved_base;      // base address -> guest texture in `resolved`
  std::unordered_map<uint64_t, ResolvedTexture> textures_by_fetch;  // fetch constant hash -> upload
  uint32_t textures_stage = 0;  // the stage textures_by_fetch was filled in (begin_frame)
  std::vector<std::unique_ptr<RenderSampler>> sampler_objects;
  std::unordered_map<uint64_t, uint32_t> sampler_slots;        // filter/clamp/mip key -> sampler slot
  // Decoded vertex data kept on the GPU between frames (see cached_vertices).
  struct VertexCacheEntry {
    GpuAlloc buffer;
    uint64_t capacity = 0, content = 0, last_frame = 0;
    uint32_t misses = 0;  // content changes seen: past a few, decoded into the ring every time
  };
  std::unordered_map<uint64_t, VertexCacheEntry> vertex_cache;
  // Converted 32-bit index lists of indexed draws, the same way (index fetch from
  // the upload heap, i.e. system memory, made Dam's big draws slow on the GPU).
  struct IndexCacheEntry {
    GpuAlloc buffer;
    uint64_t capacity = 0, content = 0, last_frame = 0;
    uint32_t misses = 0, count = 0, vcount = 0;
    int64_t lo = 0;
    RenderPrimitiveTopology topo = RenderPrimitiveTopology::TRIANGLE_LIST;
  };
  std::unordered_map<uint64_t, IndexCacheEntry> index_cache;
  uint32_t vcache_hits = 0, vcache_misses = 0, last_vcache_hits = 0, last_vcache_misses = 0;
  uint32_t next_slot = 1;

  bool failed = false;
  bool recording = false;
  bool layout_bound = false;  // root signature + descriptor tables set on the current list
  uint64_t frames = 0;
  uint32_t draws = 0, skipped = 0;
  uint32_t skip_reason[8] = {};  // shader, decl, prim, range, stream, pipeline, upload
  // Creation work on the game thread this frame (and the last presented one).
  double pipe_ms = 0, tex_ms = 0, last_pipe_ms = 0, last_tex_ms = 0;
  double sect[5] = {}, last_sect[5] = {};
  double present_ms[4] = {};  // last present: acquire, submit, present, fence wait
  double tex_decode_ms = 0, tex_mip_ms = 0, last_tex_decode_ms = 0, last_tex_mip_ms = 0;
  double vdecode_ms = 0, last_vdecode_ms = 0;
  double flush_ms = 0, alloc_ms = 0, hash_ms = 0, last_flush_ms = 0, last_alloc_ms = 0, last_hash_ms = 0;
  uint32_t flushes = 0, last_flushes = 0;  // draw path: parse, indices, decode, state+submit, total
  uint32_t pipe_n = 0, tex_n = 0, last_pipe_n = 0, last_tex_n = 0;
  uint32_t tex_requests = 0;  // new textures asked for this frame (many = a level load)
  // Present pacing and its statistics.
  double pace_period = 1000.0 / 60.0, pace_next = 0, pace_last_arrival = 0, pace_waited_ms = 0;
  double pace_last_present = 0, pace_wait_sum = 0;
  std::vector<double> present_intervals;
  uint32_t last_counter = 0, double_steps = 0;
};

Native g;

// Occlusion queries (see query_begin): per guest query object, its results.
constexpr uint32_t kMaxQueries = 2048;
constexpr uint32_t kQueryWaitMs = 8;  // longest GetData waits for last frame's GPU work
struct Occlusion {
  bool ready = false;                              // set up (either API)
  ID3D12QueryHeap* heap = nullptr;
  ID3D12Resource* readback = nullptr;
  const uint64_t* mapped = nullptr;
  bool failed = false;
  HANDLE wait_event = nullptr;                     // GetData's wait for a frame's GPU work
  DWORD thread = 0;                                // the thread that issues queries
  // Vulkan: a query must be reset before it begins (outside a render pass)
  // and must begin and end inside one render pass, and its result is copied
  // out outside one. So each command list resets its frame slot's range once,
  // before its first query; a query is begun right before the draw it brackets
  // and ended right after it; the results are copied at the list's end.
  VkQueryPool vk_pool = VK_NULL_HANDLE;
  std::unique_ptr<RenderBuffer> vk_readback;
  bool list_reset = false;                         // this list has reset its slot's range
  std::vector<uint8_t> vk_used;                    // per query: begun in this list (no second begin)
  std::vector<uint32_t> vk_copy;                   // query indices ended in this list, to copy
  bool vk_begun = false;                           // the open query is running on the GPU
  bool vk_ended = false;                           // ... and has ended (after its first draw)
  uint32_t vk_extra_draws = 0, vk_repeats = 0;     // draws past the first in a query; second begins in a list
  struct Slot {
    uint32_t owner = 0;           // its guest query object
    uint64_t ended[kFrames] = {}; // per frame slot: the frame (g.frames + 1) that ended it there, 0 = none
    uint64_t result_frame = 0;    // the frame `visible` came from, 0 = no result yet
    uint8_t visible = 0;
    bool empty[kFrames] = {};     // Vulkan: that frame's query drew nothing (hidden; nothing copied)
  };
  std::unordered_map<uint32_t, uint32_t> slot_of;  // guest query object -> slot
  std::vector<Slot> slots;
  std::vector<uint32_t> free_slots;                // given back by D3DQuery_Release
  uint32_t next_reuse = 0;                         // slots handed on round-robin once all are taken
  uint64_t submitted[kFrames] = {};                // per frame slot: the last frame submitted from it
  uint64_t no_wait_frame = 0;                      // a wait ran out this frame: no more waits in it
  int32_t open = -1;                               // slot begun in the list being recorded
  uint32_t begins = 0, reads = 0, visible_reads = 0, waits = 0, late = 0, released = 0, handed_on = 0;
  uint32_t released_elsewhere = 0;
  double wait_ms = 0;
} g_occ;

// Test flight recorder (gpu_native_trace_new_resolves): every frame is traced
// into a ring of the last 1500 lines, written to the log when a new resolve
// destination appears (so the frames that led up to it can be read).
std::deque<std::string> g_trace_ring;
inline bool recording_trace() { return REXCVAR_GET(gpu_native_trace_new_resolves); }
inline bool tracing() {
  if (recording_trace()) return true;
  const int32_t f = REXCVAR_GET(gpu_native_trace_frame);
  return f > 0 && g.frames + 1 >= uint64_t(f) && g.frames + 1 < uint64_t(f) + uint64_t(std::max(1, REXCVAR_GET(gpu_native_trace_count)));
}
void trace_line(std::string s) {
  if (!recording_trace()) { REXKRNL_INFO("PDNATIVE trace {}", s); return; }
  g_trace_ring.push_back(std::move(s));
  if (g_trace_ring.size() > 12000) g_trace_ring.pop_front();
}
void dump_trace_ring(const char* why) {
  REXKRNL_INFO("PDNATIVE flight recorder ({}): {} lines", why, g_trace_ring.size());
  for (const std::string& s : g_trace_ring) REXKRNL_INFO("PDNATIVE rec {}", s);
  g_trace_ring.clear();
}
#define NTRACE(...) do { if (tracing()) trace_line(fmt::format(__VA_ARGS__)); } while (0)

struct Stream {
  uint32_t vb = 0, offset = 0, stride = 0;
};
Stream g_streams[16];
uint32_t g_textures[32];                                   // guest texture per sampler
std::atomic<uint32_t> g_clear_color{0xFF000000u};
uint32_t g_spec_bits = 2;
uint32_t g_vs_cb_bytes = 4096, g_ps_cb_bytes = 3584;  // what the current draw's shaders read  // shared c17.y: bit 0 R11G11B10 normals, bit 1 alpha test

std::wstring exe_dir() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s(path, path + n);
  const size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

void flush_mid_frame();
void warm_pipelines();
uint32_t alloc_texture_slot();
void close_open_query();
bool vk_query_before_draw();
void vk_query_after_draw();

// Makes room for `size` more bytes of upload data, submitting the frame so far
// and waiting for it if the ring is too full. Only call it where no command
// state for the current draw has been recorded yet (the start of a draw, or a
// texture upload done before the draw's own allocations).
void ensure_room(uint64_t size) {
  if (g.upload_used + size + 4096 > kSlotUpload || g.const_used + 16384 > kSlotConsts) flush_mid_frame();
}

uint64_t const_alloc(uint64_t size) {
  const uint64_t off = (g.const_used + 255) & ~255ull;
  if (off + size > kSlotConsts) return UINT64_MAX;
  g.const_used = off + size;
  return off;
}

// Submits the current slot's list, preceded by the copy of this submission's
// constants into the GPU-local constant buffer.
void submit_frame_lists(RenderCommandSemaphore** waits, uint32_t wait_count, RenderCommandSemaphore** signals,
                        uint32_t signal_count) {
  const RenderCommandList* lists[2];
  uint32_t n = 0;
  if (g.const_used) {
    RenderCommandList* c = g.const_lists[g.slot].get();
    RenderBuffer* dst = g.const_gpu[g.slot].get();
    c->begin();
    if (g.timestamps) {
      // (Vulkan: a timestamp query is reset before each write.)
      if (g_vulkan) c->resetQueryPool(g.timestamps.get(), 2 * kFrames + g.slot, 1);
      c->writeTimestamp(g.timestamps.get(), 2 * kFrames + g.slot);
    }
    c->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(dst, RenderBufferAccess::WRITE));
    c->copyBufferRegion(RenderBufferReference(dst, 0), RenderBufferReference(g.const_upload.get(), g.slot * kSlotConsts),
                        g.const_used);
    c->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(dst, RenderBufferAccess::READ));
    c->end();
    lists[n++] = c;
  }
  lists[n++] = g.cmd;
  g.queue->executeCommandLists(lists, n, waits, wait_count, signals, signal_count, g.fence);
}

double now_ms() {
  static LARGE_INTEGER freq{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

GpuAlloc pool_alloc_impl(BufferPool& pool, uint64_t size);
GpuAlloc pool_alloc(BufferPool& pool, uint64_t size) {
  const double t0 = now_ms();
  const GpuAlloc a = pool_alloc_impl(pool, size);
  g.alloc_ms += now_ms() - t0;
  return a;
}

GpuAlloc pool_alloc_impl(BufferPool& pool, uint64_t size) {
  uint32_t cls = 0;
  while (cls + 1 < kSizeClasses && class_bytes(cls) < size) ++cls;
  if (!pool.free_lists[cls].empty()) {
    const GpuAlloc a = pool.free_lists[cls].back();
    pool.free_lists[cls].pop_back();
    return a;
  }
  const uint64_t bytes = class_bytes(cls);
  if (bytes > pool.chunk) {  // larger than a chunk: a buffer of its own
    pool.chunks.insert(pool.chunks.begin(), g.device->createBuffer(RenderBufferDesc::DefaultBuffer(bytes, pool.flags)));
    return GpuAlloc{pool.chunks.front().get(), 0, cls};
  }
  if (pool.bump == UINT64_MAX || pool.bump + bytes > pool.chunk) {
    pool.chunks.push_back(g.device->createBuffer(RenderBufferDesc::DefaultBuffer(pool.chunk, pool.flags)));
    pool.bump = 0;
  }
  const GpuAlloc a{pool.chunks.back().get(), pool.bump, cls};
  pool.bump += bytes;
  return a;
}

// Back to its pool's free list once no frame in flight can use it.
void retire(BufferPool& pool, GpuAlloc& a) {
  if (!a) return;
  g.retired.push_back({g.frames, &pool, a});
  a = GpuAlloc{};
}

uint64_t upload_alloc(uint64_t size, uint64_t align = 256) {
  uint64_t off = (g.upload_used + align - 1) & ~(align - 1);
  if (off + size > kSlotUpload) return UINT64_MAX;
  g.upload_used = off + size;
  return g.slot * kSlotUpload + off;
}


// D3D12 debug layer (gpu_native_debug_layer): messages are copied to the game log.
void drain_debug_messages(const char* where) {
  if (!REXCVAR_GET(gpu_native_debug_layer) || !g.device || g_vulkan) return;
  auto* dev = static_cast<D3D12Device*>(g.device.get())->d3d;
  ID3D12InfoQueue* q = nullptr;
  if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&q)))) return;
  const UINT64 n = q->GetNumStoredMessages();
  for (UINT64 i = 0; i < n && i < 20; ++i) {
    SIZE_T len = 0;
    q->GetMessage(i, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
    if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
      REXKRNL_WARN("PDNATIVE d3d12 ({}): {}", where, std::string(m->pDescription, m->DescriptionByteLength));
  }
  q->ClearStoredMessages();
  q->Release();
  rex::FlushLogging();
}

rex::ui::d3d12::D3D12Provider* sdk_provider() {
  auto* ks = rex::system::kernel_state();
  auto* rt = ks ? ks->emulator() : nullptr;
  auto* gs = rt ? rt->graphics_system() : nullptr;
  if (!gs || !gs->presenter() || !gs->provider()) return nullptr;
  // Perfect Dark keeps the SDK's Xenos graphics system; OnPreSetup forces gpu = d3d12.
  return dynamic_cast<rex::ui::d3d12::D3D12Provider*>(gs->provider());
}

// The presenter's adapter by name, so Plume opens its device on the same GPU
// (shared textures only work within one adapter).
std::string sdk_adapter_name() {
  auto* provider = sdk_provider();
  if (!provider) return {};
  IDXGIFactory4* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  std::string name;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(factory->EnumAdapterByLuid(provider->GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      char utf8[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
      name = utf8;
    }
    adapter->Release();
  }
  factory->Release();
  return name;
}

// The output pass on the presenter's device: converts a shared B8G8R8A8 frame
// into its R10G10B10A2 guest output, applying the gamma ramp and then Vivid
// Colours / Black Level (as the Xenos path's apply_gamma_color does). GoldenEye's
// version also drew its menu backdrop here; the bindings are kept as they were
// (t1 unused). On Vulkan (pd_native_vk_out.cpp) the same pass runs with push
// constants and bindings 0-4.
constexpr char kOutputShader[] = R"(
#ifdef __spirv__
struct PushConsts {
  uint2 size;
  float uv_scale_x, uv_scale_y, uv_offset_x, uv_offset_y;
  float key_r, key_g, key_b, key_tolerance;
  uint layout_4x3;
  uint mode;
  float black_level, contrast, saturation, vibrance, temperature, gamma_;
  float tint_r, tint_g, tint_b, tint_strength;
};
[[vk::push_constant]] ConstantBuffer<PushConsts> pc;
#define size pc.size
#define mode pc.mode
#define black_level pc.black_level
#define contrast pc.contrast
#define saturation pc.saturation
[[vk::binding(0)]] Texture2D<float4> src;
[[vk::binding(1)]] Texture2D<float4> image;
[[vk::binding(2)]] Texture2D<float4> ramp;
[[vk::binding(3)]] SamplerState image_sampler;
[[vk::binding(4)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> dst;
#else
cbuffer push_consts : register(b0) {
  uint2 size;
  float uv_scale_x, uv_scale_y, uv_offset_x, uv_offset_y;
  float key_r, key_g, key_b, key_tolerance;
  uint layout_4x3;
  uint mode;  // 8 gamma ramp, 16 colour adjustment
  float black_level, contrast, saturation, vibrance, temperature, gamma_;
  float tint_r, tint_g, tint_b, tint_strength;
};
Texture2D<float4> src : register(t0);
Texture2D<float4> image : register(t1);
Texture2D<float4> ramp : register(t2);  // the game's gamma ramp: 256 x 1, R/G/B tables
SamplerState image_sampler : register(s0);
RWTexture2D<float4> dst : register(u0);
#endif

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  [branch] if (id.x >= size.x || id.y >= size.y) return;
  float3 c = src[id.xy].rgb;
  [branch] if (mode & 8u) {
    uint3 i = uint3(round(saturate(c) * 255.0));
    c = float3(ramp.Load(int3(i.r, 0, 0)).r, ramp.Load(int3(i.g, 0, 0)).g, ramp.Load(int3(i.b, 0, 0)).b);
  }
  [branch] if (mode & 16u) {
    c = saturate((c - black_level) / (1.0 - black_level));
    float luma = dot(c, float3(0.299, 0.587, 0.114));
    c = luma + (c - luma) * saturation;
    c = saturate((c - 0.25) * contrast + 0.25);  // pivot on a dark grey: the game is dark
  }
  dst[id.xy] = float4(c, 1.0);
}
)";

bool share_handle(ID3D12Device* from, ID3D12DeviceChild* object, ID3D12Device* to, REFIID iid, void** opened) {
  HANDLE h = nullptr;
  if (FAILED(from->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &h))) return false;
  const bool ok = SUCCEEDED(to->OpenSharedHandle(h, iid, opened));
  CloseHandle(h);
  return ok;
}

struct OutputConstants {  // kOutputShader's push_consts
  uint32_t size[2];
  float uv_scale[2], uv_offset[2];
  float key[3], key_tolerance;
  uint32_t layout_4x3, mode;
  float black_level, contrast, saturation, vibrance, temperature, gamma;
  float tint_r, tint_g, tint_b, tint_strength;
};

bool setup_presenter_output() {
  rex::ui::d3d12::D3D12Provider* provider = sdk_provider();
  if (!provider) return false;
  Native::Out& o = g.out;
  ID3D12Device* dev = static_cast<D3D12Device*>(g.device.get())->d3d;
  o.pdev = provider->GetDevice();
  o.pqueue = provider->GetDirectQueue();
  const LUID a = dev->GetAdapterLuid(), b = o.pdev->GetAdapterLuid();
  if (a.LowPart != b.LowPart || a.HighPart != b.HighPart) {
    REXKRNL_WARN("PDNATIVE the presenter is on another GPU: using a swap chain of our own");
    return false;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = g_scene_w;
  rd.Height = g_scene_h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rd.SampleDesc.Count = 1;
  for (int i = 0; i < 3; ++i) {
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&o.tex[i]))) ||
        !share_handle(dev, o.tex[i], o.pdev, IID_PPV_ARGS(&o.ptex[i])))
      return false;
  }
  if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&o.ready))) ||
      !share_handle(dev, o.ready, o.pdev, IID_PPV_ARGS(&o.ready_p)) ||
      FAILED(o.pdev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&o.done_p))) ||
      !share_handle(o.pdev, o.done_p, dev, IID_PPV_ARGS(&o.done)))
    return false;

  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  if (FAILED(D3DCompile(kOutputShader, sizeof(kOutputShader) - 1, "pd_native_output", nullptr, nullptr, "main",
                        "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
    REXKRNL_ERROR("PDNATIVE output shader: {}",
                  errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "");
    if (errors) errors->Release();
    return false;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 3;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].OffsetInDescriptorsFromTableStart = 3;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = sizeof(OutputConstants) / 4;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_ROOT_SIGNATURE_DESC rs{};
  rs.NumParameters = 2;
  rs.pParameters = params;
  rs.NumStaticSamplers = 1;
  rs.pStaticSamplers = &sampler;
  ID3DBlob* rs_blob = nullptr;
  bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)) &&
            SUCCEEDED(o.pdev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&o.root)));
  if (rs_blob) rs_blob->Release();
  if (ok) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = o.root;
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ok = SUCCEEDED(o.pdev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&o.pso)));
  }
  code->Release();
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 12;  // frame, backdrop, gamma ramp and output per command list
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ok = ok && SUCCEEDED(o.pdev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&o.heap)));
  o.heap_step = o.pdev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (int i = 0; ok && i < 3; ++i)
    ok = SUCCEEDED(o.pdev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&o.alloc[i])));
  ok = ok && SUCCEEDED(o.pdev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, o.alloc[0], nullptr,
                                                 IID_PPV_ARGS(&o.list))) &&
       SUCCEEDED(o.list->Close());
  o.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!ok || !o.event) return false;
  o.presenter = static_cast<rex::system::IGraphicsSystem*>(rex::system::kernel_state()->emulator()->graphics_system())
                    ->presenter();
  return true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

float cvar_float(const char* name, float def) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v.empty() ? def : float(std::atof(v.c_str()));
}

// The output pass's settings for this frame: the size, and Vivid Colours /
// Black Level read live from the SDK's cvars with its clamps (GetColorAdjust);
// the ramp's bit is set by the caller.
void output_constants(OutputConstants& c) {
  c = {};
  c.size[0] = g_scene_w;
  c.size[1] = g_scene_h;
  c.saturation = std::clamp(cvar_float("color_saturation", 1.0f), 0.0f, 3.0f);
  c.contrast = std::clamp(cvar_float("color_contrast", 1.0f), 0.5f, 2.0f);
  c.black_level = std::clamp(cvar_float("color_black_level", 0.0f), 0.0f, 0.25f);
  if (c.saturation != 1.0f || c.contrast != 1.0f || c.black_level != 0.0f) c.mode |= 16;
}

// The 360's D3D adjusts every gamma ramp for the display it is told about
// (VdGetCurrentDisplayGamma; the SDK reports a BT.709 HDTV) before writing it
// to the GPU: sRGB-encoded in, BT.709-encoded out. Under Xenos this is what
// turns GoldenEye's identity ramp into the darker table it shows with (entry
// 64 -> 194 of 1023); the native renderer applies the same.
uint16_t for_display(uint16_t value) {
  const double v = value / 65535.0;
  const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  const double out = linear < 0.018 ? 4.5 * linear : 1.099 * std::pow(linear, 0.45) - 0.099;
  return uint16_t(std::clamp(out, 0.0, 1.0) * 65535.0 + 0.5);
}

// Uploads the game's gamma ramp when it changed (recorded into `list`; list
// slot `a` owns the upload buffer). Returns whether the ramp texture exists.
bool update_gamma_ramp(ID3D12GraphicsCommandList* list, uint32_t a) {
  Native::Out& o = g.out;
  // Never set by the game (Perfect Dark never sets one): D3D's own identity
  // ramp, which Xenos still shows through the display adjustment above (a
  // shot under Xenos maps sRGB 60 -> 44, 136 -> 124: for_display exactly).
  if (!g.gamma_ramp_version) {
    for (uint32_t i = 0; i < 256; ++i) g.gamma_ramp[i] = g.gamma_ramp[256 + i] = g.gamma_ramp[512 + i] = uint16_t(i * 257);
    g.gamma_ramp_version = 1;
  }
  if (!o.ramp) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 256;
    td.Height = 1;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(o.pdev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&o.ramp))))
      return false;
    o.ramp_version = 0;
  }
  if (o.ramp_version != g.gamma_ramp_version) {
    if (!o.ramp_upload[a]) {
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC bd{};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = 256 * 8;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(o.pdev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&o.ramp_upload[a]))))
        return o.ramp_ready;
    }
    uint16_t* mapped = nullptr;
    if (FAILED(o.ramp_upload[a]->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return o.ramp_ready;
    for (int i = 0; i < 256; ++i) {
      mapped[i * 4 + 0] = for_display(g.gamma_ramp[i]);
      mapped[i * 4 + 1] = for_display(g.gamma_ramp[256 + i]);
      mapped[i * 4 + 2] = for_display(g.gamma_ramp[512 + i]);
      mapped[i * 4 + 3] = 0xFFFF;
    }
    o.ramp_upload[a]->Unmap(0, nullptr);
    if (o.ramp_ready) {
      D3D12_RESOURCE_BARRIER b = transition(o.ramp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = o.ramp;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = o.ramp_upload[a];
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_UNORM, 256, 1, 1, 256 * 8};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER b = transition(o.ramp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
    o.ramp_version = g.gamma_ramp_version;
    o.ramp_ready = true;
  }
  return o.ramp_ready;
}

// Hands shared frame k to the presenter: its queue waits for our copy, then
// converts the frame into the guest output it paints (with FSR, the overlays).
void present_to_presenter(uint32_t k) {
  Native::Out& o = g.out;
  o.presenter->RefreshGuestOutput(g_scene_w, g_scene_h, g_scene_w, g_scene_h,
                                  [k, &o](rex::ui::Presenter::GuestOutputRefreshContext& context) {
    auto& c = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
    context.SetIs8bpc(true);
    const uint32_t a = o.next_list;
    o.next_list = (a + 1) % 3;
    if (o.list_done[a] && o.done_p->GetCompletedValue() < o.list_done[a]) {
      o.done_p->SetEventOnCompletion(o.list_done[a], o.event);
      WaitForSingleObject(o.event, INFINITE);
    }
    o.alloc[a]->Reset();
    o.list->Reset(o.alloc[a], o.pso);
    ID3D12Resource* dst = c.resource_uav_capable();
    OutputConstants constants;
    output_constants(constants);
    ID3D12Resource* picture = nullptr;  // (GoldenEye's backdrop picture: none here)
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = o.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(a) * 4 * o.heap_step;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    o.pdev->CreateShaderResourceView(o.ptex[k], &sv, cpu);
    cpu.ptr += o.heap_step;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    o.pdev->CreateShaderResourceView(picture, &sv, cpu);
    cpu.ptr += o.heap_step;
    sv.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    o.pdev->CreateShaderResourceView(update_gamma_ramp(o.list, a) ? o.ramp : nullptr, &sv, cpu);
    if (o.ramp_ready) constants.mode |= 8;
    cpu.ptr += o.heap_step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    o.pdev->CreateUnorderedAccessView(dst, nullptr, &uv, cpu);
    constexpr auto kOutState = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
    D3D12_RESOURCE_BARRIER in[2] = {
        transition(o.ptex[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(dst, kOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    o.list->ResourceBarrier(2, in);
    o.list->SetComputeRootSignature(o.root);
    o.list->SetDescriptorHeaps(1, &o.heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = o.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(a) * 4 * o.heap_step;
    o.list->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
    o.list->SetComputeRootDescriptorTable(1, gpu);
    o.list->Dispatch((g_scene_w + 7) / 8, (g_scene_h + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER back[2] = {
        transition(o.ptex[k], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOutState)};
    o.list->ResourceBarrier(2, back);
    o.list->Close();
    {  // how far behind each queue is when a frame is handed over (in frames)
      static uint64_t frames = 0, ready_lag = 0, done_lag = 0, ready_most = 0, done_most = 0;
      const uint64_t r = o.ready_value - o.ready->GetCompletedValue(), d = o.done_value - o.done_p->GetCompletedValue();
      ready_lag += r; done_lag += d;
      ready_most = std::max(ready_most, r); done_most = std::max(done_most, d);
      if (++frames == 600) {
        REXKRNL_INFO("PDNATIVE queue lag at hand-over: native {:.2f} frames (most {}), presenter {:.2f} (most {})",
                     double(ready_lag) / frames, ready_most, double(done_lag) / frames, done_most);
        frames = ready_lag = done_lag = ready_most = done_most = 0;
      }
    }
    o.pqueue->Wait(o.ready_p, o.tex_ready[k]);
    ID3D12CommandList* lists[] = {o.list};
    o.pqueue->ExecuteCommandLists(1, lists);
    o.pqueue->Signal(o.done_p, ++o.done_value);
    o.list_done[a] = o.tex_done[k] = o.done_value;
    return true;
  });
}

void prof_mark(std::string label);

// Copies the frame Swap was given into shared frame k (recorded at the end of
// the frame's command list).
bool copy_to_presenter_texture(uint32_t k) {
  const auto front = g.resolved.find(g.front_buffer);
  if (front == g.resolved.end() || !front->second.texture) {
    static uint32_t missing = 0;
    if ((missing++ % 60) == 0)
      REXKRNL_INFO("PDNATIVE no resolved image for front buffer {:08X} ({} frames so far)", g.front_buffer, missing);
    return false;
  }
  RenderTexture* src = front->second.texture.get();
  g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE));
  ID3D12GraphicsCommandList* l = static_cast<D3D12CommandList*>(g.cmd)->d3d;
  D3D12_RESOURCE_BARRIER b = transition(g.out.tex[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
  l->ResourceBarrier(1, &b);
  l->CopyResource(g.out.tex[k], static_cast<D3D12Texture*>(src)->d3d);
  b = transition(g.out.tex[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
  l->ResourceBarrier(1, &b);
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(src, RenderTextureLayout::SHADER_READ));
  prof_mark("present copy");
  return true;
}

// --- Vulkan output (pd_native_vk_out.cpp) -----------------------------------

rex::ui::Presenter* sdk_presenter() {
  auto* ks = rex::system::kernel_state();
  auto* rt = ks ? ks->emulator() : nullptr;
  auto* gs = rt ? rt->graphics_system() : nullptr;
  return gs ? gs->presenter() : nullptr;
}

// Frames to the SDK's Vulkan presenter: three images shared between its device
// and ours, and the output pass (as SPIR-V) on its device.
bool setup_presenter_output_vk() {
  rex::ui::Presenter* presenter = sdk_presenter();
  if (!pd::native_vk::PresenterIsVulkan(presenter)) return false;
  std::vector<uint8_t> spirv;
  std::string errors;
  if (!pd::native_shaders::CompileDxil(kOutputShader, L"cs_6_0", spirv, errors)) {
    REXKRNL_ERROR("PDNATIVE Vulkan output shader: {}", errors.substr(0, 400));
    return false;
  }
  auto* dev = static_cast<VulkanDevice*>(g.device.get());
  pd::native_vk::Renderer r;
  r.instance = static_cast<VulkanInterface*>(g.api.get())->instance;
  r.physical_device = dev->physicalDevice;
  r.device = dev->vk;
  r.get_instance_proc_addr = reinterpret_cast<void*>(vkGetInstanceProcAddr);
  r.queue_family = static_cast<VulkanCommandQueue*>(g.queue.get())->familyIndex;
  if (!pd::native_vk::Setup(r, presenter, g_scene_w, g_scene_h, spirv, sizeof(OutputConstants))) return false;
  g.out.presenter = presenter;
  g.out.vk = true;
  return true;
}

// The frame Swap was given into the next shared frame (recorded at the end of
// the frame's command list); returns it, or UINT32_MAX when there is no picture.
uint32_t copy_to_presenter_vk() {
  const auto front = g.resolved.find(g.front_buffer);
  if (front == g.resolved.end() || !front->second.texture) {
    static uint32_t missing = 0;
    if ((missing++ % 60) == 0)
      REXKRNL_INFO("PDNATIVE no resolved image for front buffer {:08X} ({} frames so far)", g.front_buffer, missing);
    return UINT32_MAX;
  }
  const uint32_t k = pd::native_vk::NextFrame();
  RenderTexture* src = front->second.texture.get();
  g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE));
  pd::native_vk::RecordCopy(static_cast<VulkanCommandList*>(g.cmd)->vk, static_cast<VulkanTexture*>(src)->vk, k);
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(src, RenderTextureLayout::SHADER_READ));
  prof_mark("present copy");
  return k;
}

// Hands shared frame k to the presenter on pd_native_vk_out's worker, once
// this frame's GPU work is done (the worker waits for the slot's fence without
// resetting it: the game thread waits for and resets it before reusing the
// slot, after WaitPresent). The gamma ramp and Vivid Colours / Black Level as
// in present_to_presenter. No vblank is reported: Perfect Dark's Xenos vsync
// worker does that, and presentation isn't paced to it by default.
void present_to_presenter_vk(uint32_t k) {
  OutputConstants c;
  output_constants(c);
  if (!g.gamma_ramp_version) {  // never set by the game: D3D's identity ramp (see update_gamma_ramp)
    for (uint32_t i = 0; i < 256; ++i) g.gamma_ramp[i] = g.gamma_ramp[256 + i] = g.gamma_ramp[512 + i] = uint16_t(i * 257);
    g.gamma_ramp_version = 1;
  }
  static uint16_t ramp[768];
  static uint32_t ramp_version = 0;
  if (ramp_version != g.gamma_ramp_version) {
    for (int i = 0; i < 768; ++i) ramp[i] = for_display(g.gamma_ramp[i]);
    ramp_version = g.gamma_ramp_version;
  }
  c.mode |= 8;
  VkDevice dev = static_cast<VulkanDevice*>(g.device.get())->vk;
  VkFence fence = static_cast<VulkanCommandFence*>(g.fence)->vk;
  g.out.vk_handoff[g.slot] = pd::native_vk::PresentAsync(
      k, &c, ramp, ramp_version, [dev, fence] { vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); }, nullptr);
}

bool ensure_device() {
  if (g.out.presenter) return true;
  if (g.failed) return false;
  // Every failure below leaves the Xenos renderer running: hung mode (Swap)
  // is only set once this device is up.
  auto fail = [](const char* what) {
    g.failed = true;
    REXKRNL_ERROR("PDNATIVE {}: staying on the Xenos renderer", what);
    return false;
  };
  // The API is the presenter's (OnPreSetup started it on the one
  // gpu_native_api asks for): frames are shared with its device.
  g_vulkan = pd::native_vk::PresenterIsVulkan(sdk_presenter());
  if (!g_vulkan && !sdk_provider())
    return fail("the SDK presenter is neither Direct3D 12 nor Vulkan (gpu_native needs gpu = d3d12 or vulkan)");
  pd::native_shaders::SetSpirv(g_vulkan);
  if (g_vulkan) {
    g.api = CreateVulkanInterface();
    if (g.api) {
      // On the presenter's GPU, so frames can be shared with it (Plume wants
      // the device's whole name).
      std::string want = pd::native_vk::PresenterDeviceName(sdk_presenter());
      for (const std::string& name : g.api->getDeviceNames())
        if (!want.empty() && name.find(want) != std::string::npos) { want = name; break; }
      g.device = g.api->createDevice(want);
    }
    if (!g.device) return fail("could not create a Vulkan device");
    REXKRNL_INFO("PDNATIVE Vulkan device: {}", g.device->getDescription().name);
  } else {
    if (REXCVAR_GET(gpu_native_debug_layer)) {
      ID3D12Debug* dbg = nullptr;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); }
    }
    g.api = CreateD3D12Interface();
    if (g.api) g.device = g.api->createDevice(sdk_adapter_name());
    if (!g.device) return fail("could not create a D3D12 device");
  }
  g.queue = g.device->createCommandQueue(RenderCommandListType::DIRECT);
  g_scene_scale = uint32_t(std::clamp(pd::ActiveRenderScale(), 1, 3));
  {
    rex::system::X_VIDEO_MODE mode{};
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const bool hd1080 = uint32_t(mode.display_width) == 1920 && uint32_t(mode.display_height) == 1080;
    kGuestW = hd1080 ? 1920 : 1280;
    kGuestH = hd1080 ? 1080 : 720;
  }
  g_scene_w = kGuestW * g_scene_scale;
  g_scene_h = kGuestH * g_scene_scale;
  REXKRNL_INFO("PDNATIVE scene {}x{} (game frame {}x{}, render scale {}x)", g_scene_w, g_scene_h, kGuestW,
               kGuestH, g_scene_scale);
  if (g_vulkan ? !setup_presenter_output_vk() : !setup_presenter_output()) {
    g.out = {};
    g.queue.reset();
    g.device.reset();
    g.api.reset();
    return fail("could not share frames with the SDK presenter");
  }
  for (uint32_t i = 0; i < kFrames; ++i) {
    g.cmds[i] = g.queue->createCommandList();
    g.fences[i] = g.device->createCommandFence();
  }
  g.cmd = g.cmds[0].get();
  g.fence = g.fences[0].get();

  // Binding model expected by XenosRecomp's shader_common.h: bindless
  // Texture2D/3D/Cube arrays in spaces 0-2, samplers in space 3, and the
  // vertex / pixel / shared constant buffers as root CBVs b0-b2 in space 4.
  RenderPipelineLayoutBuilder lb;
  lb.begin(false, true);
  RenderDescriptorSetBuilder tb;
  tb.begin();
  tb.addTexture(0, kTextureSlots);
  tb.end(true, kTextureSlots);
  g.textures = tb.create(g.device.get());
  lb.addDescriptorSet(tb);
  lb.addDescriptorSet(tb);
  lb.addDescriptorSet(tb);
  RenderDescriptorSetBuilder sb;
  sb.begin();
  sb.addSampler(0, kSamplerSlots);
  sb.end(true, kSamplerSlots);
  g.samplers = sb.create(g.device.get());
  lb.addDescriptorSet(sb);
  if (g_vulkan) {
    // XenosRecomp's Vulkan shaders read their constants through three buffer
    // addresses in push constants (vertex, pixel, shared).
    lb.addPushConstant(0, 0, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  } else {
    lb.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
    lb.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
    lb.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  }
  lb.end();
  g.layout = lb.create(g.device.get());

  RenderSamplerDesc sd;
  sd.minFilter = RenderFilter::LINEAR;
  sd.magFilter = RenderFilter::LINEAR;
  sd.mipmapMode = RenderMipmapMode::LINEAR;
  sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::CLAMP;
  g.default_sampler = g.device->createSampler(sd);
  for (uint32_t i = 0; i < 16; ++i) g.samplers->setSampler(i, g.default_sampler.get());

  g.timestamps = g.device->createQueryPool(3 * kFrames);  // per slot: start, end; then constants-copy starts
  g.vertex_pool.flags = RenderBufferFlag::VERTEX;
  g.index_pool.flags = RenderBufferFlag::INDEX;
  g.index_pool.chunk = 4ull << 20;  // Dam's index lists come to ~1 MB
  g.prof_pool = g.device->createQueryPool(4096);
  g.upload = g.device->createBuffer(RenderBufferDesc::UploadBuffer(kUploadSize));
  g.const_upload = g.device->createBuffer(RenderBufferDesc::UploadBuffer(kSlotConsts * kFrames));
  g.const_ptr = static_cast<uint8_t*>(g.const_upload->map());
  for (uint32_t i = 0; i < kFrames; ++i) {
    g.const_gpu[i] = g.device->createBuffer(RenderBufferDesc::DefaultBuffer(
        kSlotConsts, RenderBufferFlag::CONSTANT | (g_vulkan ? RenderBufferFlag::DEVICE_ADDRESSABLE : 0)));
    g.const_lists[i] = g.queue->createCommandList();
  }
  g.upload_ptr = static_cast<uint8_t*>(g.upload->map());

  // The game's scene target is 4x multisampled (RB_SURFACE_INFO, mirrored at
  // device+10940, has msaa_samples = 2): the scene and its depth match, and
  // resolves / present average the samples.
  g.msaa.sampleCount = pick_msaa();
  create_scene_targets();

  // Slot 0 of the texture heap: 1x1 white, sampled by anything not yet supported.
  g.white = g.device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  {
    const uint64_t off = upload_alloc(256, 512);
    std::memset(g.upload_ptr + off, 0xFF, 4);
    g.cmd->begin();
    g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g.white.get(), RenderTextureLayout::COPY_DEST));
    g.cmd->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(g.white.get()),
        RenderTextureCopyLocation::PlacedFootprint(g.upload.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64, off));
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g.white.get(), RenderTextureLayout::SHADER_READ));
    g.cmd->end();
    const RenderCommandList* l = g.cmd;
    g.queue->executeCommandLists(&l, 1, nullptr, 0, nullptr, 0, g.fence);
    g.queue->waitForCommandFence(g.fence);
    g.upload_used = 0;
  }
  for (uint32_t i = 0; i < kTextureSlots; ++i) g.textures->setTexture(i, g.white.get(), RenderTextureLayout::SHADER_READ);
  {
    g.white_cube = g.device->createTexture(RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
                                                                      RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::CUBE));
    RenderTextureViewDesc vd = RenderTextureViewDesc::TextureCube(RenderFormat::R8G8B8A8_UNORM);
    vd.arraySize = 6;
    vd.mipLevels = 1;
    g.white_cube_view = g.white_cube->createTextureView(vd);
    const uint64_t off = upload_alloc(256, 512);
    std::memset(g.upload_ptr + off, 0xFF, 4);
    g.cmd->begin();
    g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g.white_cube.get(), RenderTextureLayout::COPY_DEST));
    for (uint32_t face = 0; face < 6; ++face)
      g.cmd->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(g.white_cube.get(), 0, face),
          RenderTextureCopyLocation::PlacedFootprint(g.upload.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64, off));
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g.white_cube.get(), RenderTextureLayout::SHADER_READ));
    g.cmd->end();
    const RenderCommandList* l = g.cmd;
    g.queue->executeCommandLists(&l, 1, nullptr, 0, nullptr, 0, g.fence);
    g.queue->waitForCommandFence(g.fence);
    g.upload_used = 0;
    g.white_cube_slot = alloc_texture_slot();
    g.textures->setTexture(g.white_cube_slot, g.white_cube.get(), RenderTextureLayout::SHADER_READ, g.white_cube_view.get());
  }

  REXKRNL_INFO("PDNATIVE {} device up, frames go to the SDK presenter", g_vulkan ? "Vulkan" : "D3D12");
  std::thread(warm_pipelines).detach();
  return true;
}


// The scene's sample count from gpu_native_msaa (1, 2 or 4), as far as the GPU
// supports it for both the colour and depth formats.
RenderSampleCounts pick_msaa() {
  const int32_t want = REXCVAR_GET(gpu_native_msaa);
  const RenderSampleCounts supported = g.device->getSampleCountsSupported(kSceneFormat) &
                                       g.device->getSampleCountsSupported(RenderFormat::D32_FLOAT);
  for (RenderSampleCounts c : {RenderSampleCount::COUNT_4, RenderSampleCount::COUNT_2})
    if (want >= int32_t(c) && (supported & c)) return c;
  return RenderSampleCount::COUNT_1;
}

// The scene colour and depth targets at the current size and sample count.
void create_scene_targets() {
  g.scene = g.device->createTexture(RenderTextureDesc::ColorTarget(g_scene_w, g_scene_h, kSceneFormat, g.msaa));
  // Typeless so the depth resolve can read it (D32_FLOAT depth view, R32_FLOAT shader view).
  // (Vulkan samples a D32 depth image directly.)
  g.scene_depth = g.device->createTexture(RenderTextureDesc::DepthTarget(
      g_scene_w, g_scene_h, g_vulkan ? RenderFormat::D32_FLOAT : RenderFormat::R32_TYPELESS, g.msaa));
  const RenderTexture* sc = g.scene.get();
  g.scene_fb = g.device->createFramebuffer(RenderFramebufferDesc(&sc, 1, g.scene_depth.get()));
}

// --- shaders ----------------------------------------------------------------

// Rebuilds the contiguous container (virtual part + physical part) exactly as
// the probe saved it, so the hash matches the translated cache.
bool container_at(uint32_t container, uint32_t physical, std::vector<uint8_t>& out) {
  const uint32_t flags = g32(container), vsize = g32(container + 4), psize = g32(container + 8);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || vsize < 36 || vsize > 65536 || psize > 65536) return false;
  out.resize(vsize + psize);
  std::memcpy(out.data(), gmem() + container, vsize);
  std::memcpy(out.data() + vsize, gmem() + (physical ? physical : container + vsize), psize);
  return true;
}

// The blob a shader was created from (it may since have been freed: then the
// copy kept in the object, with its microcode at the object's GPU address).
bool container_bytes(uint32_t object, bool vertex, std::vector<uint8_t>& out) {
  const auto created = g.created_blobs.find(object);
  if (created != g.created_blobs.end() && container_at(created->second, 0, out)) return true;
  return container_at(object + (vertex ? 872 : 40), gpu_view(g32(object + (vertex ? 32 : 24))), out);
}


// Vertex shaders registered with XGRegisterVertexShader carry their own vertex
// fetch: each vertex element (usage, index, microcode address) points at a
// vfetch instruction giving the fetch constant slot, offset, stride and format
// (layout as XenosRecomp's VertexFetchInstruction). Mini fetches reuse the
// previous full fetch's slot and stride.
// live_micro: guest address of the microcode the GPU runs (D3D patches the
// vfetch format/stride/offset there when a declaration is bound; boot shaders'
// original blobs keep zeroed templates). 0 = use the container's own copy.
void parse_vertex_inputs(const std::vector<uint8_t>& blob, HostShader& hs, uint32_t live_micro = 0) {
  auto be = [&](size_t o) { return o + 4 <= blob.size() ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(&blob[o])) : 0u; };
  const uint32_t vsize = be(4), shader = be(24);
  const uint32_t phys_off = be(shader), field18 = be(shader + 24), count = be(shader + 28);
  const size_t micro = size_t(vsize) + phys_off;
  auto mc = [&](size_t o) { return live_micro ? g32(live_micro + uint32_t(o - micro)) : be(o); };
  hs.inputs.clear();
  hs.strides.clear();
  uint16_t last_slot = 0;
  uint32_t last_stride = 0;
  for (uint32_t i = 0; i < count && i < 32; ++i) {
    const uint32_t e = be(shader + 36 + (field18 + i) * 4);
    const uint32_t address = e & 0xFFF, usage = (e >> 12) & 0xF, index = (e >> 16) & 0xF;
    const size_t at = micro + size_t(address) * 12;
    const uint32_t d0 = mc(at), d1 = mc(at + 4), d2 = mc(at + 8);
    if ((d0 & 0x1F) != 0) continue;  // not a vertex fetch
    const bool mini = (d1 >> 30) & 1;
    if (!mini) {
      last_slot = uint16_t(((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3));
      last_stride = (d2 & 0xFF) * 4;
    }
    const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
    // d1: bit 12 signed (format_comp_all), bit 13 integer (num_format_all);
    // bit 14 is only the signed rounding mode.
    const uint32_t type = ((d1 >> 16) & 0x3F) | (((d1 >> 12) & 1) << 8) | (((d1 >> 13) & 1) << 9);
    const int8_t exp = int8_t(int32_t((d1 >> 24) & 0x3F) << 26 >> 26);
    const uint16_t templ = uint16_t(be(at + 4) & 0xFFF), live = uint16_t(d1 & 0xFFF);
    hs.inputs.push_back({last_slot, uint16_t(offset), type, uint8_t(usage), uint8_t(index), exp,
                         (templ == kIdentitySwizzle || templ == 0) && live ? live : kIdentitySwizzle});
    hs.strides[last_slot] = last_stride;
  }
}

// <hash>.vs|ps.final.dxil (boot shaders translated locally may lack the kind)
// and its .cbsize from the shader cache.
RenderShader* load_cached_shader(HostShader& hs, bool vertex) {
  const std::wstring& dir = pd::native_shaders::CacheDir();
  wchar_t name[64];
  for (const wchar_t* kind : {vertex ? L"vs." : L"ps.", L""}) {
    swprintf(name, 64, L"\\%016llX.%s%s", static_cast<unsigned long long>(hs.hash), kind,
             pd::native_shaders::FinalExtension());
    FILE* f = _wfopen((dir + name).c_str(), L"rb");
    if (!f) continue;
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> dxil(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    std::fread(dxil.data(), 1, dxil.size(), f);
    std::fclose(f);
    hs.shader = g.device->createShader(dxil.data(), dxil.size(), "main",
                                       g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL);
    if (!hs.shader) continue;
    hs.cb_bytes = vertex ? 4096 : 3584;
    std::wstring cb = dir + name;
    cb.replace(cb.rfind(L"final."), std::wstring(pd::native_shaders::FinalExtension()).size(), L"cbsize");
    if (FILE* c = _wfopen(cb.c_str(), L"rb")) {
      unsigned v = 0;
      if (std::fscanf(c, "%u", &v) == 1) hs.cb_bytes = std::min<uint32_t>(v, hs.cb_bytes);
      std::fclose(c);
    }
    return hs.shader.get();
  }
  return nullptr;
}

// Level shaders are registered in place while the level loads, and their
// microcode streams in a little later. Once an object's bytes read the same
// two frames running, its translation starts (pd::native_shaders::Request does
// nothing for one already cached), so a first run translates a level's shaders
// during its loading instead of at their first draw. A shader drawn before
// that is translated then, as before; the hash is the same either way.
void warm_registered_shaders() {
  for (auto it = g.warm.begin(); it != g.warm.end();) {
    auto& [object, w] = *it;
    std::vector<uint8_t> blob;
    if (g.frames - w.frame > 600 || !container_bytes(object, w.vertex, blob)) {
      if (g.frames - w.frame > 600) it = g.warm.erase(it); else ++it;
      continue;
    }
    // Microcode not streamed in yet: all zeros past the container's header.
    const uint32_t vsize = (uint32_t(blob[4]) << 24) | (uint32_t(blob[5]) << 16) | (uint32_t(blob[6]) << 8) | blob[7];
    bool filled = false;
    for (size_t i = vsize; i < blob.size() && !filled; ++i) filled = blob[i] != 0;
    const uint64_t hash = fnv1a(blob.data(), blob.size());
    if (!filled || hash != w.last_hash) {
      w.last_hash = hash;
      ++it;
      continue;
    }
    pd::native_shaders::Request(hash, std::move(blob));
    ++g.warm_requested;
    it = g.warm.erase(it);
  }
}

// A shader the game just created: start translating it if it isn't cached,
// so it is usually ready by the time a draw needs it.
void request_translation(uint32_t object, bool vertex) {
  std::vector<uint8_t> blob;
  if (!container_bytes(object, vertex, blob)) return;
  const uint64_t hash = fnv1a(blob.data(), blob.size());  // before the move: argument order is unspecified
  pd::native_shaders::Request(hash, std::move(blob));
}

RenderShader* host_shader(uint32_t object, bool vertex) {
  if (!object) return nullptr;
  HostShader& hs = g.shaders[object];
  if (hs.shader) return hs.shader.get();
  if (hs.missing) return nullptr;
  std::vector<uint8_t> blob;
  // Not cached as missing: the post-process PS is bound on frame 1 before its
  // container is filled in, and becomes valid a few frames later.
  if (!container_bytes(object, vertex, blob)) return nullptr;
  hs.hash = fnv1a(blob.data(), blob.size());
  if (vertex) { parse_vertex_inputs(blob, hs); hs.container = blob; }
  // From the cache (native_shaders); a shader not there yet is translated now
  // (usually already started when the game created it, see request_translation)
  // and this draw waits for it, so nothing is drawn with pieces missing.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (RenderShader* shader = load_cached_shader(hs, vertex)) return shader;
    if (attempt == 0) {
      pd::native_shaders::Request(hs.hash, blob);
      if (!pd::native_shaders::Wait(hs.hash)) break;
    }
  }
  hs.missing = true;
  if (!REXCVAR_GET(gpu_native_translate)) {
    // Leave the container for the local translation step (native/translate_shaders.ps1).
    const std::wstring missing = pd::native_shaders::CacheDir() + L"\\missing";
    CreateDirectoryW(missing.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\%016llX.%s.bin", static_cast<unsigned long long>(hs.hash), vertex ? L"vs" : L"ps");
    if (FILE* f = _wfopen((missing + name).c_str(), L"wb")) {
      std::fwrite(blob.data(), 1, blob.size(), f);
      std::fclose(f);
    }
  }
  REXKRNL_WARN("PDNATIVE no shader {:016X} ({}) for object {:08X} ({} bytes)", hs.hash, vertex ? "vs" : "ps", object,
               blob.size());
  return nullptr;
}

// --- vertex input -----------------------------------------------------------

const char* kSemantics[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
                            "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH",
                            "SAMPLE"};


// D3DDECLTYPE on Xbox 360: low 6 bits = Xenos vertex format, bit 8 = signed,
// bit 9 = integer (not normalised); the rest is the component swizzle, which
// the vertex fetch in the microcode applies itself.
RenderFormat element_format(uint32_t type, uint32_t& component_bytes) {
  const uint32_t fmt = type & 0x3F;
  const bool sign = (type >> 8) & 1, integer = (type >> 9) & 1;
  component_bytes = 4;
  switch (fmt) {
    case 6: component_bytes = 1;
      return integer ? (sign ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT)
                     : (sign ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM);
    case 25: component_bytes = 2;
      return integer ? (sign ? RenderFormat::R16G16_SINT : RenderFormat::R16G16_UINT)
                     : (sign ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM);
    case 26: component_bytes = 2;
      return integer ? (sign ? RenderFormat::R16G16B16A16_SINT : RenderFormat::R16G16B16A16_UINT)
                     : (sign ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM);
    case 31: component_bytes = 2; return RenderFormat::R16G16_FLOAT;
    case 32: component_bytes = 2; return RenderFormat::R16G16B16A16_FLOAT;
    case 36: return RenderFormat::R32_FLOAT;
    case 37: return RenderFormat::R32G32_FLOAT;
    case 38: return RenderFormat::R32G32B32A32_FLOAT;
    case 57: return RenderFormat::R32G32B32_FLOAT;
    case 7: return RenderFormat::UNKNOWN;  // 2:10:10:10 - Plume has no such format yet
    default: return RenderFormat::UNKNOWN;
  }
}

std::vector<Element> read_decl(uint32_t decl) {
  std::vector<Element> els;
  if (!decl) return els;
  const uint32_t n = std::min<uint32_t>(g32(decl + 24), 32);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t e = decl + 52 + i * 12;
    Element el{g16(e), g16(e + 2), g32(e + 4), gmem()[e + 9], gmem()[e + 10]};
    if (el.stream == 0xFF) break;
    // D3DDECLTYPE carries the component swizzle in bits 10-21 (3 bits each:
    // FLOAT3 = x,y,z,1; FLOAT2 = x,y,0,1; D3DCOLOR = z,y,x,w), which D3D writes
    // into the vfetch it patches; decoded here instead.
    if (const uint16_t sw = uint16_t((el.type >> 10) & 0xFFF)) el.swizzle = sw;
    els.push_back(el);
  }
  return els;
}

// Vertex decoding, done on the CPU the way the Xenos vertex fetch does it: read
// the dwords with the fetch constant's endian mode (0 none, 1 8in16, 2 8in32,
// 3 16in32), take components from the low bits up, then normalise (or not,
// for integer formats), apply the sign and scale by 2^exp. Every element is
// written as 16 bytes: float4, or for XenosRecomp's uint4 inputs (BLENDINDICES,
// NORMAL, TANGENT, BINORMAL) the raw bits (float formats as asuint, packed
// 11:11:10 normals as the dword in .x with spec bit 0).
struct Source {
  uint32_t addr = 0, stride = 0, endian = 2;
};

// 16-bit pairs. Vertex buffers follow the fetch constant's swap as is, but the
// game's own memory passed to DrawVerticesUP / Begin+EndVertices has the first
// value as X (the menu font's U and V came out crossed): those sources carry
// endian bit 2, which swaps the halves back.
inline uint32_t fetch_pair(uint32_t addr, uint32_t endian);

inline uint32_t fetch_dword(uint32_t addr, uint32_t endian) {
  uint32_t w;
  std::memcpy(&w, gmem() + addr, 4);
  switch (endian & 3) {
    case 1: return ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(w);
    case 3: return (w << 16) | (w >> 16);
    default: return w;
  }
}

inline bool uint_usage(uint8_t usage) { return usage == 2 || usage == 3 || usage == 6 || usage == 7; }

float half_to_float(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  uint32_t bits;
  if (e == 0) {
    if (m == 0) bits = sign;
    else { float f = std::ldexp(float(m), -24); std::memcpy(&bits, &f, 4); bits |= sign; }
  } else if (e == 31) bits = sign | 0x7F800000u | (m << 13);
  else bits = sign | ((e + 112) << 23) | (m << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

inline uint32_t fetch_pair(uint32_t addr, uint32_t endian) {
  const uint32_t w = fetch_dword(addr, endian);
  return (endian & 4) ? (w << 16) | (w >> 16) : w;
}

// Host vertex format per element: as small as the source allows (a vertex
// used to be 16 bytes per element: ~100 MB of cached vertices in Dam).
enum class HostKind : uint8_t { kRecord, kF32, kU32, kU8x4, kU16x2, kU16x4, kRaw };
struct HostFmt {
  RenderFormat format;
  uint32_t size;
  HostKind kind;
};

HostFmt host_format(const Element& el) {
  const uint32_t fmt = el.type & 0x3F;
  const bool sign = (el.type >> 8) & 1, integer = (el.type >> 9) & 1, plain = !integer && el.exp == 0;
  if (uint_usage(el.usage)) {
    if ((fmt == 16 || fmt == 17) && el.usage != 2) return {RenderFormat::R32_UINT, 4, HostKind::kU32};  // shader reads .x
    if (el.usage == 2 && fmt == 6) return {RenderFormat::R8G8B8A8_UINT, 4, HostKind::kU8x4};
    return {RenderFormat::R32G32B32A32_UINT, 16, HostKind::kRecord};
  }
  switch (fmt) {
    case 57: return {RenderFormat::R32G32B32_FLOAT, 12, HostKind::kF32};
    case 37: return {RenderFormat::R32G32_FLOAT, 8, HostKind::kF32};
    case 36: return {RenderFormat::R32_FLOAT, 4, HostKind::kF32};
    case 6:
      if (plain) return {sign ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM, 4, HostKind::kU8x4};
      break;
    case 25:
      if (plain) return {sign ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM, 4, HostKind::kU16x2};
      return {RenderFormat::R32G32_FLOAT, 8, HostKind::kF32};
    case 26:
      if (plain) return {sign ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM, 8, HostKind::kU16x4};
      break;
    case 31: return {RenderFormat::R16G16_FLOAT, 4, HostKind::kRaw};
    case 32: return {RenderFormat::R16G16B16A16_FLOAT, 8, HostKind::kRaw};
    default: break;
  }
  return {RenderFormat::R32G32B32A32_FLOAT, 16, HostKind::kRecord};
}

void decode_element(const Element& el, uint32_t addr, uint32_t endian, uint8_t* out, const HostFmt& hf) {
  const uint32_t fmt = el.type & 0x3F;
  const bool sign = (el.type >> 8) & 1, integer = (el.type >> 9) & 1;
  const float scale = std::ldexp(1.0f, el.exp);
  float f[4] = {0, 0, 0, 1};
  uint32_t u[4] = {0, 0, 0, 1};
  bool is_float = false;
  auto fixed = [&](uint32_t raw, int bits, int c) {
    int64_t v = raw;
    if (sign && ((raw >> (bits - 1)) & 1)) v -= int64_t(1) << bits;
    u[c] = uint32_t(v);
    float x = float(v);
    if (!integer) x = sign ? std::max(x / float((1 << (bits - 1)) - 1), -1.0f) : x / float((int64_t(1) << bits) - 1);
    f[c] = x * scale;
  };
  switch (fmt) {
    case 6: { const uint32_t w = fetch_dword(addr, endian); for (int c = 0; c < 4; ++c) fixed((w >> (8 * c)) & 0xFF, 8, c); break; }
    case 7: { const uint32_t w = fetch_dword(addr, endian);
      fixed(w & 0x3FF, 10, 0); fixed((w >> 10) & 0x3FF, 10, 1); fixed((w >> 20) & 0x3FF, 10, 2); fixed(w >> 30, 2, 3); break; }
    case 16: case 17: { const uint32_t w = fetch_dword(addr, endian);
      if (uint_usage(el.usage)) { u[0] = w; u[1] = u[2] = u[3] = 0; break; }
      fixed(w & 0x7FF, 11, 0); fixed((w >> 11) & 0x7FF, 11, 1); fixed(w >> 22, 10, 2); break; }
    case 25: { const uint32_t w = fetch_pair(addr, endian); fixed(w & 0xFFFF, 16, 0); fixed(w >> 16, 16, 1); break; }
    case 26: { for (int d = 0; d < 2; ++d) { const uint32_t w = fetch_pair(addr + 4 * d, endian);
        fixed(w & 0xFFFF, 16, 2 * d); fixed(w >> 16, 16, 2 * d + 1); } break; }
    case 31: { const uint32_t w = fetch_pair(addr, endian); f[0] = half_to_float(w & 0xFFFF); f[1] = half_to_float(w >> 16);
      u[0] = w; is_float = true; break; }
    case 32: { for (int d = 0; d < 2; ++d) { const uint32_t w = fetch_pair(addr + 4 * d, endian);
        f[2 * d] = half_to_float(w & 0xFFFF); f[2 * d + 1] = half_to_float(w >> 16); u[d] = w; } is_float = true; break; }
    case 36: case 37: case 38: case 57: {
      const int n = fmt == 36 ? 1 : fmt == 37 ? 2 : fmt == 57 ? 3 : 4;
      for (int c = 0; c < n; ++c) { const uint32_t w = fetch_dword(addr + 4 * c, endian); std::memcpy(&f[c], &w, 4); }
      is_float = true; break; }
    default: {
      static uint64_t logged = 0;
      if (fmt < 64 && !(logged & (1ull << fmt))) {
        logged |= 1ull << fmt;
        REXKRNL_WARN("PDNATIVE vertex format {} (type {:08X}, usage {}) not decoded yet", fmt, el.type, el.usage);
      }
      break;
    }
  }
  if (el.swizzle != kIdentitySwizzle) {  // the live vfetch's dst swizzle: 0-3 = x..w, 4 = 0, 5 = 1
    const float sf[4] = {f[0], f[1], f[2], f[3]};
    const uint32_t su[4] = {u[0], u[1], u[2], u[3]};
    for (int c = 0; c < 4; ++c) {
      const uint32_t sel = (el.swizzle >> (3 * c)) & 7;
      if (sel < 4) { f[c] = sf[sel]; u[c] = su[sel]; }
      else if (sel == 4) { f[c] = 0.0f; u[c] = 0; }
      else if (sel == 5) { f[c] = 1.0f; u[c] = 1; }
    }
  }
  switch (hf.kind) {
    case HostKind::kF32: std::memcpy(out, f, hf.size); return;
    case HostKind::kU32: std::memcpy(out, &u[0], 4); return;
    case HostKind::kRaw: std::memcpy(out, u, hf.size); return;
    case HostKind::kU8x4: for (int c = 0; c < 4; ++c) out[c] = uint8_t(u[c]); return;
    case HostKind::kU16x2:
    case HostKind::kU16x4:
      for (uint32_t c = 0; c < hf.size / 2; ++c) { const uint16_t v = uint16_t(u[c]); std::memcpy(out + 2 * c, &v, 2); }
      return;
    case HostKind::kRecord: break;
  }
  if (uint_usage(el.usage)) {
    // Normals, tangents and binormals reach the shader through
    // tfetchR11G11B10, which unpacks 11_11_10 bits (spec bit 0) and otherwise
    // reinterprets the input as floats: every other format goes over as float
    // bits (the Rare logo's 2_10_10_10 normals were arriving as raw integers).
    // Blend indices are used as numbers and stay integers.
    // Perfect Dark's blend indices are floats (FLOAT2): the shader converts
    // the uint4 input by value, so they go over as the numbers they hold.
    const bool packed = fmt == 16 || fmt == 17;
    if (el.usage == 2 && is_float)
      for (int c = 0; c < 4; ++c) u[c] = uint32_t(std::max(0.0f, std::round(f[c])));
    else if (is_float || (el.usage != 2 && !packed))
      for (int c = 0; c < 4; ++c) std::memcpy(&u[c], &f[c], 4);
    std::memcpy(out, u, 16);
  } else {
    std::memcpy(out, f, 16);
  }
}

// Physical address -> a guest address the CPU can read. Physical memory shows
// up through several aliases and not every page is mapped in every one: the
// 0xA0000000 view, the 0xE0000000 view (whose CPU addresses are 4 KB behind
// the physical address), and 0xC0000000. Pick the first whose first and last
// bytes of [phys, phys+size) can be read.
bool readable_seh(uint32_t a, uint32_t b) {
  __try {
    volatile uint8_t x = gmem()[a];
    volatile uint8_t y = gmem()[b];
    (void)x; (void)y;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

uint32_t cpu_view_of_physical(uint32_t phys, uint32_t size) {
  if (!phys) return 0;
  const uint32_t last = size ? size - 1 : 0;
  for (uint32_t base : {0xA0000000u + phys, 0xE0000000u + phys - 0x1000u, 0xC0000000u + phys})
    if (readable_seh(base, base + last)) return base;
  return 0;
}

// --- guest textures -----------------------------------------------------------
//
// A texture's 24-byte fetch constant sits at +28 in its D3D object (the same
// bytes SetTexture copies into the device). Decoded fields (Xenos layout):
//   d0: bits 10-18 clamp x/y/z, 22-30 pitch / 32 texels, 31 tiled
//   d1: bits 0-5 format, 6-7 endian, 12-31 base address / 4 KB (physical)
//   d2: (2D) bits 0-12 width-1, 13-25 height-1
//   d3: bits 1-12 swizzle (3 bits per component: 0-3 = x,y,z,w, 4 = 0, 5 = 1),
//       19-20 mag filter, 21-22 min filter
//   d5: bits 9-10 dimension (1 = 2D)
// Only the base level of 2D textures is uploaded for now (no mips), converted to
// RGBA8 on the CPU with the swizzle applied, since the translated shaders
// leave the fetch-constant swizzle to the hardware.

// XGAddress2DTiledOffset: texel (block) offset of (x, y) in a tiled surface.
uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb) {
  const uint32_t aligned = (pitch + 31) & ~31u;
  const uint32_t log_bpp = (bpb >> 2) + ((bpb >> 1) >> (bpb >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp;
}

struct TexFormat {
  uint32_t bpb, block;  // bytes per block, block size in texels (1 or 4)
};
bool tex_format(uint32_t fmt, TexFormat& f) {
  switch (fmt) {
    case 2: f = {1, 1}; return true;                       // k_8
    case 3: case 4: case 10: case 15: f = {2, 1}; return true;  // 1555, 565, 8_8, 4444
    case 6: f = {4, 1}; return true;                       // 8888
    case 18: f = {8, 4}; return true;                      // DXT1
    case 19: case 20: f = {16, 4}; return true;            // DXT2/3, DXT4/5
    default: return false;
  }
}

inline uint32_t swap_by_endian(uint32_t w, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(w);
    case 3: return (w << 16) | (w >> 16);
    default: return w;
  }
}

void rgb565(uint16_t c, uint8_t* o) {
  o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  o[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes one block (already endian-swapped to little-endian) into RGBA texels
// laid out as x,y,z,w components (Xenos component order: x = lowest bits).
void decode_block(uint32_t fmt, const uint8_t* b, uint8_t out[16][4]) {
  switch (fmt) {
    case 2: out[0][0] = out[0][1] = out[0][2] = out[0][3] = b[0]; break;
    case 10: out[0][0] = b[0]; out[0][1] = b[1]; out[0][2] = 0; out[0][3] = 255; break;
    case 4: { uint16_t c = uint16_t(b[0] | b[1] << 8); out[0][2] = uint8_t((c & 31) * 255 / 31);
      out[0][1] = uint8_t(((c >> 5) & 63) * 255 / 63); out[0][0] = uint8_t(((c >> 11) & 31) * 255 / 31); out[0][3] = 255; break; }
    case 3: { uint16_t c = uint16_t(b[0] | b[1] << 8); out[0][0] = uint8_t((c & 31) * 255 / 31);
      out[0][1] = uint8_t(((c >> 5) & 31) * 255 / 31); out[0][2] = uint8_t(((c >> 10) & 31) * 255 / 31); out[0][3] = (c >> 15) ? 255 : 0; break; }
    case 15: { uint16_t c = uint16_t(b[0] | b[1] << 8);
      for (int k = 0; k < 4; ++k) out[0][k] = uint8_t(((c >> (4 * k)) & 15) * 17); break; }
    case 6: for (int k = 0; k < 4; ++k) out[0][k] = b[k]; break;
    case 18: case 19: case 20: {
      const uint8_t* cb = fmt == 18 ? b : b + 8;
      uint8_t pal[4][4];
      const uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
      rgb565(c0, pal[0]); rgb565(c1, pal[1]);
      pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
      if (fmt != 18 || c0 > c1) {
        for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3); }
      } else {
        for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[3][3] = 0;
      }
      const uint32_t bits = uint32_t(cb[4]) | uint32_t(cb[5]) << 8 | uint32_t(cb[6]) << 16 | uint32_t(cb[7]) << 24;
      for (int t = 0; t < 16; ++t) std::memcpy(out[t], pal[(bits >> (2 * t)) & 3], 4);
      if (fmt == 19) {
        for (int t = 0; t < 16; ++t) out[t][3] = uint8_t(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
      } else if (fmt == 20) {
        uint8_t a[8] = {b[0], b[1]};
        if (a[0] > a[1]) for (int k = 2; k < 8; ++k) a[k] = uint8_t(((8 - k) * a[0] + (k - 1) * a[1]) / 7);
        else { for (int k = 2; k < 6; ++k) a[k] = uint8_t(((6 - k) * a[0] + (k - 1) * a[1]) / 5); a[6] = 0; a[7] = 255; }
        uint64_t ab = 0;
        for (int k = 0; k < 6; ++k) ab |= uint64_t(b[2 + k]) << (8 * k);
        for (int t = 0; t < 16; ++t) out[t][3] = a[(ab >> (3 * t)) & 7];
      }
      break;
    }
  }
}

// Decodes a whole 2D level into RGBA8 (with the swizzle); false on a fault.
bool decode_texture_seh(uint32_t addr, uint32_t fmt, uint32_t endian, bool tiled, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t swizzle, uint8_t* out, uint32_t row_bytes, uint32_t ox = 0,
                        uint32_t oy = 0) {
  TexFormat tf;
  if (!tex_format(fmt, tf)) return false;
  const uint32_t bw = (width + tf.block - 1) / tf.block, bh = (height + tf.block - 1) / tf.block;
  const uint32_t pitch_blocks = std::max<uint32_t>(pitch / tf.block, bw);
  __try {
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint32_t idx = tiled ? tiled_offset(bx + ox, by + oy, pitch_blocks, tf.bpb)
                                   : (by + oy) * pitch_blocks + bx + ox;
        uint8_t raw[16];
        std::memcpy(raw, gmem() + addr + idx * tf.bpb, tf.bpb);
        for (uint32_t k = 0; k + 3 < tf.bpb + 3 && k < tf.bpb; k += 4) {
          uint32_t w = 0;
          std::memcpy(&w, raw + k, std::min<uint32_t>(4, tf.bpb - k));
          w = tf.bpb >= 4 ? swap_by_endian(w, endian) : (endian == 1 || endian == 2 ? ((w & 0xFF) << 8 | (w >> 8 & 0xFF)) : w);
          std::memcpy(raw + k, &w, std::min<uint32_t>(4, tf.bpb - k));
        }
        uint8_t texels[16][4];
        decode_block(fmt, raw, texels);
        const uint32_t n = tf.block;
        for (uint32_t ty = 0; ty < n; ++ty)
          for (uint32_t tx = 0; tx < n; ++tx) {
            const uint32_t x = bx * n + tx, y = by * n + ty;
            if (x >= width || y >= height) continue;
            const uint8_t* src = texels[ty * n + tx];
            uint8_t* dst = out + size_t(y) * row_bytes + size_t(x) * 4;
            for (int c = 0; c < 4; ++c) {
              const uint32_t sel = (swizzle >> (3 * c)) & 7;
              dst[c] = sel < 4 ? src[sel] : sel == 4 ? 0 : 255;
            }
          }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Debug: decoded RGBA texture -> <exe>\native_textures\<object>_f<fmt>_<w>x<h>.bmp.
void dump_texture(uint32_t tex, uint32_t fmt, uint32_t width, uint32_t height, const uint8_t* rgba, uint32_t row_bytes) {
  const std::wstring dir = exe_dir() + L"\\native_textures";
  CreateDirectoryW(dir.c_str(), nullptr);
  wchar_t name[96];
  swprintf(name, 96, L"\\%08X_f%u_%ux%u.bmp", tex, fmt, width, height);
  FILE* f = _wfopen((dir + name).c_str(), L"wb");
  if (!f) return;
  const uint32_t image = width * height * 4;
  uint8_t h[54] = {'B', 'M'};
  auto put = [&](int at, uint32_t v) { std::memcpy(h + at, &v, 4); };
  put(2, 54 + image); put(10, 54); put(14, 40); put(18, width); put(22, uint32_t(-int32_t(height)));
  h[26] = 1; h[28] = 32; put(34, image);
  std::fwrite(h, 1, 54, f);
  std::vector<uint8_t> row(width * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + size_t(y) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 4 + 0] = src[x * 4 + 2]; row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + 0]; row[x * 4 + 3] = src[x * 4 + 3];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

// Packed mips (fetch dword 5 bit 11): a base level whose shorter side (rounded
// up to a power of two) is 16 texels or less lies inside the 32x32 mip-tail
// tile, 16 texels across (taller than wide or square) or down (wider), as in
// the SDK's TextureInfo::GetPackedTileOffset. Offsets are in blocks.
void packed_base_offset(const uint32_t* f, uint32_t width, uint32_t height, uint32_t block, uint32_t& ox, uint32_t& oy) {
  ox = oy = 0;
  if (!((f[5] >> 11) & 1)) return;
  const auto log2_ceil = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) ++l; return l; };
  const uint32_t lw = log2_ceil(width), lh = log2_ceil(height);
  if (std::min(lw, lh) > 4) return;
  if (lw > lh) oy = 16 / block; else ox = 16 / block;
}

// Untiles one level of DXT blocks into rows of `row_pitch` bytes, swapping
// each 16-bit word (DXT data is 8in16): the result is a little-endian BCn level.
bool untile_blocks_seh(uint32_t addr, uint32_t endian, bool tiled, uint32_t bw, uint32_t bh, uint32_t pitch_blocks,
                       uint32_t bpb, uint8_t* out, uint32_t row_pitch, uint32_t ox = 0, uint32_t oy = 0) {
  __try {
    for (uint32_t by = 0; by < bh; ++by) {
      uint8_t* row = out + size_t(by) * row_pitch;
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint32_t idx = tiled ? tiled_offset(bx + ox, by + oy, pitch_blocks, bpb) : (by + oy) * pitch_blocks + bx + ox;
        const uint8_t* src = gmem() + addr + size_t(idx) * bpb;
        uint8_t* dst = row + size_t(bx) * bpb;
        if (endian == 1 || endian == 2)
          for (uint32_t k = 0; k < bpb; k += 2) { dst[k] = src[k + 1]; dst[k + 1] = src[k]; }
        else
          std::memcpy(dst, src, bpb);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// A guest texture made ready in ordinary memory, on the game thread or on the
// texture worker: its levels laid out with the row pitch the upload footprint
// needs (256-byte aligned), so committing it is a copy.
struct PreparedTexture {
  bool ok = false;
  RenderFormat format = RenderFormat::R8G8B8A8_UNORM;
  uint32_t width = 0, height = 0;
  uint32_t block_bytes = 4, block = 1;  // footprint row width (texels) = row_pitch / block_bytes * block
  struct Level {
    uint32_t w, h, rows, row_pitch;
    size_t offset;
  };
  std::vector<Level> levels;  // faces x mips, face by face
  std::vector<uint8_t> data;
  uint32_t faces = 1;  // 6: a cube map
};

// DXT1/3/5 textures go to the GPU compressed (BC1/2/3), with the game's own mip
// chain: level 0 at the base address, levels 1.. one after another from the mip
// address, each padded to 32x32-block tiles and 4 KB. From the first level
// with a side of 16 texels or less they're packed together into one tile (the
// mip tail), each at its own offset, as the classic renderer reads them. Sizes
// that aren't powers of two store their mips as levels of the size rounded up
// to powers of two, as the SDK's GetGuestTextureLayout reads them. Linear
// textures have the same levels with rows of whole 32-block groups. All of this
// used to be left out (the chain stopped before the tail, other sizes went to
// the RGBA path with box-filtered mips, and linear textures got no mips at all),
// as in the GoldenEye renderer this one came from, which fixed the first two
// (its issue #17 and commit b2f09b5). Perfect Dark's DXT textures are all
// linear, so until 2026-11-12 every one of them was drawn without mips here:
// distant surfaces sampled their full-size level and shimmered (measured: 2.8%
// more fine detail than classic; with the game's mips, the same as classic and
// every mip of 69 of 72 textures identical to classic's, RenderDoc; the other 3
// had a base level that isn't whole blocks and are now decoded to RGBA with the
// game's mips). The plain swizzle only; returns false to leave the texture to
// the RGBA path.
bool prepare_bc(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  constexpr uint32_t swizzle_identity = 0x688;
  const uint32_t fmt = f[1] & 0x3F, endian = (f[1] >> 6) & 3;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32, swizzle = (f[3] >> 1) & 0xFFF;
  if ((fmt != 18 && fmt != 19 && fmt != 20) || swizzle != swizzle_identity || !REXCVAR_GET(gpu_native_bc_textures))
    return false;
  // The host's BCn base level must be whole blocks: other sizes (Perfect Dark
  // has 512x326 and 512x331) are decoded to RGBA8 here instead, level by level
  // from the game's own mips, which the RGBA path would rebuild with a box filter.
  const bool as_rgba = width % 4 || height % 4;
  // Any size, linear too: mips are stored as levels of the power-of-two size either way.
  const uint32_t bpb = fmt == 18 ? 8 : 16;
  if (as_rgba) {
    p.format = RenderFormat::R8G8B8A8_UNORM;
    p.width = width; p.height = height; p.block_bytes = 4; p.block = 1;
  } else {
    p.format = fmt == 18 ? RenderFormat::BC1_UNORM : fmt == 19 ? RenderFormat::BC2_UNORM : RenderFormat::BC3_UNORM;
    p.width = width; p.height = height; p.block_bytes = bpb; p.block = 4;
  }
  const uint32_t mip_phys = f[5] & 0xFFFFF000u, guest_last = (f[4] >> 6) & 0xF;
  const auto log2_ceil = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) ++l; return l; };
  const auto blocks = [](uint32_t texels) { return std::max(1u, (texels + 3) / 4); };
  // Mips are stored (pitch, tiles, size) as levels of the base size rounded up to powers of two.
  const uint32_t pow2_w = 1u << log2_ceil(width), pow2_h = 1u << log2_ceil(height);
  // The packed mip tail: from the first level whose shorter side is 16 texels or
  // less, the remaining levels share one 32x32-block tile at the address that
  // level would have, each at its own offset (the SDK's GetPackedMipOffset).
  const bool packed_mips = (f[5] >> 11) & 1;
  const uint32_t min_log = std::min(log2_ceil(width), log2_ceil(height));
  const uint32_t packed_level = packed_mips ? (min_log > 4 ? min_log - 4 : 0) : 32;
  uint32_t levels = 1;
  // Linear textures have their mips too (Perfect Dark's DXT textures are all
  // linear): the same levels, rows of whole 32-block groups (below).
  if (mip_phys)
    while (levels <= guest_last && std::max(width >> levels, height >> levels) >= 1) ++levels;
  const uint32_t mip_addr = levels > 1 ? (mip_phys >= 0xE0000000u ? gpu_view(mip_phys) : cpu_view_of_physical(mip_phys & 0x1FFFFFFFu, 4096))
                                       : 0;
  if (levels > 1 && !mip_addr) levels = 1;
  // Block offset of level l inside the packed tail.
  const auto packed_offset = [&](uint32_t l, uint32_t& bx, uint32_t& by) {
    const uint32_t lw = log2_ceil(width), lh = log2_ceil(height);
    const uint32_t base = min_log > 4 ? min_log - 4 : 0, pm = l - base;
    uint32_t x = 0, y = 0;
    if (pm < 3) {
      if (lw > lh) y = 16 >> pm; else x = 16 >> pm;
    } else {
      if (lw > lh) x = (1u << (lw - base)) >> (pm - 2); else y = (1u << (lh - base)) >> (pm - 2);
    }
    bx = x / 4;
    by = y / 4;
  };
  size_t total = 0;
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t w = std::max(1u, width >> l), h = std::max(1u, height >> l), bw = blocks(w), bh = blocks(h);
    const uint32_t rows = as_rgba ? h : bh, row_pitch = ((as_rgba ? w * 4 : bw * bpb) + 255) & ~255u;
    p.levels.push_back({w, h, rows, row_pitch, total});
    total += size_t(row_pitch) * rows;
  }
  p.data.resize(total);
  uint32_t level_addr = addr;
  for (uint32_t l = 0; l < levels; ++l) {
    const PreparedTexture::Level& lv = p.levels[l];
    const uint32_t bw = blocks(lv.w), bh = blocks(lv.h);
    const uint32_t stored_bw = blocks(pow2_w >> l), stored_bh = blocks(pow2_h >> l);
    const bool in_tail = l >= 1 && l >= packed_level;
    // A level in the packed tail is addressed within the tail level's tiles.
    const uint32_t tail = std::max(packed_level, 1u);
    uint32_t pitch_blocks = l == 0 ? std::max(pitch / 4, bw) : in_tail ? blocks(pow2_w >> tail) : stored_bw;
    // A linear mip's rows are its (power-of-two) width in blocks rounded up to
    // 32 blocks (the SDK's GetGuestTextureLayout: tile-aligned, then 256 bytes,
    // which 32 blocks of 8 or 16 bytes already are); tiled addressing does that
    // rounding itself.
    if (!tiled && l >= 1) pitch_blocks = (pitch_blocks + 31) & ~31u;
    if (l == 1) level_addr = mip_addr;
    uint32_t ox = 0, oy = 0;
    if (l == 0) packed_base_offset(f, width, height, 4, ox, oy);
    else if (in_tail) packed_offset(l, ox, oy);
    const bool level_ok =
        as_rgba ? decode_texture_seh(level_addr, fmt, endian, tiled, lv.w, lv.h, pitch_blocks * 4, swizzle_identity,
                                     p.data.data() + lv.offset, lv.row_pitch, ox, oy)
                : untile_blocks_seh(level_addr, endian, tiled, bw, bh, pitch_blocks, bpb, p.data.data() + lv.offset,
                                    lv.row_pitch, ox, oy);
    if (!level_ok) {
      if (l == 0) return true;  // handled, failed
      p.levels.resize(l);       // the chain so far is still usable
      break;
    }
    if (REXCVAR_GET(gpu_native_dump_textures)) {
      std::vector<uint8_t> rgba(size_t(lv.w) * lv.h * 4);
      if (decode_texture_seh(level_addr, fmt, endian, tiled, lv.w, lv.h, pitch_blocks * 4, swizzle_identity, rgba.data(),
                             lv.w * 4, ox, oy))
        dump_texture(tex + l * 0x10000000u, fmt, lv.w, lv.h, rgba.data(), lv.w * 4);
    }
    if (l >= 1 && !in_tail) {  // next level: this one's 32x32-block tiles, 4 KB aligned (the tail stays put)
      const uint64_t size = uint64_t((stored_bw + 31) & ~31u) * ((stored_bh + 31) & ~31u) * bpb;
      level_addr += uint32_t((size + 4095) & ~4095ull);
    }
  }
  p.ok = true;
  if (REXCVAR_GET(gpu_native_log_creation))
    REXKRNL_INFO("PDNATIVE texture {:08X}: BC{}{} {}x{} {} levels (mips at {:08X}, fetch last level {}, tiled {}, "
                 "packed tail from {}, d4 {:08X} d5 {:08X})",
                 tex, fmt - 17, as_rgba ? " as RGBA" : "", width, height, p.levels.size(), mip_phys, guest_last, tiled,
                 packed_level, f[4], f[5]);
  return true;
}

// Everything else is decoded to RGBA8; mip levels the guest texture has (fetch
// dword 4 bits 6-9 = last level) are rebuilt from level 0 with a box filter.
void fill_rgba_levels(PreparedTexture& p, std::vector<uint8_t> prev, uint32_t width, uint32_t height, uint32_t levels);

void prepare_rgba(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  const uint32_t fmt = f[1] & 0x3F, endian = (f[1] >> 6) & 3;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32, swizzle = (f[3] >> 1) & 0xFFF;
  uint32_t levels = 1;
  const uint32_t guest_last = REXCVAR_GET(gpu_native_all_mips) ? 15 : (f[4] >> 6) & 0xF;
  while (levels <= guest_last && std::max(width, height) >> levels) ++levels;
  std::vector<uint8_t> prev(size_t(width) * height * 4);
  TexFormat tf{};
  tex_format(fmt, tf);
  uint32_t ox = 0, oy = 0;
  packed_base_offset(f, width, height, std::max(1u, tf.block), ox, oy);
  if (!decode_texture_seh(addr, fmt, endian, tiled, width, height, std::max(pitch, width), swizzle, prev.data(), width * 4,
                          ox, oy))
    return;
  if (REXCVAR_GET(gpu_native_log_creation))
    REXKRNL_INFO("PDNATIVE texture {:08X}: d0 {:08X} d3 {:08X} fmt {} {}x{} pitch {} tiled {} endian {} swizzle {:03X} d4 {:08X} d5 {:08X} "
                 "levels {}", tex, f[0], f[3], fmt, width, height, pitch, tiled, endian, swizzle, f[4], f[5], levels);
  if (REXCVAR_GET(gpu_native_dump_textures)) dump_texture(tex, fmt, width, height, prev.data(), width * 4);
  fill_rgba_levels(p, std::move(prev), width, height, levels);
}

// `prev`: the base level, RGBA8 rows of width * 4 bytes. Fills `p` with it and
// `levels - 1` box-filtered mips.
void fill_rgba_levels(PreparedTexture& p, std::vector<uint8_t> prev, uint32_t width, uint32_t height, uint32_t levels) {
  std::vector<uint8_t> cur;
  p.format = RenderFormat::R8G8B8A8_UNORM;
  p.width = width; p.height = height; p.block_bytes = 4; p.block = 1;
  size_t total = 0;
  for (uint32_t l = 0, w = width, h = height; l < levels; ++l, w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
    const uint32_t row_pitch = (w * 4 + 255) & ~255u;
    p.levels.push_back({w, h, h, row_pitch, total});
    total += size_t(row_pitch) * h;
  }
  p.data.resize(total);
  uint32_t w = width, h = height;
  for (uint32_t l = 0; l < levels; ++l) {
    const PreparedTexture::Level& lv = p.levels[l];
    if (l) {  // box-filter the previous level
      cur.resize(size_t(lv.w) * lv.h * 4);
      const size_t sp = size_t(w) * 4;
      for (uint32_t y = 0; y < lv.h; ++y) {
        const uint8_t* r0 = prev.data() + std::min(y * 2, h - 1) * sp;
        const uint8_t* r1 = prev.data() + std::min(y * 2 + 1, h - 1) * sp;
        uint8_t* d = cur.data() + size_t(y) * lv.w * 4;
        for (uint32_t x = 0; x < lv.w; ++x) {
          const uint32_t a = std::min(x * 2, w - 1) * 4, b = std::min(x * 2 + 1, w - 1) * 4;
          for (int c = 0; c < 4; ++c) d[x * 4 + c] = uint8_t((r0[a + c] + r0[b + c] + r1[a + c] + r1[b + c] + 2) >> 2);
        }
      }
      prev.swap(cur);
      w = lv.w; h = lv.h;
    }
    for (uint32_t y = 0; y < lv.h; ++y)
      std::memcpy(p.data.data() + lv.offset + size_t(y) * lv.row_pitch, prev.data() + size_t(y) * lv.w * 4, size_t(lv.w) * 4);
  }
  p.ok = true;
}

void prepare_2d(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  if (!prepare_bc(f, addr, tex, p)) prepare_rgba(f, addr, tex, p);
}

// Cube maps (fetch dimension 3; Perfect Dark's shiny surfaces: logos, metal,
// flags): six faces one after another, each laid out like a 2D texture's base
// level, at a stride of the face's 32x32-block tiles rounded up to 4 KB (as
// xenia's texture layout has array slices). The base level only, for now.
uint32_t cube_face_stride(const uint32_t* f) {
  TexFormat tf{};
  if (!tex_format(f[1] & 0x3F, tf)) return 0;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32;
  const uint32_t bw = (std::max(pitch, width) + tf.block - 1) / tf.block, bh = (height + tf.block - 1) / tf.block;
  const uint64_t bytes = tiled ? uint64_t((bw + 31) & ~31u) * ((bh + 31) & ~31u) * tf.bpb : uint64_t(bw) * bh * tf.bpb;
  return uint32_t((bytes + 4095) & ~4095ull);
}

void prepare_cube(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  uint32_t ff[6];
  std::memcpy(ff, f, sizeof(ff));
  ff[4] &= ~(0xFu << 6);                 // last mip level 0: the base level only
  ff[5] = (ff[5] & ~(3u << 9)) | (1u << 9);  // each face as a 2D texture
  const uint32_t stride = cube_face_stride(f);
  if (!stride) return;
  for (uint32_t face = 0; face < 6; ++face) {
    PreparedTexture q;
    prepare_2d(ff, addr + face * stride, tex, q);
    if (!q.ok || q.levels.empty() || (face && (q.format != p.format || q.width != p.width || q.height != p.height)))
      return;
    if (!face) {
      p.format = q.format;
      p.width = q.width;
      p.height = q.height;
      p.block_bytes = q.block_bytes;
      p.block = q.block;
    }
    const size_t base = p.data.size();
    PreparedTexture::Level lv = q.levels[0];
    lv.offset += base;
    p.levels.push_back(lv);
    p.data.insert(p.data.end(), q.data.begin(), q.data.begin() + q.levels[0].offset + size_t(lv.row_pitch) * lv.rows);
  }
  p.faces = 6;
  p.ok = true;
}

// Thread-safe: reads guest memory only (under SEH) and fills `p`.
void prepare_texture(const uint32_t* f, uint32_t addr, uint32_t tex, PreparedTexture& p) {
  if (((f[5] >> 9) & 3) == 3) prepare_cube(f, addr, tex, p);
  else prepare_2d(f, addr, tex, p);
}

// Game thread, at the start of a draw (may flush the upload ring): creates the
// host texture and records the copies of its levels.
// Descriptor slots: a slot freed by a re-uploaded texture is reused only once
// no frame in flight can still read it.
uint32_t alloc_texture_slot() {
  if (!g.free_slots.empty()) {
    const uint32_t slot = g.free_slots.back();
    g.free_slots.pop_back();
    return slot;
  }
  return g.next_slot < kTextureSlots ? g.next_slot++ : 0;
}

uint32_t commit_texture(ResolvedTexture& t, const PreparedTexture& p) {
  if (!p.ok || p.levels.empty()) { t.slot = UINT32_MAX; return 0; }
  uint64_t total = 0;
  for (const auto& lv : p.levels) total += uint64_t(lv.row_pitch) * lv.rows + 512;
  ensure_room(total);
  const uint32_t mips = uint32_t(p.levels.size()) / p.faces;
  t.texture = g.device->createTexture(
      p.faces == 6 ? RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, p.width, p.height, 1, mips, 6, p.format,
                                                RenderTextureFlag::CUBE)
                   : RenderTextureDesc::Texture2D(p.width, p.height, mips, p.format));
  t.view.reset();
  if (p.faces == 6) {
    RenderTextureViewDesc vd = RenderTextureViewDesc::TextureCube(p.format);
    vd.arraySize = 6;
    vd.mipLevels = mips;
    t.view = t.texture->createTextureView(vd);
  }
  t.slot = alloc_texture_slot();
  g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::COPY_DEST));
  for (uint32_t l = 0; l < p.levels.size(); ++l) {
    const auto& lv = p.levels[l];
    const uint64_t bytes = uint64_t(lv.row_pitch) * lv.rows;
    const uint64_t off = upload_alloc(bytes, 512);
    if (off == UINT64_MAX) break;
    std::memcpy(g.upload_ptr + off, p.data.data() + lv.offset, bytes);
    g.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(t.texture.get(), l % mips, l / mips),
                             RenderTextureCopyLocation::PlacedFootprint(g.upload.get(), p.format, lv.w, lv.h, 1,
                                                                        lv.row_pitch / p.block_bytes * p.block, off));
  }
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::SHADER_READ));
  g.textures->setTexture(t.slot, t.texture.get(), RenderTextureLayout::SHADER_READ, t.view.get());
  return t.slot;
}

// Texture worker: prepares textures the game thread had no time for this
// frame (gpu_native_async_textures); the game thread commits them later.
struct TextureJob {
  uint32_t f[6] = {}, addr = 0, tex = 0;
  PreparedTexture result;
  std::atomic<bool> done{false};
};

struct TextureWorker {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::shared_ptr<TextureJob>> queue;
  bool started = false;

  void push(std::shared_ptr<TextureJob> job) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      queue.push_back(std::move(job));
      if (!started) {
        started = true;
        std::thread([this] { run(); }).detach();
      }
    }
    cv.notify_one();
  }

  void run() {
    for (;;) {
      std::shared_ptr<TextureJob> job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return !queue.empty(); });
        job = std::move(queue.front());
        queue.pop_front();
      }
      prepare_texture(job->f, job->addr, job->tex, job->result);
      job->done.store(true, std::memory_order_release);
    }
  }
};
TextureWorker g_texture_worker;

// Host texture slot for a guest texture object (0 = white when unsupported or
// still being prepared). may_commit: the caller is at the start of a draw,
// where new textures may be created (their uploads can flush the ring).
bool hash_bytes_seh(uint32_t addr, uint32_t bytes, uint64_t* out);

void copy_pending_depth(uint32_t dest, ResolvedTexture& r);
void depth_will_change();

// The slot of a colour resolve as `tex` samples it: its own, or a view whose
// components follow the constant selectors (0 / 1) of tex's fetch swizzle.
// The colour channels keep the host order (the resolve holds the picture).
uint32_t resolved_slot(uint32_t tex, ResolvedTexture& r) {
  if (!r.texture || r.span != 0 || g.depth_sets.count(tex)) return r.slot;
  const uint16_t sw = uint16_t((g32(tex + 40) >> 1) & 0xFFF);
  bool constant = false;
  for (int c = 0; c < 4; ++c) constant |= ((sw >> (3 * c)) & 7) >= 4;
  if (!constant) return r.slot;
  auto& v = r.swizzled[sw];
  if (!v.first) {
    RenderTextureViewDesc vd = RenderTextureViewDesc::Texture2D(kSceneFormat);
    RenderSwizzle* comps[4] = {&vd.componentMapping.r, &vd.componentMapping.g, &vd.componentMapping.b, &vd.componentMapping.a};
    for (int c = 0; c < 4; ++c) {
      const uint32_t sel = (sw >> (3 * c)) & 7;
      *comps[c] = sel == 4 ? RenderSwizzle::ZERO : sel == 5 ? RenderSwizzle::ONE : RenderSwizzle::IDENTITY;
    }
    v.first = r.texture->createTextureView(vd);
    v.second = alloc_texture_slot();
    g.textures->setTexture(v.second, r.texture.get(), RenderTextureLayout::SHADER_READ, v.first.get());
    REXKRNL_INFO("PDNATIVE resolve {:08X} sampled with swizzle {:03X}: view in slot {}", tex, sw, v.second);
  }
  return v.second;
}

uint32_t texture_slot(uint32_t tex, bool may_commit = false) {
  if (!tex) return 0;
  const auto r = g.resolved.find(tex);
  if (r != g.resolved.end()) {
    if (r->second.depth_pending && may_commit) {
      r->second.depth_used_frame = g.frames + 1;
      copy_pending_depth(tex, r->second);
    }
    return resolved_slot(tex, r->second);
  }
  uint32_t f[6];
  for (int i = 0; i < 6; ++i) f[i] = g32(tex + 28 + i * 4);
  if (const auto rb = g.resolved_base.find(f[1] & 0xFFFFF000u); rb != g.resolved_base.end()) {
    const auto rr = g.resolved.find(rb->second);
    if (rr != g.resolved.end() && rr->second.texture) return resolved_slot(tex, rr->second);
  }
  const uint64_t key = fnv1a(reinterpret_cast<const uint8_t*>(f), sizeof(f));
  ResolvedTexture& t = g.textures_by_fetch[key];
  if (t.job) {  // on the worker
    if (!may_commit || !t.job->done.load(std::memory_order_acquire)) return 0;
    const std::shared_ptr<TextureJob> job = std::move(t.job);
    return commit_texture(t, job->result);
  }
  if (t.texture && t.dynamic && may_commit && t.checked_frame != g.frames + 1) {
    t.checked_frame = g.frames + 1;
    uint64_t h = 0;
    if (hash_bytes_seh(t.addr, t.span, &h) && h != t.content) {
      // Rewritten by the game: upload again (the old copy and its slot stay
      // until the frames in flight are done with them).
      g.retired_textures.push_back({g.frames, std::move(t.texture), t.slot});
      t.content = h;
      PreparedTexture p;
      prepare_texture(f, t.addr, tex, p);
      return commit_texture(t, p);
    }
  }
  if (t.texture || t.slot == UINT32_MAX || !may_commit) return t.texture ? t.slot : 0;
  struct Timer {
    double t0 = now_ms();
    ~Timer() { g.tex_ms += now_ms() - t0; ++g.tex_n; }
  } timer;
  const uint32_t fmt = f[1] & 0x3F, dim = (f[5] >> 9) & 3;
  const bool tiled = (f[0] >> 31) & 1;
  const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32;
  const uint32_t phys = f[1] & 0xFFFFF000u;
  TexFormat tf;
  if ((dim != 1 && dim != 3) || !tex_format(fmt, tf) || width > 4096 || height > 4096) {
    t.slot = UINT32_MAX;
    static uint64_t logged = 0;
    if (fmt < 64 && !(logged & (1ull << fmt))) {
      logged |= 1ull << fmt;
      REXKRNL_WARN("PDNATIVE texture format {} dim {} {}x{} not uploaded yet", fmt, dim, width, height);
    }
    return 0;
  }
  const uint32_t bh = (height + tf.block - 1) / tf.block, pitch_blocks = std::max<uint32_t>(pitch, width) / tf.block;
  const uint32_t span = dim == 3 ? cube_face_stride(f) * 6
                        : tiled ? ((pitch_blocks + 31) & ~31u) * ((bh + 31) & ~31u) * tf.bpb : pitch_blocks * bh * tf.bpb;
  // Base addresses stored in the 0xE0000000 range (CAFF-loaded textures) are
  // read 4 KB on, like shader microcode (tested: the title logo only decodes
  // there); true physical addresses go through the mapped alias.
  const uint32_t addr = phys >= 0xE0000000u ? gpu_view(phys) : cpu_view_of_physical(phys & 0x1FFFFFFFu, span);
  if (!addr) { t.slot = UINT32_MAX; return 0; }
  t.dynamic = phys < 0xE0000000u;
  t.addr = addr;
  t.span = span;
  if (t.dynamic) {
    hash_bytes_seh(addr, span, &t.content);
    t.checked_frame = g.frames + 1;
  } else if (!REXCVAR_GET(gpu_native_stage_textures)) {
    hash_bytes_seh(addr, span, &t.content);  // (test: kept to count stale ones in begin_frame)
  }
  // In level, a burst of new textures (new objects coming into view) used to
  // cost a 25 ms frame: past ~1 ms of texture work in a frame they go to the
  // worker, and the draws using them wait a frame or two. Frames that ask for
  // many at once are level loads, which are long anyway: those stay here, so
  // a level doesn't open with missing textures.
  // The front end (stage 90) loads in the frame, as the Xenos path does: on
  // the worker, the main menu's first frame or two drew without its pages and
  // photo, showing the bare green folder for a moment.
  ++g.tex_requests;
  if (REXCVAR_GET(gpu_native_async_textures) && g32(kStageNum) != 90u &&
      g.tex_ms + (now_ms() - timer.t0) > 1.0 && g.tex_requests <= 24) {
    auto job = std::make_shared<TextureJob>();
    std::memcpy(job->f, f, sizeof(f));
    job->addr = addr;
    job->tex = tex;
    t.job = job;
    g_texture_worker.push(std::move(job));
    return 0;
  }
  const double t_dec = now_ms();
  PreparedTexture p;
  prepare_texture(f, addr, tex, p);
  g.tex_decode_ms += now_ms() - t_dec;
  return commit_texture(t, p);
}

// True while the texture's data is still being prepared on the worker: draws
// using it wait (the object appears a frame or two later instead of white).
bool texture_pending(uint32_t tex) {
  if (!tex || g.resolved.count(tex)) return false;
  uint32_t f[6];
  for (int i = 0; i < 6; ++i) f[i] = g32(tex + 28 + i * 4);
  const auto it = g.textures_by_fetch.find(fnv1a(reinterpret_cast<const uint8_t*>(f), sizeof(f)));
  return it != g.textures_by_fetch.end() && it->second.job != nullptr;
}

// Sampler slot from the texture's fetch constant (filters, clamp modes, mip
// filter, anisotropy, LOD bias and mip range), as the SDK's Xenos texture
// cache sets its samplers up. Clamp and filter come from the device's copy of
// the fetch constant (device+1152 + sampler*24: SetTexture copies the
// texture's in, and SetSamplerState patches it there), not the texture header.
//   d3: 19-20 mag, 21-22 min, 23-24 mip filter (0 point, 1 linear, 2 base map),
//       25-27 aniso (0 off, n = 2^(n-1):1)
//   d4: 2-5 / 6-9 mip min / max level, 12-21 LOD bias (signed, 1/32 units)
// The Texture Filtering setting (anisotropic_override, -1 = the game's own; the
// Xenos plugin's default 3 = 4x when that cvar isn't loaded) applies as there:
// to linear, mipmapped sampling only.
uint32_t sampler_slot(uint32_t dev, uint32_t sampler, uint32_t tex) {
  if (!tex) return 0;
  const uint32_t fetch = dev + 1152 + sampler * 24;
  const uint32_t d0 = g32(fetch), d3 = g32(fetch + 12), d4 = g32(fetch + 16), d5 = g32(fetch + 20);
  const uint32_t clamp_x = (d0 >> 10) & 7, clamp_y = (d0 >> 13) & 7, mag = (d3 >> 19) & 3, min = (d3 >> 21) & 3;
  const uint32_t mip = (d3 >> 23) & 3, mip_min = (d4 >> 2) & 0xF, mip_max = std::max((d4 >> 6) & 0xF, mip_min);
  const int32_t bias = int32_t(d4 << 10) >> 22;  // bits 12-21, sign-extended
  const int32_t override_aniso = pd::g_native_aniso.load(std::memory_order_relaxed);
  uint32_t aniso = (d3 >> 25) & 7;
  // As the classic renderer (and GoldenEye d28d81b): the Texture Filtering
  // setting applies only to a texture that really has mips (a mip address;
  // without one the fetch constant's mip range is ignored and the game's own
  // anisotropy stays).
  const bool has_mips = (d5 >> 12) != 0 && mip_max > mip_min;
  if (override_aniso > -1 && override_aniso < 6 && mip != 2 && mag == 1 && min == 1 && has_mips) aniso = uint32_t(override_aniso);
  aniso = std::min<uint32_t>(aniso, 5);
  const uint64_t key = uint64_t(clamp_x) | clamp_y << 3 | mag << 6 | min << 8 | mip << 10 | aniso << 12 | mip_min << 15 |
                       mip_max << 19 | uint64_t(uint32_t(bias) & 0x3FF) << 23;
  const auto it = g.sampler_slots.find(key);
  if (it != g.sampler_slots.end()) return it->second;
  if (g.sampler_objects.size() >= kSamplerSlots - 1) return 0;
  auto addr = [](uint32_t c) {
    switch (c) {
      case 0: return RenderTextureAddressMode::WRAP;
      case 1: return RenderTextureAddressMode::MIRROR;
      case 2: case 3: case 5: return RenderTextureAddressMode::CLAMP;
      default: return RenderTextureAddressMode::BORDER;
    }
  };
  RenderSamplerDesc sd;
  sd.magFilter = mag == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  sd.minFilter = min == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
  sd.mipmapMode = mip == 0 ? RenderMipmapMode::NEAREST : RenderMipmapMode::LINEAR;
  if (aniso) {
    sd.anisotropyEnabled = true;
    sd.maxAnisotropy = 1u << (aniso - 1);
  }
  sd.mipLODBias = float(bias) / 32.0f;
  sd.minLOD = float(mip_min);
  sd.maxLOD = mip == 2 ? float(mip_min) : float(mip_max);  // base map: no mips
  sd.addressU = addr(clamp_x);
  sd.addressV = addr(clamp_y);
  sd.addressW = RenderTextureAddressMode::CLAMP;
  g.sampler_objects.push_back(g.device->createSampler(sd));
  const uint32_t slot = uint32_t(g.sampler_objects.size());  // slot 0 = default
  g.samplers->setSampler(slot, g.sampler_objects.back().get());
  g.sampler_slots[key] = slot;
  return slot;
}

bool hash_bytes_seh(uint32_t addr, uint32_t bytes, uint64_t* out) {
  __try {
    *out = XXH3_64bits(gmem() + addr, bytes);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// xxHash of every source's bytes for vertices [first, first + vcount).
bool hash_sources_seh(const Source* srcs, size_t n, uint32_t first, uint32_t vcount, uint64_t* out) {
  __try {
    uint64_t h = 0;
    for (size_t i = 0; i < n; ++i)
      h = XXH3_64bits_withSeed(gmem() + srcs[i].addr + size_t(first) * srcs[i].stride, size_t(vcount) * srcs[i].stride, h);
    *out = h;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// --- render states from the device shadows --------------------------------
// Field locations come from the D3D render-state getters (d3d_functions.toml):
// RB_DEPTHCONTROL (+10548), PA_SU_SC_MODE_CNTL (+10568), RB_BLENDCONTROL
// (+11840), blend enables (+11844), RB_COLORCONTROL (+10556), ZEnable (+11868),
// ColorWriteEnable (+11852), AlphaRef float (+10500).
struct DrawState {
  bool z_enable, z_write, blend;
  uint8_t z_func, cull, src, dst, op, src_a, dst_a, op_a, write_mask;
  bool alpha_test;
  uint8_t alpha_func;
  float alpha_ref;
  bool alpha_to_mask;  // RB_COLORCONTROL bit 4: alpha also sets the MSAA coverage (soft text edges)
};

DrawState read_state(uint32_t dev) {
  DrawState st{};
  const uint32_t depth = g32(dev + 10548), mode = g32(dev + 10568), blend = g32(dev + 12024);
  const uint32_t enables = g32(dev + 12028), color = g32(dev + 10556);
  st.z_enable = g32(dev + 12052) != 0;
  st.z_write = (depth >> 2) & 1;
  st.z_func = (depth >> 4) & 7;
  st.cull = mode & 7;
  st.blend = (enables >> 31) & 1;
  st.src = blend & 0x1F; st.op = (blend >> 5) & 7; st.dst = (blend >> 8) & 0x1F;
  st.src_a = (blend >> 16) & 0x1F; st.op_a = (blend >> 21) & 7; st.dst_a = (blend >> 24) & 0x1F;
  if (!((enables >> 30) & 1)) { st.src_a = st.src; st.dst_a = st.dst; st.op_a = st.op; }
  st.write_mask = uint8_t(g32(dev + 12036) & 0xF);
  st.alpha_test = (color >> 3) & 1;
  st.alpha_func = color & 7;
  st.alpha_to_mask = (color >> 4) & 1;
  uint32_t ref = g32(dev + 10500);
  std::memcpy(&st.alpha_ref, &ref, 4);
  return st;
}
DrawState g_draw_state{};  // state of the draw being prepared (pipeline key + desc)

RenderBlend to_blend(uint8_t b) {
  switch (b) {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return RenderBlend::SRC_COLOR;
    case 5: return RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_COLOR;
    case 9: return RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    case 12: return RenderBlend::BLEND_FACTOR;
    case 13: return RenderBlend::INV_BLEND_FACTOR;
    case 16: return RenderBlend::SRC_ALPHA_SAT;
    default: return RenderBlend::ONE;
  }
}
RenderBlendOperation to_blend_op(uint8_t o) {
  switch (o) {
    case 1: return RenderBlendOperation::SUBTRACT;
    case 2: return RenderBlendOperation::MIN;
    case 3: return RenderBlendOperation::MAX;
    case 4: return RenderBlendOperation::REV_SUBTRACT;
    default: return RenderBlendOperation::ADD;
  }
}
RenderComparisonFunction to_cmp(uint8_t c) {
  switch (c) {
    case 0: return RenderComparisonFunction::NEVER;
    case 1: return RenderComparisonFunction::LESS;
    case 2: return RenderComparisonFunction::EQUAL;
    case 3: return RenderComparisonFunction::LESS_EQUAL;
    case 4: return RenderComparisonFunction::GREATER;
    case 5: return RenderComparisonFunction::NOT_EQUAL;
    case 6: return RenderComparisonFunction::GREATER_EQUAL;
    default: return RenderComparisonFunction::ALWAYS;
  }
}

// Decodes vcount vertices into out; returns false if guest memory faulted.
// Kept free of C++ objects so it can use SEH.
bool decode_vertices_seh(const Element* els, size_t n_els, const Source* srcs, const uint16_t* keys,
                         size_t n_keys, uint32_t first, uint32_t vcount, uint8_t* out, uint32_t host_stride,
                         const HostFmt* fmts, const uint32_t* offsets) {
  __try {
    for (uint32_t v = 0; v < vcount; ++v) {
      uint8_t* dst = out + uint64_t(v) * host_stride;
      for (size_t e = 0; e < n_els; ++e) {
        size_t k = 0;
        while (k < n_keys && keys[k] != els[e].stream) ++k;
        const Source& src = srcs[k];
        decode_element(els[e], src.addr + (first + v) * src.stride + els[e].offset, src.endian, dst + offsets[e], fmts[e]);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// --- frame ------------------------------------------------------------------

void copy_scene_to(RenderTexture* dst);

void bind_target() {
  const bool scene = g.guest_depth != 0 || g.guest_rt == 0;
  if (scene) {
    if (g.guest_rt) g.scene_surface = g.guest_rt;
    if (g.bound_fb == g.scene_fb.get()) return;
    g.cmd->barriers(RenderBarrierStage::GRAPHICS,
                    {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::COLOR_WRITE),
                     RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::DEPTH_WRITE)});
    g.cmd->setFramebuffer(g.scene_fb.get());
    g.bound_fb = g.scene_fb.get();
    g.bound_color = g.scene.get();
    g.bound_scene = true;
    return;
  }
  Native::ColorTarget& t = g.color_targets[g.guest_rt];
  if (!t.color) {
    t.color = g.device->createTexture(RenderTextureDesc::ColorTarget(g_scene_w, g_scene_h, kSceneFormat));
    const RenderTexture* c = t.color.get();
    t.fb = g.device->createFramebuffer(RenderFramebufferDesc(&c, 1));
  }
  if (g.bound_fb == t.fb.get()) return;
  // EDRAM aliasing: a surface at the same EDRAM base as the scene starts out
  // holding the scene's pixels on the 360 (the title's post pass blends onto
  // them: without this the never-cleared target smeared old frames). The base
  // is the low 12 bits of the surface's colour info word (+24).
  const uint32_t base_rt = g32(g.guest_rt + 24) & 0xFFF;
  if (g.bound_scene && g.scene_surface && base_rt == (g32(g.scene_surface + 24) & 0xFFF)) {
    copy_scene_to(t.color.get());
    g.cmd->barriers(RenderBarrierStage::GRAPHICS,
                    {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::COLOR_WRITE),
                     RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::DEPTH_WRITE)});
  }
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.color.get(), RenderTextureLayout::COLOR_WRITE));
  g.cmd->setFramebuffer(t.fb.get());
  g.bound_fb = t.fb.get();
  g.bound_color = t.color.get();
  g.bound_scene = false;
}

void begin_frame() {
  if (g.recording) return;
  if (!g.warm.empty() && REXCVAR_GET(gpu_native_warm_shaders)) warm_registered_shaders();
  // The Anti-Aliasing setting changed: wait for the frames in flight, then new
  // scene targets with the new sample count (pipelines are keyed by it, and
  // the depth resolve pass is rebuilt for it).
  if (const RenderSampleCounts want = pick_msaa(); g.scene && want != g.msaa.sampleCount) {
    for (uint32_t i = 0; i < kFrames; ++i) {
      if (!g.pending[i]) continue;
      if (g.out.vk) pd::native_vk::WaitPresent(g.out.vk_handoff[i]);  // (the worker is done with the fence)
      g.queue->waitForCommandFence(g.fences[i].get());
      g.pending[i] = false;
    }
    g.msaa.sampleCount = want;
    g.bound_fb = nullptr;
    g.bound_color = nullptr;
    g.scene_fb.reset();
    create_scene_targets();
    for (auto& [dest, r] : g.resolved) {  // full-size depth resolves: made again with the new pass
      if (!g.depth_sets.count(dest) || !r.texture || r.span != 0) continue;
      g.retired_textures.push_back({g.frames, std::move(r.texture), r.slot});
      r.depth_pending = false;
    }
    g.depth_pending.clear();
    g.depth_sets.clear();
    g.depth_pipe.reset();
    g.depth_shader.reset();
    g.depth_layout.reset();
    g.depth_view.reset();
    g.depth_pass_failed = false;
    REXKRNL_INFO("PDNATIVE anti-aliasing: {}x MSAA", uint32_t(want));
  }
  // A new stage (a level, or the front end) loads its textures where the last
  // one's were: the same fetch constants can then name a different picture
  // (Cradle after a run of levels showed red from an earlier one), and the
  // old ones would stay in video memory. Start the texture cache afresh.
  if (const uint32_t stage = g32(kStageNum); stage != g.textures_stage && !REXCVAR_GET(gpu_native_stage_textures)) {
    // Test (cache kept across stages): how many cached pictures the new stage
    // has overwritten in memory, i.e. would be drawn with the old picture.
    g.textures_stage = stage;
    size_t cached = 0, stale = 0;
    for (const auto& [key, t] : g.textures_by_fetch) {
      if (!t.texture || t.dynamic) continue;
      ++cached;
      uint64_t h = 0;
      if (hash_bytes_seh(t.addr, t.span, &h) && h != t.content) ++stale;
    }
    REXKRNL_INFO("PDNATIVE stage {}: {} cached level textures, {} now hold a different picture", stage, cached, stale);
  } else if (const uint32_t stage = g32(kStageNum); stage != g.textures_stage) {
    g.textures_stage = stage;
    size_t n = 0;
    for (auto& [key, t] : g.textures_by_fetch) {
      if (!t.texture) continue;
      g.retired_textures.push_back({g.frames, std::move(t.texture), t.slot});
      ++n;
    }
    g.textures_by_fetch.clear();  // (a texture still on the worker is dropped with its job)
    if (n) REXKRNL_INFO("PDNATIVE stage {}: {} textures from the last stage released", stage, n);
  }
  g.cmd->begin();
  g.prof_active = g.prof_pool && REXCVAR_GET(gpu_native_profile_frame) > 0 &&
                  g.frames + 1 == uint64_t(REXCVAR_GET(gpu_native_profile_frame));
  if (g_vulkan) {  // Vulkan queries are reset before use, outside a render pass
    if (g.timestamps) g.cmd->resetQueryPool(g.timestamps.get(), g.slot * 2, 2);
    if (g.prof_active) g.cmd->resetQueryPool(g.prof_pool.get(), 0, g.prof_pool->getCount());
  }
  g_occ.list_reset = false;
  g.layout_bound = false;
  g.bound_fb = nullptr;
  bind_target();
  if (g.timestamps) g.cmd->writeTimestamp(g.timestamps.get(), g.slot * 2);
  g.prof_labels.clear();
  if (g.prof_active) { g.cmd->writeTimestamp(g.prof_pool.get(), 0); g.prof_labels.push_back("start"); }
  g.recording = true;
  g.draws = g.skipped = 0;
  std::memset(g.skip_reason, 0, sizeof(g.skip_reason));
}

void prof_mark(std::string label) {
  if (!g.prof_active || g.prof_labels.size() >= 4096) return;
  g.cmd->writeTimestamp(g.prof_pool.get(), uint32_t(g.prof_labels.size()));
  g.prof_labels.push_back(std::move(label));
}

void set_common_state(uint32_t dev) {
  // The root signature and the bindless tables stay bound for the whole
  // command list (re-setting them every draw reset all root bindings).
  if (!g.layout_bound) {
    g.cmd->setGraphicsPipelineLayout(g.layout.get());
    g.cmd->setGraphicsDescriptorSet(g.textures.get(), 0);
    g.cmd->setGraphicsDescriptorSet(g.textures.get(), 1);
    g.cmd->setGraphicsDescriptorSet(g.textures.get(), 2);
    g.cmd->setGraphicsDescriptorSet(g.samplers.get(), 3);
    g.layout_bound = true;
  }
  const uint32_t vp = dev + kDevViewport;
  float f[6];
  for (int i = 0; i < 6; ++i) {
    const uint32_t u = g32(vp + i * 4);
    std::memcpy(&f[i], &u, 4);
  }
  if (f[2] <= 0 || f[3] <= 0 || (g32(dev + 10572) & 0x3F) == 0) {
    f[0] = f[1] = 0; f[2] = float(kGuestW); f[3] = float(kGuestH); f[4] = 0; f[5] = 1;
  }
  const float vp_w = f[2], vp_h = f[3];  // in guest pixels (the half-pixel offset below)
  for (int i = 0; i < 4; ++i) f[i] *= float(g_scene_scale);  // guest pixels -> scene pixels
  // Perfect Dark sets an inverted depth range (MinZ 1, MaxZ 0: reversed depth,
  // cleared to 0 and tested GREATER_EQUAL); GoldenEye's "MaxZ 1 if not above
  // MinZ" flattened all its depth to 1. Kept as given, within [0, 1].
  float zmin = std::clamp(f[4], 0.0f, 1.0f), zmax = zmin == std::clamp(f[5], 0.0f, 1.0f) ? 1.0f : std::clamp(f[5], 0.0f, 1.0f);
  // A backwards range is drawn as the classic renderer does (SDK util/draw.cpp):
  // a forward viewport with Z flipped in the vertex shader (z' = w - z, c19).
  // Direct3D 12 never allowed a reversed viewport depth range (some drivers
  // honour it, some don't); the depth values are the same either way.
  float depth_flip[2] = {1.0f, 0.0f};
  if (zmin > zmax && REXCVAR_GET(gpu_native_depth_flip_vs)) {
    std::swap(zmin, zmax);
    depth_flip[0] = -1.0f;
    depth_flip[1] = 1.0f;
  }
  RenderViewport viewport(f[0], f[1], f[2], f[3], zmin, zmax);
  g.cmd->setViewports(&viewport, 1);
  // The hardware scissor Xenos applies to every draw (the cut-scene letterbox
  // clips the sun's glow with it); the whole target if it is empty.
  {
    RenderRect scissor(0, 0, g_scene_w, g_scene_h);
    const uint32_t tl = g32(dev + kDevWindowScissorTL), br = g32(dev + kDevWindowScissorBR);
    const int32_t l = tl & 0x7FFF, t = (tl >> 16) & 0x7FFF, r = br & 0x7FFF, b = (br >> 16) & 0x7FFF;
    if (r > l && b > t) {
      const int32_t k = int32_t(g_scene_scale), w = int32_t(g_scene_w), h = int32_t(g_scene_h);
      scissor = RenderRect(std::min(l * k, w), std::min(t * k, h), std::min(r * k, w), std::min(b * k, h));
    }
    g.cmd->setScissors(&scissor, 1);
  }

  // Constants: guest big-endian floats -> little-endian.
  // Each shader's constants are only the bytes it reads (<hash>.cbsize), and a
  // block identical to one already uploaded for this submission is reused:
  // copying full 4 KB + 3.5 KB blocks per draw took ~3.7 ms of GPU time a frame.
  uint8_t* cbase = g.const_ptr + g.slot * kSlotConsts;
  auto swap_copy = [](uint8_t* dst, uint32_t src, uint32_t bytes) {
    const uint8_t* s = gmem() + src;
    for (uint32_t i = 0; i < bytes; i += 4) {
      uint32_t w;
      std::memcpy(&w, s + i, 4);
      w = __builtin_bswap32(w);
      std::memcpy(dst + i, &w, 4);
    }
  };
  auto upload_block = [&](uint32_t src, uint32_t bytes) -> uint64_t {
    alignas(16) uint8_t tmp[4096];
    bytes = std::max<uint32_t>(16, std::min<uint32_t>(bytes, 4096));
    swap_copy(tmp, src, bytes);
    const uint64_t key = XXH3_64bits_withSeed(tmp, bytes, bytes);
    const auto it = g.const_dedup.find(key);
    if (it != g.const_dedup.end()) return it->second;
    const uint64_t off = const_alloc(bytes);
    if (off != UINT64_MAX) { std::memcpy(cbase + off, tmp, bytes); g.const_dedup[key] = off; }
    return off;
  };
  const uint64_t vs = upload_block(dev + kDevVsConsts, g_vs_cb_bytes);
  const uint64_t ps = upload_block(dev + kDevPsConsts, g_ps_cb_bytes);
  const uint64_t shared = const_alloc(512);
  if (vs == UINT64_MAX || ps == UINT64_MAX || shared == UINT64_MAX) return;
  alignas(16) uint32_t sh[128] = {};
  for (uint32_t s = 0; s < 16; ++s) {
    sh[s] = texture_slot(g_textures[s]);       // Texture2D descriptor index
    // TextureCube descriptor index (XenosRecomp's shared constants: 2D at s, 3D
    // at 16 + s, cube at 32 + s, sampler at 48 + s).
    const bool cube = g_textures[s] && !g.resolved.count(g_textures[s]) && ((g32(g_textures[s] + 48) >> 9) & 3) == 3;
    sh[32 + s] = cube && sh[s] ? sh[s] : g.white_cube_slot;
    sh[48 + s] = sampler_slot(dev, s, g_textures[s]);  // sampler descriptor index
  }
  // Alpha test: the translated pixel shaders clip(alpha - threshold); only
  // GREATER / GREATER_EQUAL tests map onto that, others draw untested.
  const DrawState st = read_state(dev);
  float threshold = 0.0f;
  if (st.alpha_test && (st.alpha_func == 4 || st.alpha_func == 6)) {
    threshold = st.alpha_ref > 1.0f ? st.alpha_ref / 255.0f : st.alpha_ref;
    if (st.alpha_func == 4) threshold += 1.0f / 512.0f;
  }
  std::memcpy(&sh[68], &threshold, 4);  // g_AlphaThreshold (c17.x)
  sh[64] = (g32(dev + kDevVsBools) & 0xFFFF) | ((g32(dev + kDevPsBools) & 0xFFFF) << 16);  // g_Booleans
  sh[65] = 0;  // g_SwappedTexcoords (16-bit components are fixed on upload)
  sh[69] = g_spec_bits;  // g_NativeSpecConstants (c17.y)
  NTRACE("  bools {:08X} alpha ref {} threshold {} sampler0 slot {} fetch d0 {:08X} d3 {:08X}", sh[64], st.alpha_ref,
         threshold, sh[48], g32(dev + 1152), g32(dev + 1152 + 12));
  if (tracing()) {
    const float* c = reinterpret_cast<const float*>(cbase + vs);  // (GoldenEye read the upload ring here)
    std::string guest;
    // (Every constant the shader reads, up to 64: the lighting capture compares them.)
    for (uint32_t i = 0; i < std::min<uint32_t>(64, std::max<uint32_t>(8, g_vs_cb_bytes / 16)); ++i) {
      float f4[4];
      for (int k = 0; k < 4; ++k) { const uint32_t u = g32(dev + kDevVsConsts + i * 16 + k * 4); std::memcpy(&f4[k], &u, 4); }
      guest += fmt::format(" c{}[{} {} {} {}]", i, f4[0], f4[1], f4[2], f4[3]);
    }
    NTRACE("  guest vs consts{} ({} bytes uploaded)", guest, g_vs_cb_bytes);
    // PA_CL_CLIP_CNTL (bits 0-5: user clip planes), plane 0 and the scissor.
    float ucp0[4];
    for (int i = 0; i < 4; ++i) { const uint32_t u = g32(dev + kDevUserClipPlanes + i * 4); std::memcpy(&ucp0[i], &u, 4); }
    NTRACE("  clip cntl {:08X} ucp0 {} {} {} {} scissor {:08X} {:08X} en {:08X}", g32(dev + kDevClipCntl), ucp0[0],
           ucp0[1], ucp0[2], ucp0[3], g32(dev + kDevWindowScissorTL), g32(dev + kDevWindowScissorBR),
           g32(dev + 12032));
    std::string pguest;
    for (uint32_t i = 0; i < std::min<uint32_t>(40, std::max<uint32_t>(8, g_ps_cb_bytes / 16)); ++i) {
      float f4[4];
      for (int k = 0; k < 4; ++k) { const uint32_t u = g32(dev + kDevPsConsts + i * 16 + k * 4); std::memcpy(&f4[k], &u, 4); }
      pguest += fmt::format(" c{}[{} {} {} {}]", i, f4[0], f4[1], f4[2], f4[3]);
    }
    NTRACE("  guest ps consts{}", pguest);
    NTRACE("  vs c0-3 [{} {} {} {}] [{} {} {} {}] [{} {} {} {}] [{} {} {} {}]", c[0], c[1], c[2], c[3], c[4], c[5], c[6],
           c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
    const float* pc = reinterpret_cast<const float*>(cbase + ps);
    NTRACE("  ps c0-1 [{} {} {} {}] [{} {} {} {}] ({} bytes)", pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7],
           g_ps_cb_bytes);
  }
  // c18: position scale / offset. PA_CL_VTE_CNTL (+10572, written by the
  // VIEWPORTENABLE setter: 0x43F on, 0x400 off). With the viewport transform off
  // the shader outputs window coordinates, mapped here to NDC for the target.
  const bool vte = (g32(dev + 10572) & 0x3F) != 0;
  // With PA_SU_VTX_CNTL's pixel centre at 0 (D3D9) the 360 puts pixel centres
  // on whole coordinates, D3D12 half a pixel on: those draws move right and
  // down by half a pixel, as XenosRecomp's g_HalfPixelOffset intends (without
  // it text lands on texel centres and comes out thinner than on Xenos).
  // Draws with the centre at 0.5 already match D3D12 and stay put, as on the
  // classic renderer (the SDK's draw util): offsetting a full-screen copy drawn
  // that way point-samples exactly between texels and drops and doubles pixel
  // columns and rows across the whole picture (found in GoldenEye, b2f09b5).
  // Half a pixel is 1 / the viewport's size in NDC: the pause menu's blur passes
  // draw into 256x256 and 128x128 viewports, and half a pixel of the game frame
  // there (1/1280 or 1/1920) left each pass sampling half a texel off, so the
  // blurred backdrop crept up and left and faded to black at its bottom edge.
  const bool half = REXCVAR_GET(gpu_native_half_pixel) && (g32(dev + kDevVtxCntl) & 1) == 0;
  g.last_draw_half_pixel = half;
  float pos[4] = {1.0f, 1.0f, half ? 1.0f / vp_w : 0.0f, half ? -1.0f / vp_h : 0.0f};
  if (!vte) { pos[0] = 2.0f / vp_w; pos[1] = -2.0f / vp_h; pos[2] = -1.0f + pos[2]; pos[3] = 1.0f + pos[3]; }
  std::memcpy(&sh[72], pos, sizeof(pos));
  std::memcpy(&sh[76], depth_flip, sizeof(depth_flip));  // g_NativeDepthFlip (c19.xy)
  // User clip planes: PA_CL_CLIP_CNTL (0x2204) bits 0-5 enable PA_CL_UCP_0..5
  // (kDevUserClipPlanes), none while clipping is off (bit 16). Perfect Dark cuts
  // an open sliding door at the wall it slides into with plane 0 (issue #25).
  // g_NativeClipMask (c19.z), planes c20-c25 (pd_native_shaders.cpp).
  const uint32_t clip_cntl = g32(dev + kDevClipCntl);
  sh[78] = (clip_cntl & 0x10000) || !REXCVAR_GET(gpu_native_clip_planes) ? 0 : clip_cntl & 0x3F;
  for (uint32_t i = 0; i < 24; ++i) sh[80 + i] = g32(dev + kDevUserClipPlanes + i * 4);  // big-endian floats
  std::memcpy(cbase + shared, sh, sizeof(sh));
  if (g_vulkan) {
    const uint64_t at = g.const_gpu[g.slot]->getDeviceAddress();
    const uint64_t addrs[3] = {at + vs, at + ps, at + shared};
    g.cmd->setGraphicsPushConstants(0, addrs, 0, sizeof(addrs));
  } else {
    g.cmd->setGraphicsRootDescriptor(RenderBufferReference(g.const_gpu[g.slot].get(), vs), 0);
    g.cmd->setGraphicsRootDescriptor(RenderBufferReference(g.const_gpu[g.slot].get(), ps), 1);
    g.cmd->setGraphicsRootDescriptor(RenderBufferReference(g.const_gpu[g.slot].get(), shared), 2);
  }
}

// --- pipelines ----------------------------------------------------------------
//
// A pipeline's first creation on a PC costs the driver's compile: 13-29 ms for
// the one to three new pipelines a level shows when it starts or a new effect
// first appears, the only hitches left in play. The driver keeps what it
// compiled in its own disk cache (a pipeline it has seen costs ~0.1 ms), so
// every pipeline's description (shader hashes and state: no game data) goes on
// a list, and at boot a worker thread creates each listed pipeline whose
// shaders are cached; the game thread's own creation then finds it in the
// driver's cache. The list is the shader cache's pipelines.bin (what this PC
// has drawn) plus native_pipelines.bin beside the exe (every level, recorded
// on the developer's PC), so a first play starts warm too.

struct PipeRecord {  // one pipeline as listed (version 1, little-endian, no padding)
  uint64_t vs = 0, ps = 0;  // shader hashes
  uint8_t topo = 0, scene = 0, samples = 1, n_els = 0;
  uint8_t z_enable = 0, z_write = 0, z_func = 0, cull = 0;  // cull: 0 none, 1 front, 2 back
  uint8_t blend = 0, src = 0, dst = 0, op = 0, src_a = 0, dst_a = 0, op_a = 0, write_mask = 0;
  uint8_t alpha_to_mask = 0, reserved[7] = {};
  struct El {
    uint32_t type = 0;
    uint8_t usage = 0, index = 0;
    int8_t exp = 0;
    uint8_t reserved = 0;
  } els[16];
};
static_assert(sizeof(PipeRecord) == 168, "PipeRecord is stored as it is");
constexpr uint32_t kPipeListMagic = 0x4C504450;  // "PDPL"
constexpr uint32_t kPipeListVersion = 1;

// What reaches the pipeline, nothing else: state the pipeline ignores (blend
// factors with blending off, the depth test with depth off) is zeroed, so it
// doesn't make a second pipeline. Shaders by hash, not by host object: a
// shader the game releases and makes again keeps its pipelines.
PipeRecord make_record(uint64_t vs, uint64_t ps, const std::vector<Element>& els, RenderPrimitiveTopology topo) {
  const DrawState& st = g_draw_state;
  PipeRecord r;
  r.vs = vs;
  r.ps = ps;
  r.topo = uint8_t(topo);
  r.scene = g.bound_scene;
  r.samples = g.bound_scene ? uint8_t(g.msaa.sampleCount) : 1;
  r.n_els = uint8_t(std::min<size_t>(els.size(), std::size(r.els)));
  for (uint32_t e = 0; e < r.n_els; ++e) {
    r.els[e].type = els[e].type;
    r.els[e].usage = els[e].usage;
    r.els[e].index = els[e].index;
    r.els[e].exp = els[e].exp;
  }
  if (r.scene && st.z_enable) { r.z_enable = 1; r.z_write = st.z_write; r.z_func = st.z_func; }
  r.cull = (st.cull & 3) == 0 ? 0 : st.cull == 2 ? 1 : 2;
  if (st.blend) {
    r.blend = 1;
    r.src = st.src; r.dst = st.dst; r.op = st.op;
    r.src_a = st.src_a; r.dst_a = st.dst_a; r.op_a = st.op_a;
  }
  r.write_mask = st.write_mask;
  r.alpha_to_mask = r.scene && st.alpha_to_mask && r.samples > 1;
  return r;
}

// The creation description for a record (inputs: storage the description points into).
RenderGraphicsPipelineDesc describe_pipeline(const PipeRecord& r, RenderShader* vs, RenderShader* ps,
                                             std::vector<RenderInputElement>& inputs, RenderInputSlot& slot) {
  inputs.clear();
  uint32_t offset = 0;
  for (uint32_t e = 0; e < r.n_els; ++e) {
    Element el{};
    el.type = r.els[e].type;
    el.usage = r.els[e].usage;
    el.index = r.els[e].index;
    el.exp = r.els[e].exp;
    const HostFmt hf = host_format(el);
    if (el.usage < std::size(kSemantics))
      inputs.emplace_back(kSemantics[el.usage], el.index, vk_location(el.usage, el.index), hf.format, 0, offset);
    offset += hf.size;
  }
  slot = RenderInputSlot(0, offset);
  RenderGraphicsPipelineDesc d;
  d.pipelineLayout = g.layout.get();
  d.vertexShader = vs;
  d.pixelShader = ps;
  d.primitiveTopology = RenderPrimitiveTopology(r.topo);
  d.renderTargetFormat[0] = kSceneFormat;
  d.multisampling = RenderMultisampling(RenderSampleCounts(r.samples));
  d.alphaToCoverageEnabled = r.alpha_to_mask;
  RenderBlendDesc blend = RenderBlendDesc::Copy();
  blend.blendEnabled = r.blend;
  blend.srcBlend = to_blend(r.src); blend.dstBlend = to_blend(r.dst); blend.blendOp = to_blend_op(r.op);
  blend.srcBlendAlpha = to_blend(r.src_a); blend.dstBlendAlpha = to_blend(r.dst_a); blend.blendOpAlpha = to_blend_op(r.op_a);
  blend.renderTargetWriteMask = r.write_mask;
  d.renderTargetBlend[0] = blend;
  d.depthEnabled = r.z_enable;
  d.depthWriteEnabled = r.z_enable && r.z_write;
  d.depthFunction = to_cmp(r.z_func);
  d.frontFace = RenderFrontFace::CLOCKWISE;
  d.cullMode = r.cull == 0 ? RenderCullMode::NONE : r.cull == 1 ? RenderCullMode::FRONT : RenderCullMode::BACK;
  d.renderTargetCount = 1;
  d.depthTargetFormat = r.scene ? RenderFormat::D32_FLOAT : RenderFormat::UNKNOWN;
  d.inputSlots = &slot;
  d.inputSlotsCount = 1;
  d.inputElements = inputs.data();
  d.inputElementsCount = uint32_t(inputs.size());
  return d;
}

struct PipeList {
  std::mutex mutex;
  std::unordered_set<uint64_t> listed;  // records in this PC's pipelines.bin
  bool loaded = false;
};
PipeList g_pipe_list;

std::wstring user_pipe_list_path() { return pd::native_shaders::CacheDir() + L"\\pipelines.bin"; }

std::vector<PipeRecord> read_pipe_list(const std::wstring& path) {
  std::vector<PipeRecord> out;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return out;
  uint32_t head[3] = {};
  if (std::fread(head, 4, 3, f) == 3 && head[0] == kPipeListMagic && head[1] == kPipeListVersion &&
      head[2] == sizeof(PipeRecord)) {
    // Records; a header again in between is skipped (the first test builds
    // wrote one before every record).
    for (;;) {
      uint32_t peek[3] = {};
      const long at = std::ftell(f);
      if (std::fread(peek, 4, 3, f) != 3) break;
      if (std::memcmp(peek, head, sizeof(head)) != 0) std::fseek(f, at, SEEK_SET);
      PipeRecord r;
      if (std::fread(&r, sizeof(r), 1, f) != 1) break;
      out.push_back(r);
    }
  }
  std::fclose(f);
  return out;
}

// Puts a pipeline the game thread just made on this PC's list (once).
void list_pipeline(const PipeRecord& r, uint64_t key) {
  std::lock_guard<std::mutex> lock(g_pipe_list.mutex);
  if (!g_pipe_list.loaded) {
    for (const PipeRecord& old : read_pipe_list(user_pipe_list_path()))
      g_pipe_list.listed.insert(XXH3_64bits(&old, sizeof(old)));
    g_pipe_list.loaded = true;
  }
  if (!g_pipe_list.listed.insert(key).second) return;
  const std::wstring path = user_pipe_list_path();
  FILE* f = _wfopen(path.c_str(), L"ab");
  if (!f) return;
  std::fseek(f, 0, SEEK_END);  // (append mode reports position 0 until the first write)
  if (std::ftell(f) == 0) {
    const uint32_t head[3] = {kPipeListMagic, kPipeListVersion, uint32_t(sizeof(PipeRecord))};
    std::fwrite(head, 4, 3, f);
  }
  std::fwrite(&r, sizeof(r), 1, f);
  std::fclose(f);
}

// The boot worker: creates every listed pipeline whose shaders are cached (and
// drops it: the driver keeps the compiled result), then again every 10 s for
// those whose shaders were not translated yet, for up to 10 minutes.
void warm_pipelines() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::vector<PipeRecord> todo = read_pipe_list(exe_dir() + L"\\native_pipelines.bin");
  const size_t shipped = todo.size();
  for (const PipeRecord& r : read_pipe_list(user_pipe_list_path())) todo.push_back(r);
  {
    // Scene pipelines at the scene's current sample count (the shipped list was
    // recorded without anti-aliasing; the Anti-Aliasing setting changes it).
    const uint8_t samples = uint8_t(g.msaa.sampleCount);
    std::unordered_set<uint64_t> seen;
    std::vector<PipeRecord> unique;
    for (PipeRecord r : todo) {
      if (r.scene) r.samples = samples;
      if (r.samples <= 1) r.alpha_to_mask = 0;
      if (seen.insert(XXH3_64bits(&r, sizeof(r))).second) unique.push_back(r);
    }
    todo = std::move(unique);
  }
  if (todo.empty()) return;
  std::unordered_map<uint64_t, std::unique_ptr<RenderShader>> shaders;
  auto shader = [&](uint64_t hash, bool vertex) -> RenderShader* {
    auto& s = shaders[hash];
    if (!s) {
      HostShader hs;
      hs.hash = hash;
      if (load_cached_shader(hs, vertex)) s = std::move(hs.shader);
    }
    return s.get();
  };
  size_t made = 0;
  double ms = 0;
  for (int pass = 0; pass < 60 && !todo.empty(); ++pass) {
    if (pass) std::this_thread::sleep_for(std::chrono::seconds(10));
    std::vector<PipeRecord> later;
    const size_t before = made;
    for (const PipeRecord& r : todo) {
      RenderShader* vs = shader(r.vs, true);
      RenderShader* ps = r.ps ? shader(r.ps, false) : nullptr;
      if (!vs || (r.ps && !ps)) {
        later.push_back(r);
        if (!vs) shaders.erase(r.vs);  // look again next pass
        if (r.ps && !ps) shaders.erase(r.ps);
        continue;
      }
      std::vector<RenderInputElement> inputs;
      RenderInputSlot slot;
      const RenderGraphicsPipelineDesc d = describe_pipeline(r, vs, ps, inputs, slot);
      const double t0 = now_ms();
      g.device->createGraphicsPipeline(d);
      ms += now_ms() - t0;
      ++made;
    }
    todo = std::move(later);
    if (made != before || pass == 0)
      REXKRNL_INFO("PDNATIVE pipeline warm-up: {} of the listed made in {:.0f} ms ({} from the shipped list), {} waiting "
                   "for their shaders", made, ms, shipped, todo.size());
  }
}

RenderPipeline* pipeline_for(uint64_t vs_hash, RenderShader* vs, uint64_t ps_hash, RenderShader* ps,
                             const std::vector<Element>& els, RenderPrimitiveTopology topo) {
  const PipeRecord r = make_record(vs_hash, ps_hash, els, topo);
  const uint64_t key = XXH3_64bits(&r, sizeof(r));
  const bool known = g.pipelines.count(key) != 0;
  auto& p = g.pipelines[key];
  if (p || known) return p.get();  // known && !p: creation failed before
  std::vector<RenderInputElement> inputs;
  RenderInputSlot slot;
  const RenderGraphicsPipelineDesc d = describe_pipeline(r, vs, ps, inputs, slot);
  if (REXCVAR_GET(gpu_native_log_creation)) {
    std::string desc;
    for (const RenderInputElement& in : inputs)
      desc += fmt::format(" {}{}:slot{}+{}:fmt{}", in.semanticName, in.semanticIndex, in.slotIndex,
                          in.alignedByteOffset, int(in.format));
    desc += fmt::format(" [slot {} stride {}]", slot.index, slot.stride);
    REXKRNL_INFO("PDNATIVE pipeline {:016X}:{}", key, desc);
  }
  const double t0 = now_ms();
  p = g.device->createGraphicsPipeline(d);
  g.pipe_ms += now_ms() - t0;
  ++g.pipe_n;
  drain_debug_messages("pipeline");
  if (p && (g_vulkan ? !static_cast<VulkanGraphicsPipeline*>(p.get())->vk
                     : !static_cast<D3D12GraphicsPipeline*>(p.get())->d3d)) {
    REXKRNL_WARN("PDNATIVE pipeline {:016X} failed to create; its draws are skipped", key);
    p.reset();
  }
  if (p) {
    // Listed with the alpha-to-coverage the game asked for, whatever the scene's
    // sample count now, so a list recorded at 1x also warms the anti-aliased
    // pipelines (warm_pipelines drops it again at 1x).
    PipeRecord listed = r;
    listed.alpha_to_mask = r.scene && g_draw_state.alpha_to_mask;
    list_pipeline(listed, XXH3_64bits(&listed, sizeof(listed)));
  }
  return p.get();
}

// One draw. indices == nullptr: non-indexed, `count` vertices from `first`.
// up_data != 0: vertices come from guest memory directly (DrawVerticesUP /
// Begin/EndVertices) with stride up_stride, as stream 0.
// idx_addr != 0: indexed draw, `count` indices from guest memory at idx_addr
// (32-bit when idx_wide, else 16-bit), read only when the cached list is stale.
// Applies a resolve's deferred clears to the scene target (bound here).
void apply_pending_clears() {
  if (!g.pending_clear_color && !g.pending_clear_depth) return;
  g.cmd->barriers(RenderBarrierStage::GRAPHICS,
                  {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::COLOR_WRITE),
                   RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::DEPTH_WRITE)});
  g.cmd->setFramebuffer(g.scene_fb.get());
  if (g.pending_clear_color) g.cmd->clearColor(0, g.pending_color);
  if (g.pending_clear_depth) {
    depth_will_change();
    g.cmd->clearDepthStencil(true, true, g.pending_depth, 0);
    g.depth_clear = g.pending_depth;
    g.depth_written = false;
    ++g.depth_gen;
  }
  g.pending_clear_color = g.pending_clear_depth = false;
  g.bound_fb = nullptr;  // bind whatever the game has set again
  bind_target();
  NTRACE("  deferred resolve clears applied");
}

void native_draw(uint32_t dev, uint32_t prim, uint32_t count, const uint32_t* indices,
                 int32_t base_vertex, uint32_t first, uint32_t up_data, uint32_t up_stride,
                 bool restart_16 = true, uint32_t idx_addr = 0, bool idx_wide = false) {
  if (!ensure_device() || count == 0) return;
  begin_frame();
  const double t_start = now_ms();
  struct Total {
    double t0;
    ~Total() { g.sect[4] += now_ms() - t0; }
  } total{t_start};
  const uint32_t vs_obj = g32(dev + kDevVertexShader);
  RenderShader* vs = host_shader(vs_obj, true);
  RenderShader* ps = host_shader(g32(dev + kDevPixelShader), false);
  if (!vs || !ps) {
    NTRACE("skip prim {} n {}: vs {:08X} {} ps {:08X} {}", prim, count, vs_obj, vs != nullptr,
           g32(dev + kDevPixelShader), ps != nullptr);
    if (tracing() && !ps) {
      const uint32_t o = g32(dev + kDevPixelShader);
      std::string w;
      for (uint32_t i = 0; i < 24; ++i) w += fmt::format(" {:08X}", g32(o + i * 4));
      NTRACE("ps object{}", w);
    }
    ++g.skipped; ++g.skip_reason[0]; return;
  }
  {  // gpu_native_hide_vs: leave out draws with that vertex shader (debugging; read every draw)
    const std::string hide_vs = REXCVAR_GET(gpu_native_hide_vs);
    if (!hide_vs.empty() && fmt::format("{:016X}", g.shaders[vs_obj].hash) == hide_vs) return;
  }
  {  // gpu_native_skip_ps: leave out draws with that pixel shader (debugging)
    static const std::string skip = REXCVAR_GET(gpu_native_skip_ps);
    if (!skip.empty() && fmt::format("{:016X}", g.shaders[g32(dev + kDevPixelShader)].hash) == skip) return;
  }
  HostShader& vsh = g.shaders[vs_obj];
  if (!vsh.container.empty()) {
    const uint32_t shader_off = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(&vsh.container[24]));
    const uint32_t phys_off = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(&vsh.container[shader_off]));
    const uint32_t live = gpu_view(g32(vs_obj + 32));
    if (live) parse_vertex_inputs(vsh.container, vsh, live + phys_off);
  }
  // A shader whose vertex fetches are still templates (format 0: D3D fills them
  // in for the bound declaration, which in hung mode it doesn't do; Perfect
  // Dark's quad and sprite shader) takes its layout from the declaration.
  const bool templ = std::any_of(vsh.inputs.begin(), vsh.inputs.end(),
                                 [](const Element& el) { return (el.type & 0x3F) == 0; });
  const bool use_decl = g32(dev + kDevVertexDecl) && (templ || REXCVAR_GET(gpu_native_prefer_decl));
  const bool from_shader = !vsh.inputs.empty() && !use_decl;
  const double t_parsed = now_ms();
  g.sect[0] += t_parsed - t_start;
  const uint32_t decl = from_shader ? 0 : g32(dev + kDevVertexDecl);
  std::vector<Element> els = from_shader ? vsh.inputs : read_decl(decl);
  if (els.empty()) {
    static int logged = 0;
    if (logged++ < 4)
      REXKRNL_WARN("PDNATIVE empty decl {:08X}: +0 {:08X} +24 {:08X} +28 {:08X} +52 {:08X} {:08X} {:08X}", decl,
                   decl ? g32(decl) : 0, decl ? g32(decl + 24) : 0, decl ? g32(decl + 28) : 0,
                   decl ? g32(decl + 52) : 0, decl ? g32(decl + 56) : 0, decl ? g32(decl + 60) : 0);
    ++g.skipped; ++g.skip_reason[1]; return;
  }

  // Index list (rebased to the vertex range actually used); fans -> lists.
  // Indexed draws keep the converted list on the GPU between frames, checked
  // against an xxHash of the guest indices.
  Native::IndexCacheEntry* ientry = nullptr;
  uint64_t icontent = 0;
  if (idx_addr) {
    const uint32_t bytes = count * (idx_wide ? 4 : 2);
    if (!hash_bytes_seh(idx_addr, bytes, &icontent)) { ++g.skipped; ++g.skip_reason[3]; return; }
    const uint64_t key = XXH3_64bits_withSeed(&idx_addr, 4, (uint64_t(count) << 32) ^ (uint64_t(prim) << 8) ^ idx_wide) ^
                         uint64_t(uint32_t(base_vertex)) * 0x9E3779B97F4A7C15ull;
    ientry = &g.index_cache[key];
    ientry->last_frame = g.frames;
  }
  // A traced frame decodes every draw afresh (the geometry log below needs the indices and vertices).
  const std::string hide = REXCVAR_GET(gpu_native_hide);  // test: see gpu_native_hide
  if (prim == 8) ientry = nullptr;  // rect lists: made-up corners, not cached
  const bool ihit = !tracing() && hide.empty() && ientry && ientry->buffer && ientry->content == icontent;
  std::vector<uint32_t> idx;
  // Rect lists (primitive 8): three corners per rectangle, the GPU makes the
  // fourth (v1 + v2 - v0). Each rect becomes two triangles whose fourth corner
  // is an extra vertex after the decoded ones (index kRectCorner + n until the
  // indices are rebased). Perfect Dark draws its Perfect Menu blur this way.
  constexpr uint32_t kRectCorner = 0xF0000000u;
  std::vector<uint32_t> rect_src;  // per rect: its three corners' indices
  uint32_t rects = 0;
  std::vector<uint32_t> hidden_tris;  // gpu_native_hide's triangles, traced below
  int64_t lo = 0;
  uint32_t vcount = 0, icount = 0;
  RenderPrimitiveTopology topo = RenderPrimitiveTopology::TRIANGLE_LIST;
  if (ihit) {
    lo = ientry->lo; vcount = ientry->vcount; icount = ientry->count; topo = ientry->topo;
  } else {
  if (idx_addr) {
    idx.resize(count);
    for (uint32_t i = 0; i < count; ++i) idx[i] = idx_wide ? g32(idx_addr + i * 4) : g16(idx_addr + i * 2);
  } else if (indices) idx.assign(indices, indices + count);
  else for (uint32_t i = 0; i < count; ++i) idx.push_back(first + i);
  // Everything becomes a triangle (or point/line) list. Index 0xFFFF / 0xFFFFFFFF
  // is the primitive-restart cut (D3DRS_PRIMITIVERESETINDEX): strips and fans
  // restart there, list triangles touching it are dropped.
  const uint32_t restart = ((indices || idx_addr) && restart_16) ? 0xFFFFu : 0xFFFFFFFFu;
  std::vector<uint32_t> list;
  switch (prim) {
    case 4:
      for (size_t i = 0; i + 2 < idx.size(); i += 3)
        if (idx[i] != restart && idx[i + 1] != restart && idx[i + 2] != restart)
          list.insert(list.end(), {idx[i], idx[i + 1], idx[i + 2]});
      break;
    case 5: case 6: {  // fan / strip, split at restart
      size_t start = 0;
      for (size_t i = 0; i <= idx.size(); ++i) {
        if (i < idx.size() && idx[i] != restart) continue;
        for (size_t k = start; k + 2 < i; ++k) {
          if (prim == 5) list.insert(list.end(), {idx[start], idx[k + 1], idx[k + 2]});
          else if (((k - start) & 1) == 0) list.insert(list.end(), {idx[k], idx[k + 1], idx[k + 2]});
          else list.insert(list.end(), {idx[k + 1], idx[k], idx[k + 2]});
        }
        start = i + 1;
      }
      break;
    }
    case 13:  // quad list
      // Split along the 1-3 diagonal, as the classic renderer does (its quad
      // geometry shader emits the strip 0, 1, 3, 2), so a quad that isn't flat
      // or evenly lit shades the same in both (GoldenEye d28d81b).
      for (size_t i = 0; i + 3 < idx.size(); i += 4)
        list.insert(list.end(), {idx[i], idx[i + 1], idx[i + 3], idx[i + 1], idx[i + 2], idx[i + 3]});
      break;
    case 8:  // rect list
      for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const uint32_t d = kRectCorner + rects++;
        rect_src.insert(rect_src.end(), {idx[i], idx[i + 1], idx[i + 2]});
        list.insert(list.end(), {idx[i], idx[i + 1], idx[i + 2], idx[i + 2], idx[i + 1], d});
      }
      break;
    case 1: topo = RenderPrimitiveTopology::POINT_LIST; list = idx; break;
    case 2: topo = RenderPrimitiveTopology::LINE_LIST; list = idx; break;
    case 3: topo = RenderPrimitiveTopology::LINE_STRIP; list = idx; break;
    default: ++g.skipped; ++g.skip_reason[2]; return;
  }
  idx.swap(list);
  if (idx.empty()) return;
  if (!hide.empty()) {  // gpu_native_hide: "a-b" draws, or "d:a-b" triangles of draw d
    unsigned d = 0, a = 0, b = 0;
    if (std::sscanf(hide.c_str(), "%u:%u-%u", &d, &a, &b) == 3) {
      if (g.draws == d && prim != 1 && prim != 2 && prim != 3) {
        std::vector<uint32_t> kept;
        hidden_tris.clear();
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
          if (t / 3 < a || t / 3 > b) kept.insert(kept.end(), {idx[t], idx[t + 1], idx[t + 2]});
          else hidden_tris.insert(hidden_tris.end(), {idx[t], idx[t + 1], idx[t + 2]});
        if (kept.empty()) { ++g.draws; return; }
        idx.swap(kept);
      }
    } else if (std::sscanf(hide.c_str(), "%u-%u", &a, &b) == 2 && g.draws >= a && g.draws <= b) {
      ++g.draws;
      return;
    }
  }
  lo = INT64_MAX;
  int64_t hi = INT64_MIN;
  for (uint32_t i : idx) {
    if (rects && i >= kRectCorner) continue;
    lo = std::min<int64_t>(lo, int64_t(i) + base_vertex);
    hi = std::max<int64_t>(hi, int64_t(i) + base_vertex);
  }
  if (lo < 0 || hi - lo > 1'000'000) { ++g.skipped; ++g.skip_reason[3]; return; }
  vcount = uint32_t(hi - lo + 1);
  icount = uint32_t(idx.size());
  }
  const double t_indexed = now_ms();
  g.sect[1] += t_indexed - t_parsed;

  // Sources per fetch slot / stream. The shader path reads the device's vertex
  // fetch constants (device+1152 + slot*8: physical address | type, size |
  // endian); fetch constants hold physical addresses, which the CPU sees at
  // 0xA0000000+. The declaration path uses SetStreamSource; UP draws read
  // up_data. Everything is decoded into one interleaved host stream.
  std::vector<uint16_t> keys;
  std::vector<Source> sources;
  for (const Element& el : els) {
    if (std::find(keys.begin(), keys.end(), el.stream) != keys.end()) continue;
    keys.push_back(el.stream);
    Source src;
    if (up_data) { src.addr = up_data; src.stride = up_stride; src.endian = 2 | 4; }
    else if (from_shader) {
      const uint32_t d0 = g32(dev + 1152 + el.stream * 8), d1 = g32(dev + 1152 + el.stream * 8 + 4);
      const uint32_t phys = d0 & 0x1FFFFFFCu;
      src.endian = d1 & 3;
      const auto st = vsh.strides.find(el.stream);
      src.stride = st != vsh.strides.end() ? st->second : 0;
      // A vfetch with no stride (Perfect Dark's quad shader, whose microcode D3D
      // leaves as the template here): D3D stream s is fetch slot 95 - s, so the
      // stride comes from SetStreamSource.
      if (!src.stride && el.stream >= 80 && el.stream <= 95 && g_streams[95 - el.stream].vb)
        src.stride = g_streams[95 - el.stream].stride;
      src.addr = cpu_view_of_physical(phys, (uint32_t(lo) + vcount) * src.stride);
    } else if (el.stream < 16 && g_streams[el.stream].vb) {
      const uint32_t vb = g_streams[el.stream].vb;
      // (0xE0000000+ buffers: the data is 4 KB on, as for index buffers.)
      src.addr = gpu_view(g32(vb + 24) & ~3u) + g_streams[el.stream].offset;
      src.stride = g_streams[el.stream].stride;
      src.endian = g32(vb + 28) & 3;
    }
    if (!src.addr || !src.stride) {
      NTRACE("skip prim {} n {}: no source for stream {} ({}; vs {:016X} decl {:08X}; fetch {:08X} {:08X}; vb {:08X} "
             "stride {})", prim, count, el.stream, up_data ? "UP" : from_shader ? "fetch" : "decl", vsh.hash, decl,
             g32(dev + 1152 + el.stream * 8), g32(dev + 1152 + el.stream * 8 + 4),
             el.stream < 16 ? g_streams[el.stream].vb : 0, src.stride);
      ++g.skipped; ++g.skip_reason[4]; return;
    }
    sources.push_back(src);
  }
  std::vector<HostFmt> hfmts;
  std::vector<uint32_t> hoffsets;
  uint32_t host_stride = 0;
  for (const Element& el : els) {
    hfmts.push_back(host_format(el));
    hoffsets.push_back(host_stride);
    host_stride += hfmts.back().size;
  }
  // Textures first (their uploads may flush the ring), then room for this
  // draw's vertices, indices and constants in one go.
  for (uint32_t s = 0; s < 16; ++s) texture_slot(g_textures[s], true);
  for (uint32_t s = 0; s < 16; ++s)
    if (texture_pending(g_textures[s])) { ++g.skipped; ++g.skip_reason[6]; return; }
  ensure_room(uint64_t(host_stride) * vcount + uint64_t(icount) * 4 + 16384);
  // Static geometry is decoded once and kept in a GPU buffer, found again by
  // the draw's layout and source range and checked against an xxHash of the
  // guest bytes (decoding every draw every frame cost ~47 ms a frame in Dam).
  // Data that keeps changing, and UP draws, go through the upload ring.
  const uint64_t vbytes = uint64_t(host_stride) * (vcount + rects);
  uint64_t layout = XXH3_64bits(sources.data(), sources.size() * sizeof(Source));
  for (const Element& el : els)  // field by field: Element has a padding byte
    layout = layout * 0x100000001B3ull ^ (uint64_t(el.stream) | uint64_t(el.offset) << 16 | uint64_t(el.type) << 32) ^
             (uint64_t(el.usage) << 8 | el.index | uint64_t(uint8_t(el.exp)) << 16 | uint64_t(el.swizzle) << 24) *
                 0xC2B2AE3D27D4EB4Full;
  layout ^= (uint64_t(lo) << 32 | vcount) * 0x9E3779B97F4A7C15ull;
  Native::VertexCacheEntry* entry = up_data || rects ? nullptr : &g.vertex_cache[layout];
  uint64_t content = 0;
  const double t_h = now_ms();
  const bool hashed = !entry || hash_sources_seh(sources.data(), sources.size(), uint32_t(lo), vcount, &content);
  g.hash_ms += now_ms() - t_h;
  if (!hashed) {
    ++g.skipped; ++g.skip_reason[4]; return;
  }
  RenderBufferReference vref;
  const bool hit = !tracing() && entry && entry->buffer && entry->content == content;
  if (entry) entry->last_frame = g.frames;
  if (hit) ++g.vcache_hits; else ++g.vcache_misses;
  const uint64_t voff = hit ? 0 : upload_alloc(vbytes, 16);
  if (voff == UINT64_MAX) return;
  const double t_vd = now_ms();
  const bool decoded = hit || decode_vertices_seh(els.data(), els.size(), sources.data(), keys.data(), keys.size(),
                                                  uint32_t(lo), vcount, g.upload_ptr + voff, host_stride,
                                                  hfmts.data(), hoffsets.data());
  g.vdecode_ms += now_ms() - t_vd;
  if (!decoded) {
    static int logged = 0;
    if (logged++ < 8)
      REXKRNL_WARN("PDNATIVE vertex read faulted: {} sources, first {:08X} stride {} endian {}, vertices {}..{} ({})",
                   sources.size(), sources[0].addr, sources[0].stride, sources[0].endian, lo, lo + vcount - 1,
                   up_data ? "UP" : from_shader ? "fetch" : "decl");
    ++g.skipped; ++g.skip_reason[4];
    return;
  }
  size_t rect_pos = els.size();  // the position element (for the right-angle corner)
  for (size_t e = 0; e < els.size(); ++e)
    if (els[e].usage == 0 && hfmts[e].kind != HostKind::kRaw && hfmts[e].size >= 8 && rect_pos == els.size()) rect_pos = e;
  for (uint32_t r = 0; r < rects; ++r) {
    uint8_t* base = g.upload_ptr + voff;
    uint32_t v[3] = {rect_src[r * 3], rect_src[r * 3 + 1], rect_src[r * 3 + 2]};
    // The corner opposite the longest edge (the diagonal) is the right angle,
    // as xenia finds it; the fourth corner mirrors it across the diagonal.
    if (rect_pos < els.size()) {
      auto xy = [&](uint32_t i, float* f) {
        std::memcpy(f, base + uint64_t(int64_t(i) + base_vertex - lo) * host_stride + hoffsets[rect_pos], 8);
      };
      float p0[2], p1[2], p2[2];
      xy(v[0], p0); xy(v[1], p1); xy(v[2], p2);
      auto d2 = [](const float* x, const float* y) { return (x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]); };
      const float e01 = d2(p0, p1), e02 = d2(p0, p2), e12 = d2(p1, p2);
      if (e02 > e12 && e02 > e01) std::swap(v[0], v[1]);       // corner at v1
      else if (e01 > e12 && e01 > e02) std::swap(v[0], v[2]);  // corner at v2
      idx[r * 6 + 3] = v[2];
      idx[r * 6 + 4] = v[1];
    }
    const uint8_t* a = base + uint64_t(int64_t(v[0]) + base_vertex - lo) * host_stride;
    const uint8_t* b = base + uint64_t(int64_t(v[1]) + base_vertex - lo) * host_stride;
    const uint8_t* c = base + uint64_t(int64_t(v[2]) + base_vertex - lo) * host_stride;
    uint8_t* d = base + uint64_t(vcount + r) * host_stride;
    for (size_t e = 0; e < els.size(); ++e) {
      const uint32_t o = hoffsets[e], n = hfmts[e].size;
      const HostKind k = hfmts[e].kind;
      if ((k == HostKind::kF32 || k == HostKind::kRecord) && !uint_usage(els[e].usage)) {
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
          float fa, fb, fc;
          std::memcpy(&fa, a + o + i, 4); std::memcpy(&fb, b + o + i, 4); std::memcpy(&fc, c + o + i, 4);
          const float fd = fb + fc - fa;
          std::memcpy(d + o + i, &fd, 4);
        }
      } else if (k == HostKind::kU8x4) {
        for (uint32_t i = 0; i < 4; ++i) d[o + i] = uint8_t(std::clamp(int(b[o + i]) + int(c[o + i]) - int(a[o + i]), 0, 255));
      } else {
        std::memcpy(d + o, c + o, n);  // (integers and half floats: the third corner's)
      }
    }
  }
  if (hit) {
    vref = RenderBufferReference(entry->buffer.buffer, entry->buffer.offset);
  } else if (entry && (!entry->buffer || entry->misses < 4)) {
    if (entry->buffer) ++entry->misses;
    if (!entry->buffer || entry->capacity < vbytes) {
      retire(g.vertex_pool, entry->buffer);
      entry->buffer = pool_alloc(g.vertex_pool, vbytes);
      entry->capacity = class_bytes(entry->buffer.size_class);
    }
    g.cmd->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(entry->buffer.buffer, RenderBufferAccess::WRITE));
    g.cmd->copyBufferRegion(RenderBufferReference(entry->buffer.buffer, entry->buffer.offset),
                            RenderBufferReference(g.upload.get(), voff), vbytes);
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(entry->buffer.buffer, RenderBufferAccess::READ));
    entry->content = content;
    vref = RenderBufferReference(entry->buffer.buffer, entry->buffer.offset);
  } else {
    vref = RenderBufferReference(g.upload.get(), voff);
  }
  const double t_decoded = now_ms();
  g.sect[2] += t_decoded - t_indexed;
  std::vector<RenderInputSlot> slots{RenderInputSlot(0, host_stride)};
  std::vector<RenderVertexBufferView> views{RenderVertexBufferView(vref, uint32_t(vbytes))};
  RenderBufferReference iref;
  if (ihit) {
    iref = RenderBufferReference(ientry->buffer.buffer, ientry->buffer.offset);
  } else {
    auto rebase = [&](uint32_t i) {
      return rects && i >= kRectCorner ? int64_t(vcount + (i - kRectCorner)) : int64_t(i) + base_vertex - lo;
    };
    const uint64_t ibytes = uint64_t(icount) * (vcount + rects < 0xFFFF ? 2 : 4);
    const uint64_t ioff = upload_alloc(ibytes, 16);
    if (ioff == UINT64_MAX) return;
    if (vcount + rects < 0xFFFF) {  // rebased indices fit 16 bits: half the size and bandwidth
      uint16_t* id = reinterpret_cast<uint16_t*>(g.upload_ptr + ioff);
      for (size_t i = 0; i < idx.size(); ++i) id[i] = uint16_t(rebase(idx[i]));
    } else {
      uint32_t* id = reinterpret_cast<uint32_t*>(g.upload_ptr + ioff);
      for (size_t i = 0; i < idx.size(); ++i) id[i] = uint32_t(rebase(idx[i]));
    }
    if (ientry && (!ientry->buffer || ientry->misses < 4)) {
      if (ientry->buffer) ++ientry->misses;
      if (!ientry->buffer || ientry->capacity < ibytes) {
        retire(g.index_pool, ientry->buffer);
        ientry->buffer = pool_alloc(g.index_pool, ibytes);
        ientry->capacity = class_bytes(ientry->buffer.size_class);
      }
      g.cmd->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(ientry->buffer.buffer, RenderBufferAccess::WRITE));
      g.cmd->copyBufferRegion(RenderBufferReference(ientry->buffer.buffer, ientry->buffer.offset),
                              RenderBufferReference(g.upload.get(), ioff), ibytes);
      g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(ientry->buffer.buffer, RenderBufferAccess::READ));
      ientry->content = icontent; ientry->lo = lo; ientry->vcount = vcount; ientry->count = icount; ientry->topo = topo;
      iref = RenderBufferReference(ientry->buffer.buffer, ientry->buffer.offset);
    } else {
      iref = RenderBufferReference(g.upload.get(), ioff);
    }
  }

  g_spec_bits = 2;
  for (const Element& el : els) {
    uint32_t cb = 4;
    (void)cb;
    if ((el.usage == 3 || el.usage == 6 || el.usage == 7) && ((el.type & 0x3F) == 16 || (el.type & 0x3F) == 17))
      g_spec_bits |= 1;
  }
  g_draw_state = read_state(dev);
  bind_target();
  if (g.bound_scene) apply_pending_clears();
  g.draws_since_resolve = true;
  if (g.bound_scene && g_draw_state.z_enable && g_draw_state.z_write) {
    depth_will_change();
    g.depth_written = true;
    ++g.depth_gen;
  }
  RenderPipeline* pipe = pipeline_for(g.shaders[vs_obj].hash, vs, ps ? g.shaders[g32(dev + kDevPixelShader)].hash : 0,
                                      ps, els, topo);
  if (!pipe) { ++g.skipped; ++g.skip_reason[5]; return; }
  g_vs_cb_bytes = g.shaders[vs_obj].cb_bytes;
  g_ps_cb_bytes = g.shaders[g32(dev + kDevPixelShader)].cb_bytes;
  set_common_state(dev);
  if (tracing()) {
    const DrawState& st = g_draw_state;
    std::string tex;
    for (uint32_t t = 0; t < 8; ++t)
      if (g_textures[t]) tex += fmt::format(" s{}={:08X}->{} sign {:02X} fmt {}", t, g_textures[t], texture_slot(g_textures[t]), (g32(dev + 1152 + t * 24) >> 2) & 0xFF, g32(dev + 1152 + t * 24 + 4) & 0x3F);
    NTRACE("draw#{} prim {} n {} verts {} vs {:016X} ps {:016X} z {}{} f{} cull {} blend {} {}/{}/{} mask {:X} at {} {}{}",
           g.draws, prim, count, vcount, g.shaders[vs_obj].hash, g.shaders[g32(dev + kDevPixelShader)].hash,
           st.z_enable, st.z_write ? "w" : "", st.z_func, st.cull, st.blend, st.src, st.dst, st.op, st.write_mask,
           st.alpha_test, st.alpha_func, tex);
    float vp[6];
    for (int i = 0; i < 6; ++i) { const uint32_t u = g32(dev + kDevViewport + i * 4); std::memcpy(&vp[i], &u, 4); }
    const float* p0 = reinterpret_cast<const float*>(g.upload_ptr + voff);
    NTRACE("  vte {:X} viewport {} {} {} {} {} {} pos0 {} {} {} {} (el0 usage {} fmt {})", g32(dev + 10572), vp[0], vp[1],
           vp[2], vp[3], vp[4], vp[5], p0[0], p0[1], p0[2], p0[3], els[0].usage, els[0].type & 0x3F);
    // PA_SU_SC_MODE_CNTL (bits 11/12: polygon offset front/back; Perfect Dark
    // has not been seen using them).
    NTRACE("  mode {:08X}", g32(dev + 10568));
    // The register shadow around PA_SU_VTX_CNTL (kDevVtxCntl): the viewport
    // scale (half the target size on a full-screen draw), the line / AA config
    // words before it (AA non-zero only in an MSAA pass), the guard band floats
    // after it and the copy registers; how kDevVtxCntl was checked.
    float vps[2], gb[4];
    for (int i = 0; i < 2; ++i) { const uint32_t u = g32(dev + kDevVportXScale + i * 8); std::memcpy(&vps[i], &u, 4); }
    for (int i = 0; i < 4; ++i) { const uint32_t u = g32(dev + kDevVtxCntl + 4 + i * 4); std::memcpy(&gb[i], &u, 4); }
    NTRACE("  vport scale {} {} | line {:X} aa {:X} vtx_cntl {:X} gb {} {} {} {} | copy {:X} {:08X}", vps[0], vps[1],
           g32(dev + kDevVtxCntl - 8), g32(dev + kDevVtxCntl - 4), g32(dev + kDevVtxCntl), gb[0], gb[1], gb[2], gb[3],
           g32(dev + kDevVtxCntl + 0x16 * 4), g32(dev + kDevVtxCntl + 0x17 * 4));
    std::string e;
    for (size_t i = 0; i < els.size() * std::min<size_t>(vcount, 4); ++i) {
      const float* v = p0 + i * 4;
      if (i % els.size() == 0 && i) e += " |";
      const Element& el = els[i % els.size()];
      e += fmt::format(" [u{}.{} t{:X} exp{} off{} = {:.3f} {:.3f} {:.3f} {:.3f}]", el.usage, el.index,
                       el.type, int(el.exp), el.offset, v[0], v[1], v[2], v[3]);
    }
    NTRACE("  elements{} {}", e, from_shader ? "fetch" : "decl");
    // Every vertex's first element and the triangle list, for small draws (issue
    // #25: the door's and the wall's depth worked out at the same pixel).
    if (vcount <= 64 && !hoffsets.empty()) {
      std::string vs_str;
      for (uint32_t i = 0; i < vcount; ++i) {
        const float* v = reinterpret_cast<const float*>(g.upload_ptr + voff + uint64_t(i) * host_stride + hoffsets[0]);
        vs_str += fmt::format(" ({:.9g} {:.9g} {:.9g} {:.9g})", v[0], v[1], v[2], v[3]);
      }
      NTRACE("  allverts{}", vs_str);
      if (!idx.empty() && idx.size() <= 192) {
        std::string is;
        for (uint32_t i : idx) is += fmt::format(" {}", i);
        NTRACE("  allidx lo {}:{}", lo, is);
      }
    }
    std::string raw;
    for (uint32_t i = 0; !sources.empty() && i < 8; ++i)
      raw += fmt::format(" {:08X}", g32(sources[0].addr + uint32_t(lo) * sources[0].stride + i * 4));
    NTRACE("  source0 {:08X} stride {} endian {} {} first vertex:{}", sources[0].addr, sources[0].stride,
           sources[0].endian, up_data ? "UP" : "", raw);
    // Stretched triangles (one edge far longer than the draw's usual edge):
    // a vertex out of place in the data, e.g. a spike across the sky.
    size_t pe = els.size();
    for (size_t e = 0; e < els.size(); ++e)
      if (els[e].usage == 0 && hfmts[e].size >= 12 && pe == els.size()) pe = e;
    // Triangles hidden by gpu_native_hide, with their vertices' guest addresses.
    if (pe < els.size() && !sources.empty())
      for (size_t t = 0; t + 2 < hidden_tris.size(); t += 3) {
        uint32_t src_addr = 0, src_stride = 0;
        for (size_t k = 0; k < keys.size(); ++k)
          if (keys[k] == els[pe].stream) { src_addr = sources[k].addr; src_stride = sources[k].stride; }
        std::string vs;
        for (int k = 0; k < 3; ++k) {
          const uint32_t gv = uint32_t(int64_t(hidden_tris[t + k]) + base_vertex);
          const uint32_t ga = src_addr ? src_addr + gv * src_stride + els[pe].offset : 0;
          vs += fmt::format(" v{} at {:08X}: {:08X} {:08X} {:08X}", gv, ga, ga ? g32(ga) : 0, ga ? g32(ga + 4) : 0,
                            ga ? g32(ga + 8) : 0);
        }
        NTRACE("  HIDDEN tri of draw#{}:{}", g.draws, vs);
      }
    if (pe < els.size() && topo == RenderPrimitiveTopology::TRIANGLE_LIST && idx.size() >= 3 && !rects) {
      auto pos = [&](uint32_t i) {
        const float* f = reinterpret_cast<const float*>(g.upload_ptr + voff +
                                                        uint64_t(int64_t(i) + base_vertex - lo) * host_stride +
                                                        hoffsets[pe]);
        return std::array<float, 3>{f[0], f[1], f[2]};
      };
      auto dist = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
      };
      std::vector<float> edges;
      for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int k = 0; k < 3; ++k) edges.push_back(dist(pos(idx[t + k]), pos(idx[t + (k + 1) % 3])));
      std::vector<float> sorted = edges;
      std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
      const float median = std::max(sorted[sorted.size() / 2], 1e-3f);
      int reported = 0;
      uint32_t src_addr = 0, src_stride = 0;
      for (size_t k = 0; k < keys.size(); ++k)
        if (keys[k] == els[pe].stream) { src_addr = sources[k].addr; src_stride = sources[k].stride; }
      for (size_t t = 0; t + 2 < idx.size() && reported < 8; t += 3) {
        const float m = std::max({edges[t], edges[t + 1], edges[t + 2]});
        if (m < median * 8.0f) continue;
        ++reported;
        std::string vs;
        for (int k = 0; k < 3; ++k) {
          const auto q = pos(idx[t + k]);
          const uint32_t gv = uint32_t(int64_t(idx[t + k]) + base_vertex);
          const uint32_t ga = src_addr ? src_addr + gv * src_stride + els[pe].offset : 0;
          vs += fmt::format(" v{} ({:.1f} {:.1f} {:.1f}) at {:08X}: {:08X} {:08X} {:08X}", gv, q[0], q[1], q[2], ga,
                            ga ? g32(ga) : 0, ga ? g32(ga + 4) : 0, ga ? g32(ga + 8) : 0);
        }
        NTRACE("  STRETCHED tri {} of draw#{}: longest edge {:.1f} vs median {:.1f}, fmt {:X}:{}", t / 3, g.draws, m,
               median, els[pe].type, vs);
      }
    }
  }
  g.cmd->setPipeline(pipe);
  g.cmd->setVertexBuffers(0, views.data(), uint32_t(views.size()), slots.data());
  const bool narrow = vcount + rects < 0xFFFF;
  RenderIndexBufferView ib(iref, icount * (narrow ? 2 : 4), narrow ? RenderFormat::R16_UINT : RenderFormat::R32_UINT);
  g.cmd->setIndexBuffer(&ib);
  const bool vk_query = g_vulkan && vk_query_before_draw();
  g.cmd->drawIndexedInstanced(icount, 1, 0, 0, 0);
  if (vk_query) vk_query_after_draw();
  if (g.prof_active)
    prof_mark(fmt::format("draw#{} prim {} idx {} verts {} vs {:016X} ps {:016X} blend {}", g.draws, prim, icount, vcount,
                          g.shaders[vs_obj].hash, g.shaders[g32(dev + kDevPixelShader)].hash, g_draw_state.blend));
  ++g.draws;
}

// Scene -> a single-sampled texture of the same size and format: a copy, or
// a resolve (sample average) when the scene is multisampled.
void copy_scene_to(RenderTexture* dst) {
  if (g.msaa.sampleCount == RenderSampleCount::COUNT_1) {
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::COPY_SOURCE),
                                               RenderTextureBarrier(dst, RenderTextureLayout::COPY_DEST)});
    g.cmd->copyTexture(dst, g.scene.get());
  } else {
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(g.scene.get(), RenderTextureLayout::RESOLVE_SOURCE),
                                               RenderTextureBarrier(dst, RenderTextureLayout::RESOLVE_DEST)});
    g.cmd->resolveTexture(dst, g.scene.get());
  }
}

// Paced presents (gpu_native_pace): like the SDK presenter's pacing, each frame
// is presented at a fixed moment instead of whenever the game thread gets to
// Swap. Without a GPU plugin there is no guest vblank tick to anchor to, so the
// moments run on the game's own frame period (smoothed from Swap arrivals) and
// sit gpu_native_pace_margin_us after the typical arrival; a slow phase lock
// keeps them there. Frames arriving after their moment present at once (and
// the schedule re-anchors after a real hitch).
void precise_wait_until(double target_ms) {
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  const double ahead = 0.7;
  const double now = now_ms();
  if (timer && target_ms - now > ahead) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t((target_ms - now - ahead) * 10000.0);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
  }
  while (now_ms() < target_ms) std::this_thread::yield();
}

void pace_present() {
  const double now = now_ms();
  if (g.pace_last_arrival > 0) {
    const double iv = now - g.pace_last_arrival;
    if (iv > 12.0 && iv < 22.0) g.pace_period += (iv - g.pace_period) * 0.01;
  }
  g.pace_last_arrival = now;
  if (!REXCVAR_GET(gpu_native_pace)) { g.pace_waited_ms = 0; return; }
  const double margin = REXCVAR_GET(gpu_native_pace_margin_us) / 1000.0;
  double target = g.pace_next;
  if (target <= 0 || now > target + g.pace_period * 0.5 || target - now > g.pace_period * 1.5) {
    target = now + margin;  // (re)anchor
  } else {
    target += (now + margin - target) * 0.02;  // keep the moment `margin` after the usual arrival
  }
  g.pace_waited_ms = std::max(0.0, target - now);
  if (target > now) precise_wait_until(target);
  g.pace_next = std::max(target, now) + g.pace_period;
}

// Present-to-present intervals and the game's sim steps per frame, for the
// frame stats line (double steps as in the 0.9.6c "ge frames" line).
void note_present() {
  const double now = now_ms();
  if (g.pace_last_present > 0) g.present_intervals.push_back(now - g.pace_last_present);
  g.pace_last_present = now;
  g.pace_wait_sum += g.pace_waited_ms;
  // (GoldenEye also counted its double sim steps here, from its own frame
  // counter; Perfect Dark's are in the pd_frame_stats line.)
}

void present_frame() {
  if (!ensure_device()) return;
  NTRACE("---- end of frame {} ({} draws)", g.frames + 1, g.draws);
  if (REXCVAR_GET(gpu_native_trace_frame) == -1)  // the test pipe's TRACE: the next frame
    rex::cvar::SetFlagByName("gpu_native_trace_frame", std::to_string(g.frames + 2));
  begin_frame();
  const int32_t debug = REXCVAR_GET(gpu_native_debug_clear);
  if (g.draws == 0 && debug) {
    const uint32_t c = uint32_t(debug);
    g.cmd->clearColor(0, RenderColor(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f));
  }
  bool have_image = false;
  uint32_t vk_frame = UINT32_MAX;  // Vulkan: the shared frame written
  const double p0 = now_ms();
  double p1 = p0, p2 = p0, p3 = p0;
  if (g.out.vk) {
    // The presenter's device reads the shared frame once this frame's GPU work
    // is done: a worker waits for that (present_to_presenter_vk), not the game
    // thread.
    vk_frame = copy_to_presenter_vk();
    have_image = vk_frame != UINT32_MAX;
    close_open_query();
    if (g.timestamps) g.cmd->writeTimestamp(g.timestamps.get(), g.slot * 2 + 1);
    g.cmd->end();
    g.recording = false;
    p1 = p2 = now_ms();
    submit_frame_lists(nullptr, 0, nullptr, 0);
    p3 = now_ms();
  } else {
    const uint32_t k = g.out.next;
    have_image = copy_to_presenter_texture(k);
    close_open_query();
    if (g.timestamps) g.cmd->writeTimestamp(g.timestamps.get(), g.slot * 2 + 1);
    g.cmd->end();
    g.recording = false;
    p1 = p2 = now_ms();
    ID3D12CommandQueue* q = static_cast<D3D12CommandQueue*>(g.queue.get())->d3d;
    if (have_image && g.out.tex_done[k]) q->Wait(g.out.done, g.out.tex_done[k]);  // the presenter has read it
    submit_frame_lists(nullptr, 0, nullptr, 0);
    if (have_image) {
      q->Signal(g.out.ready, ++g.out.ready_value);
      g.out.tex_ready[k] = g.out.ready_value;
    }
    p3 = now_ms();
  }
  g.pending[g.slot] = true;
  g_occ.submitted[g.slot] = g.frames + 1;
  if (g.prof_active) {  // wait for this frame now and log where its GPU time went
    if (g_vulkan) {  // (not reset: the hand-off's worker waits for it too)
      vkWaitForFences(static_cast<VulkanDevice*>(g.device.get())->vk, 1, &static_cast<VulkanCommandFence*>(g.fence)->vk,
                      VK_TRUE, UINT64_MAX);
    } else {
      g.queue->waitForCommandFence(g.fence);
      g.pending[g.slot] = false;
    }
    g.prof_pool->queryResults();
    const uint64_t* ts = g.prof_pool->getResults();
    std::vector<std::pair<double, size_t>> ops;
    std::unordered_map<std::string, std::pair<double, int>> kinds;
    for (size_t i = 1; i < g.prof_labels.size(); ++i) {
      const double ms = ts[i] > ts[i - 1] ? double(ts[i] - ts[i - 1]) / 1e6 : 0.0;
      ops.emplace_back(ms, i);
      const std::string& l = g.prof_labels[i];
      std::string kind = l.rfind("draw#", 0) == 0 ? l.substr(l.find(" vs ")) : l.substr(0, l.find(' '));
      kinds[kind].first += ms;
      ++kinds[kind].second;
    }
    const double total = g.prof_labels.size() > 1 ? double(ts[g.prof_labels.size() - 1] - ts[0]) / 1e6 : 0.0;
    REXKRNL_INFO("PDNATIVE profile frame {}: {} operations, {:.3f} ms GPU", g.frames + 1, ops.size(), total);
    std::sort(ops.begin(), ops.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < ops.size() && i < 25; ++i)
      REXKRNL_INFO("PDNATIVE profile op {:.3f} ms {}", ops[i].first, g.prof_labels[ops[i].second]);
    std::vector<std::pair<double, std::string>> ks;
    for (auto& [k, v] : kinds) ks.emplace_back(v.first, fmt::format("{} x{}", k, v.second));
    std::sort(ks.begin(), ks.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < ks.size() && i < 20; ++i)
      REXKRNL_INFO("PDNATIVE profile kind {:.3f} ms {}", ks[i].first, ks[i].second);
    g.prof_active = false;
  }
  if (have_image) {
    pace_present();
    // (The Xenos vsync worker keeps reporting vblanks to the presenter's pacing,
    // unlike GoldenEye's port, which had to report them from here.)
    if (g.out.vk) {
      present_to_presenter_vk(vk_frame);
    } else {
      present_to_presenter(g.out.next);
      g.out.next = (g.out.next + 1) % 3;
    }
    note_present();
  }
  // Next frame records into the next slot once the GPU is done with the frame
  // that used it last (kFrames frames ago): the game thread doesn't wait for
  // this frame's GPU work, nor for one GPU clock stall.
  g.slot = (g.slot + 1) % kFrames;
  g.cmd = g.cmds[g.slot].get();
  g.fence = g.fences[g.slot].get();
  const bool finished = g.pending[g.slot];
  const double p4 = now_ms();
  // (Vulkan: the worker is done with that frame's fence before it is reset.)
  if (g.out.vk && finished) pd::native_vk::WaitPresent(g.out.vk_handoff[g.slot]);
  if (finished) { g.queue->waitForCommandFence(g.fence); g.pending[g.slot] = false; }
  g.present_ms[0] = p1 - p0; g.present_ms[1] = p3 - p2; g.present_ms[2] = p4 - p3; g.present_ms[3] = now_ms() - p4;
  g.const_used = 0;
  g.const_dedup.clear();
  drain_debug_messages("frame");
  // GPU time of that finished frame's work (submitted at once, so no CPU gaps
  // inside), logged like the Xenos backend's "ge gpu time" line every 600 frames.
  if (g.timestamps && finished) {
    g.timestamps->queryResults();
    const uint64_t* ts = g.timestamps->getResults() + g.slot * 2;  // nanoseconds
    const double ms = ts[1] > ts[0] ? double(ts[1] - ts[0]) / 1e6 : 0.0;
    const uint64_t copy_start = g.timestamps->getResults()[2 * kFrames + g.slot];  // constants copy, just before the frame
    g.copy_ms_sum += ts[0] > copy_start && ts[0] - copy_start < 100000000ull ? double(ts[0] - copy_start) / 1e6 : 0.0;
    g.gpu_ms_sum += ms;
    g.gpu_ms_max = std::max(g.gpu_ms_max, ms);
    if (++g.gpu_frames >= 600) {
      REXKRNL_INFO("PDNATIVE gpu time per frame: {:.2f} ms (max {:.2f}) + constants copy {:.2f} ms",
                   g.gpu_ms_sum / g.gpu_frames, g.gpu_ms_max, g.copy_ms_sum / g.gpu_frames);
      g.gpu_ms_sum = g.gpu_ms_max = g.copy_ms_sum = 0;
      g.gpu_frames = 0;
    }
  }
  g.upload_used = 0;
  for (int i = 0; i < 5; ++i) { g.last_sect[i] = g.sect[i]; g.sect[i] = 0; }
  g.last_vdecode_ms = g.vdecode_ms; g.vdecode_ms = 0;
  g.last_flush_ms = g.flush_ms; g.last_alloc_ms = g.alloc_ms; g.last_hash_ms = g.hash_ms; g.flush_ms = g.alloc_ms = g.hash_ms = 0; g.last_flushes = g.flushes; g.flushes = 0;
  g.last_tex_decode_ms = g.tex_decode_ms; g.last_tex_mip_ms = g.tex_mip_ms; g.tex_decode_ms = g.tex_mip_ms = 0;
  g.last_vcache_hits = g.vcache_hits; g.last_vcache_misses = g.vcache_misses; g.vcache_hits = g.vcache_misses = 0;
  // Buffers dropped two or more frames ago are no longer used by the GPU.
  std::erase_if(g.retired_textures, [](const auto& r) {
    if (r.frame + kFrames > g.frames) return false;
    if (r.slot) g.free_slots.push_back(r.slot);
    return true;
  });
  std::erase_if(g.retired, [](const auto& r) {
    if (r.frame + kFrames > g.frames) return false;
    r.pool->free_lists[r.alloc.size_class].push_back(r.alloc);
    return true;
  });
  if ((g.frames % 600) == 0)
    for (auto it = g.vertex_cache.begin(); it != g.vertex_cache.end();) {
      if (g.frames - it->second.last_frame <= 600) { ++it; continue; }
      retire(g.vertex_pool, it->second.buffer);
      it = g.vertex_cache.erase(it);
    }
  if ((g.frames % 600) == 0)
    for (auto it = g.index_cache.begin(); it != g.index_cache.end();) {
      if (g.frames - it->second.last_frame <= 600) { ++it; continue; }
      retire(g.index_pool, it->second.buffer);
      it = g.index_cache.erase(it);
    }
  g.last_pipe_ms = g.pipe_ms; g.last_tex_ms = g.tex_ms; g.last_pipe_n = g.pipe_n; g.last_tex_n = g.tex_n;
  g.pipe_ms = g.tex_ms = 0; g.pipe_n = g.tex_n = 0; g.tex_requests = 0;
  if ((g.frames % 600) == 599) {
    auto used = [](const BufferPool& p) {
      uint64_t free_bytes = 0;
      for (uint32_t c = 0; c < kSizeClasses; ++c) free_bytes += p.free_lists[c].size() * class_bytes(c);
      return fmt::format("{} chunks, {:.1f} MB free-listed, {:.1f} MB bump left", p.chunks.size(), free_bytes / 1048576.0,
                         p.bump == UINT64_MAX ? 0.0 : (p.chunk - std::min(p.bump, p.chunk)) / 1048576.0);
    };
    REXKRNL_INFO("PDNATIVE pools: vertex {} ({} entries); index {} ({} entries); constants {:.1f} MB last frame",
                 used(g.vertex_pool), g.vertex_cache.size(), used(g.index_pool), g.index_cache.size(), g.const_used / 1048576.0);
  }
  pd::g_native_frames.store(g.frames + 1, std::memory_order_relaxed);
  if (++g.frames == 1 || (g.frames % 600) == 0) {
    REXKRNL_INFO("PDNATIVE frame {}: {} draws, {} skipped (shader {} decl {} prim {} range {} stream {} pipeline {}), "
                 "{} depth copies in all; occlusion queries {} begun, {} read, {} objects",
                 g.frames, g.draws, g.skipped, g.skip_reason[0], g.skip_reason[1], g.skip_reason[2],
                 g.skip_reason[3], g.skip_reason[4], g.skip_reason[5], g.depth_copies, g_occ.begins, g_occ.reads,
                 g_occ.slot_of.size());
    if (g_occ.begins || g_occ.released || g_occ.handed_on)
      REXKRNL_INFO("PDNATIVE occlusion: {} of the reads visible; {} objects released, {} handed on, {} released "
                   "on another thread; {} waits for last frame's GPU work ({:.2f} ms in all), {} ran out",
                   g_occ.visible_reads, g_occ.released, g_occ.handed_on, g_occ.released_elsewhere, g_occ.waits,
                   g_occ.wait_ms, g_occ.late);
    if (g_vulkan && (g_occ.vk_extra_draws || g_occ.vk_repeats))
      REXKRNL_INFO("PDNATIVE occlusion (Vulkan): {} draws past a query's first not counted, {} second begins in "
                   "one list skipped", g_occ.vk_extra_draws, g_occ.vk_repeats);
    if (g.out.vk) {
      uint64_t queued = 0, done = 0;
      double wait_ms = 0, refresh_ms = 0;
      pd::native_vk::Stats(queued, done, wait_ms, refresh_ms);
      static uint64_t last_done = 0;
      static double last_wait = 0, last_refresh = 0;
      if (done > last_done)
        REXKRNL_INFO("PDNATIVE Vulkan hand-offs: {} so far ({} waiting); per frame {:.2f} ms waiting for the GPU, "
                     "{:.2f} ms handing over", done, queued - done, (wait_ms - last_wait) / double(done - last_done),
                     (refresh_ms - last_refresh) / double(done - last_done));
      last_done = done;
      last_wait = wait_ms;
      last_refresh = refresh_ms;
    }
    g_occ.begins = g_occ.reads = g_occ.visible_reads = g_occ.waits = g_occ.late = g_occ.released = g_occ.handed_on = 0;
    g_occ.released_elsewhere = g_occ.vk_extra_draws = g_occ.vk_repeats = 0;
    g_occ.wait_ms = 0;
  }
}

// Submits what has been recorded this frame, waits, and carries on with an
// empty upload ring (level loads upload far more texture data than fits).
void flush_mid_frame() {
  if (!g.recording) { g.upload_used = 0; return; }
  const double t0 = now_ms();
  struct T { double t0; ~T() { g.flush_ms += now_ms() - t0; } } timer{t0};
  close_open_query();
  g.cmd->end();
  submit_frame_lists(nullptr, 0, nullptr, 0);
  g.queue->waitForCommandFence(g.fence);
  g.upload_used = 0;
  g.const_used = 0;
  g.const_dedup.clear();
  g.cmd->begin();
  g_occ.list_reset = false;
  g.layout_bound = false;
  g.bound_fb = nullptr;
  bind_target();
  ++g.flushes;
}

// Depth resolve (D3DRESOLVE_DEPTHSTENCIL, flag 0x4): the scene's depth into an
// R32F texture the game samples (the ZSprite pass reads it back as depth).
// Sample 0 of each pixel, as the game asks for one fragment. While nothing has
// written depth since its last clear, a 1x1 texture of the clear value is
// enough (and cheaper); otherwise a compute pass copies the whole target.
constexpr char kDepthCopyShader[] = R"(
#ifdef MSAA
Texture2DMS<float> src : register(t0);
#else
Texture2D<float> src : register(t0);
#endif
RWTexture2D<float> dst : register(u1);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  dst.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
#ifdef MSAA
  dst[id.xy] = src.Load(int2(id.xy), 0);
#else
  dst[id.xy] = src.Load(int3(id.xy, 0));
#endif
}
)";

bool ensure_depth_pass() {
  if (g.depth_pipe) return true;
  if (g.depth_pass_failed) return false;
  g.depth_pass_failed = true;  // until it is up
  std::string source = kDepthCopyShader;
  if (g.msaa.sampleCount > 1) source = "#define MSAA 1\n" + source;
  std::vector<uint8_t> dxil;
  std::string errors;
  if (!pd::native_shaders::CompileDxil(source, L"cs_6_0", dxil, errors)) {
    REXKRNL_WARN("PDNATIVE depth copy shader: {}", errors.substr(0, 300));
    return false;
  }
  RenderDescriptorSetBuilder sb;
  sb.begin();
  sb.addTexture(0);
  sb.addReadWriteTexture(1);
  sb.end();
  RenderPipelineLayoutBuilder lb;
  lb.begin();
  lb.addDescriptorSet(sb);
  lb.end();
  g.depth_layout = lb.create(g.device.get());
  g.depth_shader = g.device->createShader(dxil.data(), dxil.size(), "main",
                                         g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL);
  g.depth_view = g.scene_depth->createTextureView(
      RenderTextureViewDesc::Texture2D(g_vulkan ? RenderFormat::D32_FLOAT : RenderFormat::R32_FLOAT));
  g.depth_pipe = g.device->createComputePipeline(RenderComputePipelineDesc(g.depth_layout.get(), g.depth_shader.get(), 8, 8, 1));
  if (!g.depth_pipe) return false;
  g.depth_pass_failed = false;
  return true;
}

void native_depth_resolve(uint32_t dest) {
  if (g.bound_scene) apply_pending_clears();
  ResolvedTexture& r = g.resolved[dest];
  if (!g.depth_written || !REXCVAR_GET(gpu_native_depth_copy)) {  // every texel is the clear value
    const float value = g.depth_clear;
    r.depth_pending = false;
    if (r.texture && r.span == 1 && r.content == uint64_t(std::bit_cast<uint32_t>(value))) return;
    if (r.texture) g.retired_textures.push_back({g.frames, std::move(r.texture), r.slot});
    r.texture = g.device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R32_FLOAT));
    r.slot = alloc_texture_slot();
    r.span = 1;
    r.content = std::bit_cast<uint32_t>(value);
    ensure_room(512);
    const uint64_t off = upload_alloc(256, 512);
    if (off == UINT64_MAX) return;
    std::memcpy(g.upload_ptr + off, &value, 4);
    g.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(r.texture.get(), RenderTextureLayout::COPY_DEST));
    g.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.texture.get(), 0),
                             RenderTextureCopyLocation::PlacedFootprint(g.upload.get(), RenderFormat::R32_FLOAT, 1, 1, 1, 64, off));
    g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(r.texture.get(), RenderTextureLayout::SHADER_READ));
    g.textures->setTexture(r.slot, r.texture.get(), RenderTextureLayout::SHADER_READ);
    g.bound_fb = nullptr;
    bind_target();
    return;
  }
  if (!ensure_depth_pass()) return;
  if (r.texture && r.span == 0 && r.content == g.depth_gen) return;  // already holds this depth
  if (!r.texture || r.span != 0) {  // full size
    if (r.texture) g.retired_textures.push_back({g.frames, std::move(r.texture), r.slot});
    r.texture = g.device->createTexture(RenderTextureDesc::Texture2D(g_scene_w, g_scene_h, 1, RenderFormat::R32_FLOAT,
                                                                    RenderTextureFlag::UNORDERED_ACCESS));
    r.slot = alloc_texture_slot();
    r.span = 0;
    r.content = 0;
    RenderDescriptorSetBuilder sb;
    sb.begin();
    sb.addTexture(0);
    sb.addReadWriteTexture(1);
    sb.end();
    std::unique_ptr<RenderDescriptorSet>& set = g.depth_sets[dest];
    set = sb.create(g.device.get());
    set->setTexture(0, g.scene_depth.get(), RenderTextureLayout::SHADER_READ, g.depth_view.get());
    set->setTexture(1, r.texture.get(), RenderTextureLayout::GENERAL);
  }
  if (!r.depth_pending) {
    r.depth_pending = true;
    g.depth_pending.push_back(dest);
  }
}

// The copy of a recorded depth resolve: the scene depth as it is now.
void copy_pending_depth(uint32_t dest, ResolvedTexture& r) {
  r.depth_pending = false;
  std::erase(g.depth_pending, dest);
  if (r.content == g.depth_gen) return;
  g.cmd->barriers(RenderBarrierStage::COMPUTE, {RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::SHADER_READ),
                                                RenderTextureBarrier(r.texture.get(), RenderTextureLayout::GENERAL)});
  g.cmd->setComputePipelineLayout(g.depth_layout.get());
  g.cmd->setPipeline(g.depth_pipe.get());
  g.cmd->setComputeDescriptorSet(g.depth_sets[dest].get(), 0);
  g.cmd->dispatch((g_scene_w + 7) / 8, (g_scene_h + 7) / 8, 1);
  r.content = g.depth_gen;
  ++g.depth_copies;
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, {RenderTextureBarrier(g.scene_depth.get(), RenderTextureLayout::DEPTH_WRITE),
                                                 RenderTextureBarrier(r.texture.get(), RenderTextureLayout::SHADER_READ)});
  g.textures->setTexture(r.slot, r.texture.get(), RenderTextureLayout::SHADER_READ);
  g.bound_fb = nullptr;
  bind_target();
}

// Before a draw or clear changes the scene depth: recorded depth resolves are
// copied now if their texture has been sampled lately (so it gets the depth as
// it was at the resolve); the rest stay recorded and cost nothing unless a
// draw samples them later (then they get the depth of that moment).
void depth_will_change() {
  for (size_t i = 0; i < g.depth_pending.size();) {
    const uint32_t dest = g.depth_pending[i];
    ResolvedTexture& r = g.resolved[dest];
    if (r.depth_used_frame && r.depth_used_frame + 120 > g.frames + 1)
      copy_pending_depth(dest, r);  // erases it from the list
    else
      ++i;
  }
}

// The half-pixel fill (the classic renderer's resolve_resolution_scale_fill_
// half_pixel_offset). Draws with D3D9 pixel centres move half a guest pixel
// right and down, which at a Render Scale of s is s/2 scene pixels: the first
// (s + 1) / 2 columns and rows of a target are never drawn and stay at the
// clear colour. The 360 has no such gap (its pixels are whole guest pixels), so
// each resolve copies the first surely covered column and row over them. Seen
// as a black band along the top and left of the Perfect Menu's blurred
// backdrop at 3x: the blur chain resolves and re-samples a 128x128 target six
// times, and the gap grows each pass. `dst` holds the resolve (w x h) and is in
// COPY_DEST or RESOLVE_DEST; it comes back in COPY_DEST.
void fill_half_pixel_gap(RenderTexture* dst, uint32_t w, uint32_t h) {
  const uint32_t c = (g_scene_scale + 1) / 2;
  if (g_scene_scale < 2 || !g.last_draw_half_pixel || !REXCVAR_GET(gpu_native_fill_half_pixel) || w <= c || h <= c)
    return;
  if (!g.gap_column || g.gap_w != g_scene_w || g.gap_h != g_scene_h) {
    if (g.gap_column) {
      g.retired_textures.push_back({g.frames, std::move(g.gap_column), 0});
      g.retired_textures.push_back({g.frames, std::move(g.gap_row), 0});
    }
    g.gap_column = g.device->createTexture(RenderTextureDesc::Texture2D(1, g_scene_h, 1, kSceneFormat));
    g.gap_row = g.device->createTexture(RenderTextureDesc::Texture2D(g_scene_w, 1, 1, kSceneFormat));
    g.gap_w = g_scene_w;
    g.gap_h = g_scene_h;
  }
  auto box = [](uint32_t l, uint32_t t, uint32_t r, uint32_t b) {
    RenderBox x;
    x.left = int32_t(l); x.top = int32_t(t); x.front = 0;
    x.right = int32_t(r); x.bottom = int32_t(b); x.back = 1;
    return x;
  };
  auto copy = [&](RenderTexture* to, uint32_t x, uint32_t y, RenderTexture* from, const RenderBox& b) {
    g.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(to), RenderTextureCopyLocation::Subresource(from), x, y,
                             0, &b);
  };
  // Column c and row c of the resolve, out to the scratch strips.
  g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(dst, RenderTextureLayout::COPY_SOURCE),
                                             RenderTextureBarrier(g.gap_column.get(), RenderTextureLayout::COPY_DEST),
                                             RenderTextureBarrier(g.gap_row.get(), RenderTextureLayout::COPY_DEST)});
  const RenderBox col = box(c, 0, c + 1, h), row = box(0, c, w, c + 1);
  copy(g.gap_column.get(), 0, 0, dst, col);
  copy(g.gap_row.get(), 0, 0, dst, row);
  // ... and back over the gap: columns 0..c-1 below row c, rows 0..c-1 right of
  // column c, and the corner from the pixel (c, c).
  g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(dst, RenderTextureLayout::COPY_DEST),
                                             RenderTextureBarrier(g.gap_column.get(), RenderTextureLayout::COPY_SOURCE),
                                             RenderTextureBarrier(g.gap_row.get(), RenderTextureLayout::COPY_SOURCE)});
  const RenderBox col_rest = box(0, c, 1, h), row_rest = box(c, 0, w, 1), corner = box(0, c, 1, c + 1);
  for (uint32_t k = 0; k < c; ++k) {
    copy(dst, k, c, g.gap_column.get(), col_rest);
    copy(dst, c, k, g.gap_row.get(), row_rest);
    for (uint32_t j = 0; j < c; ++j) copy(dst, k, j, g.gap_column.get(), corner);
  }
}

// Resolve: the bound target's colour into a texture keyed by the guest
// destination (depth: native_depth_resolve); the game resolves each EDRAM tile
// separately, so repeats with no draws in between are skipped. Flags (XDK):
// 0x4 depth/stencil, 0x8 all fragments, 0x10.. one fragment, 0x100 / 0x200
// clear colour / depth afterwards (the game clears explicitly as well).
void native_resolve_copy(uint32_t dest, uint32_t flags, uint32_t src_rect) {
  if (!dest || !ensure_device() || REXCVAR_GET(gpu_native_skip_resolves)) return;
  NTRACE("  resolve copy {:08X}: last {:08X} draws since {} bound scene {} rt {:08X}", dest, g.last_resolve_dest,
         g.draws_since_resolve, g.bound_scene, g.guest_rt);
  if (flags & 0x4) {
    begin_frame();
    native_depth_resolve(dest);
    return;
  }
  begin_frame();
  bind_target();
  if (dest == g.last_resolve_dest && !g.draws_since_resolve) return;
  if (g.bound_scene) apply_pending_clears();
  g.last_resolve_dest = dest;
  g.draws_since_resolve = false;
  // The destination's size (its fetch constant, at the Render Scale) taken from
  // the source rectangle's corner: the Perfect Menu's blur passes draw into the
  // top-left 256x256 and 128x128 of a target and resolve just that. A full-size
  // destination is the whole target (predicated tiles: the first resolve).
  const uint32_t d2 = g32(dest + 36);
  const uint32_t dw = std::min(g_scene_w, ((d2 & 0x1FFF) + 1) * g_scene_scale);
  const uint32_t dh = std::min(g_scene_h, (((d2 >> 13) & 0x1FFF) + 1) * g_scene_scale);
  uint32_t sx = 0, sy = 0;
  if (src_rect && (dw < g_scene_w || dh < g_scene_h)) {
    sx = std::min(uint32_t(std::max(0, int32_t(g32(src_rect)))) * g_scene_scale, g_scene_w - dw);
    sy = std::min(uint32_t(std::max(0, int32_t(g32(src_rect + 4)))) * g_scene_scale, g_scene_h - dh);
  }
  ResolvedTexture& r = g.resolved[dest];
  if (r.texture && r.span == 0 && (r.w != dw || r.h != dh) && !g.depth_sets.count(dest)) {
    g.retired_textures.push_back({g.frames, std::move(r.texture), 0});
    r.swizzled.clear();
  }
  if (!r.texture) {
    r.texture = g.device->createTexture(RenderTextureDesc::Texture2D(dw, dh, 1, kSceneFormat));
    r.w = dw;
    r.h = dh;
    r.slot = g.next_slot < kTextureSlots ? g.next_slot++ : 0;
    g.textures->setTexture(r.slot, r.texture.get(), RenderTextureLayout::SHADER_READ);
    const uint32_t d1 = g32(dest + 32), d2 = g32(dest + 36);
    if (const uint32_t base = d1 & 0xFFFFF000u) g.resolved_base[base] = dest;
    REXKRNL_INFO("PDNATIVE resolve destination {:08X}: memory {:08X}, {}x{}", dest, d1 & 0xFFFFF000u, (d2 & 0x1FFF) + 1,
                 ((d2 >> 13) & 0x1FFF) + 1);
    if (recording_trace()) dump_trace_ring("new resolve destination");
  }
  RenderTexture* src = g.bound_scene ? g.scene.get() : g.bound_color;
  {
    static std::unordered_map<uint32_t, uint32_t> logged;  // the first resolves to each destination
    if (logged[dest]++ < 4)
      REXKRNL_INFO("PDNATIVE resolve {:08X} {}x{} from {},{} of {} (rt {:08X} depth {:08X}), frame {} draw {}", dest, dw, dh,
                   sx, sy, g.bound_scene ? "scene" : "target", g.guest_rt, g.guest_depth, g.frames + 1, g.draws);
  }
  if (dw == g_scene_w && dh == g_scene_h) {
    if (g.bound_scene) {
      copy_scene_to(r.texture.get());
    } else {
      g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE),
                                                 RenderTextureBarrier(r.texture.get(), RenderTextureLayout::COPY_DEST)});
      g.cmd->copyTexture(r.texture.get(), src);
    }
  } else if (g.bound_scene && g.msaa.sampleCount != RenderSampleCount::COUNT_1) {
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(src, RenderTextureLayout::RESOLVE_SOURCE),
                                               RenderTextureBarrier(r.texture.get(), RenderTextureLayout::RESOLVE_DEST)});
    const RenderRect rect(int32_t(sx), int32_t(sy), int32_t(sx + dw), int32_t(sy + dh));
    g.cmd->resolveTextureRegion(r.texture.get(), 0, 0, src, &rect);
  } else {
    g.cmd->barriers(RenderBarrierStage::COPY, {RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE),
                                               RenderTextureBarrier(r.texture.get(), RenderTextureLayout::COPY_DEST)});
    RenderBox box;
    box.left = int32_t(sx); box.top = int32_t(sy); box.front = 0;
    box.right = int32_t(sx + dw); box.bottom = int32_t(sy + dh); box.back = 1;
    g.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.texture.get()),
                             RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
  }
  fill_half_pixel_gap(r.texture.get(), dw, dh);
  g.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(r.texture.get(), RenderTextureLayout::SHADER_READ));
  g.bound_fb = nullptr;  // the source left COLOR_WRITE: bind it again
  bind_target();
  prof_mark(fmt::format("resolve {:08X}", dest));
}

// Resolve, then the clears it asks for (D3DRESOLVE_CLEARRENDERTARGET 0x100,
// D3DRESOLVE_CLEARDEPTHSTENCIL 0x200) with its pClearColor (a D3DVECTOR4 of
// big-endian floats) and ClearZ: Perfect Dark clears its targets this way,
// not with Clear (GoldenEye also called Clear, so its port ignored these).
void native_resolve(uint32_t dest, uint32_t flags, uint32_t src_rect, uint32_t clear_color, double clear_z) {
  native_resolve_copy(dest, flags, src_rect);
  if (!(flags & 0x300) || !ensure_device() || !g.bound_scene) return;
  if ((flags & 0x100) && clear_color) {
    float c[4];
    for (int i = 0; i < 4; ++i) { const uint32_t u = g32(clear_color + i * 4); std::memcpy(&c[i], &u, 4); }
    g.pending_color = RenderColor(c[0], c[1], c[2], c[3]);
    g.pending_clear_color = true;
  }
  if (flags & 0x200) {
    g.pending_depth = float(clear_z);
    g.pending_clear_depth = true;
  }
  // (Not counted as new drawing: the game resolves each predicated tile to the
  // same texture with these clears, and a repeat resolve must not copy the
  // cleared target over the picture.)
  NTRACE("  resolve clears {:X} deferred, color {:08X} = {:08X} {:08X} {:08X} {:08X} z {}", flags & 0x300, clear_color,
         clear_color ? g32(clear_color) : 0, clear_color ? g32(clear_color + 4) : 0, clear_color ? g32(clear_color + 8) : 0,
         clear_color ? g32(clear_color + 12) : 0, clear_z);
}

// --- occlusion queries (the flares) -----------------------------------------
//
// Perfect Dark tests each flare with a point draw inside a query and reads the
// result at once (D3DQuery_GetData 0x8233F0F0; the flare code 0x82267468 only
// asks whether any sample passed). Under Xenos that read waited for the GPU to
// finish the frame so far (HANDOVER 4be/4bg: the stutter on Intel hybrid PCs).
// Here each query is a real D3D12 binary occlusion query around the native
// draws. GetData answers with the object's newest result from an earlier frame,
// as the guest's own GetData did after the frame's swap: one frame late, as on
// the console. That frame's GPU work has normally finished by then (1-4 ms of
// GPU time a frame); if not, GetData waits for it, at most kQueryWaitMs, then
// takes the one before. An object with no result yet reads as hidden.
//
// (Until 0.9.0a-test4 every answer was kFrames frames old and a new object read
// as visible, and the 512 slots ran out within a level because released
// objects kept theirs: glows showed through walls and flashed, issue #1.)

bool ensure_occlusion() {
  if (g_occ.ready) return true;
  if (g_occ.failed || !g.device) return false;
  g_occ.failed = true;  // until it is up
  if (g_vulkan) {
    VkQueryPoolCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kMaxQueries * kFrames;
    g_occ.vk_readback = g.device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(kMaxQueries) * kFrames * 8));
    void* mapped = g_occ.vk_readback ? g_occ.vk_readback->map() : nullptr;
    if (vkCreateQueryPool(static_cast<VulkanDevice*>(g.device.get())->vk, &qi, nullptr, &g_occ.vk_pool) != VK_SUCCESS ||
        !mapped) {
      REXKRNL_WARN("PDNATIVE occlusion queries could not be set up: flares stay hidden");
      return false;
    }
    g_occ.mapped = static_cast<const uint64_t*>(mapped);
    g_occ.vk_used.assign(kMaxQueries, 0);
    g_occ.slots.reserve(kMaxQueries);
    g_occ.failed = false;
    g_occ.ready = true;
    return true;
  }
  ID3D12Device* dev = static_cast<D3D12Device*>(g.device.get())->d3d;
  D3D12_QUERY_HEAP_DESC qd{};
  qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  qd.Count = kMaxQueries * kFrames;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = uint64_t(kMaxQueries) * kFrames * 8;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  void* mapped = nullptr;
  if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_occ.heap))) ||
      FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&g_occ.readback))) ||
      FAILED(g_occ.readback->Map(0, nullptr, &mapped))) {
    REXKRNL_WARN("PDNATIVE occlusion queries could not be set up: flares stay visible");
    return false;
  }
  g_occ.mapped = static_cast<const uint64_t*>(mapped);
  g_occ.wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_occ.slots.reserve(kMaxQueries);
  g_occ.failed = false;
  g_occ.ready = true;
  return true;
}

uint32_t query_slot(uint32_t object) {
  const auto it = g_occ.slot_of.find(object);
  if (it != g_occ.slot_of.end()) return it->second;
  uint32_t slot;
  if (!g_occ.free_slots.empty()) {
    slot = g_occ.free_slots.back();
    g_occ.free_slots.pop_back();
  } else if (g_occ.slots.size() < kMaxQueries) {
    slot = uint32_t(g_occ.slots.size());
    g_occ.slots.emplace_back();
  } else {
    // All taken by live objects (the game releases its query objects as
    // lights come and go, D3DQuery_Release below): hand the oldest slot on.
    slot = g_occ.next_reuse;
    g_occ.next_reuse = (g_occ.next_reuse + 1) % kMaxQueries;
    if (g_occ.open == int32_t(slot)) return UINT32_MAX;
    g_occ.slot_of.erase(g_occ.slots[slot].owner);
    ++g_occ.handed_on;
  }
  // (A result still in flight for the old owner is never read: the new one
  // reads only frames that ended its own queries.)
  g_occ.slots[slot] = Occlusion::Slot{};
  g_occ.slots[slot].owner = object;
  g_occ.slot_of[object] = slot;
  return slot;
}

// The game released a query object for good: its slot goes to the next one.
void query_released(uint32_t object) {
  const auto it = g_occ.slot_of.find(object);
  if (it == g_occ.slot_of.end()) return;
  const uint32_t slot = it->second;
  if (g_occ.open == int32_t(slot)) {
    if (g.recording && !g_vulkan) close_open_query();  // (Vulkan: never left running)
    g_occ.open = -1;
  }
  g_occ.slot_of.erase(it);
  g_occ.slots[slot] = Occlusion::Slot{};
  g_occ.free_slots.push_back(slot);
  ++g_occ.released;
}

// Vulkan: copies the results of the queries this list ended into the readback
// buffer (outside the render pass), made visible to the CPU once the list's
// fence has signalled.
void vk_copy_query_results() {
  if (g_occ.vk_copy.empty()) return;
  auto* l = static_cast<VulkanCommandList*>(g.cmd);
  l->endActiveRenderPass();
  VkBuffer dst = static_cast<VulkanBuffer*>(g_occ.vk_readback.get())->vk;
  std::sort(g_occ.vk_copy.begin(), g_occ.vk_copy.end());
  for (size_t i = 0; i < g_occ.vk_copy.size();) {  // in runs of consecutive queries
    size_t j = i + 1;
    while (j < g_occ.vk_copy.size() && g_occ.vk_copy[j] == g_occ.vk_copy[j - 1] + 1) ++j;
    vkCmdCopyQueryPoolResults(l->vk, g_occ.vk_pool, g_occ.vk_copy[i], uint32_t(j - i), dst,
                              uint64_t(g_occ.vk_copy[i]) * 8, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    i = j;
  }
  VkMemoryBarrier mb{};
  mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(l->vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                       nullptr);
  g_occ.vk_copy.clear();
}

// Ends a query left open when the list is closed (frame end, mid-frame flush):
// a D3D12 query must begin and end in one command list. (On Vulkan a query
// never stays open past its draw; the list's results are copied here.)
void close_open_query() {
  if (g_vulkan) {
    g_occ.open = -1;
    vk_copy_query_results();
    return;
  }
  if (g_occ.open < 0) return;
  const UINT index = g.slot * kMaxQueries + uint32_t(g_occ.open);
  static_cast<D3D12CommandList*>(g.cmd)->d3d->EndQuery(g_occ.heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, index);
  g_occ.open = -1;  // (not resolved: its old result stays)
}

void query_begin(uint32_t object) {
  if (!ensure_device() || !ensure_occlusion()) return;
  begin_frame();
  close_open_query();
  const uint32_t q = query_slot(object);
  if (q == UINT32_MAX) return;
  if (g_vulkan) {
    // The list's slot range is reset before its first query (here, between
    // draws: the render pass ends and the next draw starts it again).
    if (!g_occ.list_reset) {
      static_cast<VulkanCommandList*>(g.cmd)->endActiveRenderPass();
      vkCmdResetQueryPool(static_cast<VulkanCommandList*>(g.cmd)->vk, g_occ.vk_pool, g.slot * kMaxQueries, kMaxQueries);
      std::fill(g_occ.vk_used.begin(), g_occ.vk_used.end(), uint8_t(0));
      g_occ.list_reset = true;
    }
    if (g_occ.vk_used[q]) {  // begun already in this list: keeps the result it has
      ++g_occ.vk_repeats;
      return;
    }
    g_occ.vk_begun = g_occ.vk_ended = false;  // begun at its draw (vk_query_before_draw)
  } else {
    static_cast<D3D12CommandList*>(g.cmd)->d3d->BeginQuery(g_occ.heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION,
                                                           g.slot * kMaxQueries + q);
  }
  g_occ.open = int32_t(q);
  g_occ.thread = GetCurrentThreadId();
  ++g_occ.begins;
}

// Vulkan: the open query begins right before its first draw, in that draw's
// render pass (true: end it right after the draw).
bool vk_query_before_draw() {
  if (g_occ.open < 0 || g_occ.vk_begun) {
    if (g_occ.open >= 0) ++g_occ.vk_extra_draws;  // (Perfect Dark's flare tests draw once)
    return false;
  }
  auto* l = static_cast<VulkanCommandList*>(g.cmd);
  l->checkActiveRenderPass();
  vkCmdBeginQuery(l->vk, g_occ.vk_pool, g.slot * kMaxQueries + uint32_t(g_occ.open), 0);
  g_occ.vk_begun = true;
  g_occ.vk_used[g_occ.open] = 1;
  return true;
}

void vk_query_after_draw() {
  const uint32_t index = g.slot * kMaxQueries + uint32_t(g_occ.open);
  vkCmdEndQuery(static_cast<VulkanCommandList*>(g.cmd)->vk, g_occ.vk_pool, index);
  g_occ.vk_ended = true;
  g_occ.vk_copy.push_back(index);
}

void query_end(uint32_t object) {
  const auto it = g_occ.slot_of.find(object);
  if (g_occ.open < 0 || it == g_occ.slot_of.end() || int32_t(it->second) != g_occ.open || !g.recording) return;
  Occlusion::Slot& q = g_occ.slots[it->second];
  if (g_vulkan) {
    // (Ended after its draw; with no draw, nothing passed: hidden.)
    q.empty[g.slot] = !g_occ.vk_ended;
  } else {
    const UINT index = g.slot * kMaxQueries + it->second;
    ID3D12GraphicsCommandList* l = static_cast<D3D12CommandList*>(g.cmd)->d3d;
    l->EndQuery(g_occ.heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, index);
    l->ResolveQueryData(g_occ.heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, index, 1, g_occ.readback, uint64_t(index) * 8);
  }
  q.ended[g.slot] = g.frames + 1;
  g_occ.open = -1;
}

// Whether the GPU has finished frame `frame`, recorded in frame slot s (its
// results are then in the readback buffer). Never waits on the slot's own
// event: present_frame's wait needs it.
bool frame_done(uint32_t s, uint64_t frame) {
  if (frame < g_occ.submitted[s] || !g.pending[s]) return true;  // waited for since
  if (g_vulkan)
    return vkGetFenceStatus(static_cast<VulkanDevice*>(g.device.get())->vk,
                            static_cast<VulkanCommandFence*>(g.fences[s].get())->vk) == VK_SUCCESS;
  const auto* f = static_cast<D3D12CommandFence*>(g.fences[s].get());
  return f->d3d->GetCompletedValue() + 1 >= f->fenceValue;  // fenceValue is one past the last signal
}

// Waits up to `ms` for frame slot s's last submission.
bool wait_for_slot(uint32_t s, uint32_t ms) {
  if (g_vulkan)  // (a fence may be waited for on several threads; only the game thread resets it)
    return vkWaitForFences(static_cast<VulkanDevice*>(g.device.get())->vk, 1,
                           &static_cast<VulkanCommandFence*>(g.fences[s].get())->vk, VK_TRUE,
                           uint64_t(ms) * 1000000ull) == VK_SUCCESS;
  const auto* f = static_cast<D3D12CommandFence*>(g.fences[s].get());
  const uint64_t value = f->fenceValue - 1;
  const double end = now_ms() + ms;
  // (Checked again after each wake: a wait that ran out earlier leaves the
  // event set when its value is reached.)
  while (f->d3d->GetCompletedValue() < value) {
    const double left = end - now_ms();
    if (left <= 0 || !g_occ.wait_event || FAILED(f->d3d->SetEventOnCompletion(value, g_occ.wait_event))) return false;
    WaitForSingleObject(g_occ.wait_event, DWORD(std::ceil(left)));
  }
  return true;
}

// GetData's answer for a query object: 1 if any sample passed in its newest
// finished query from an earlier frame, 0 if none did or there is none yet.
uint32_t query_result(uint32_t object) {
  const auto it = g_occ.slot_of.find(object);
  if (it == g_occ.slot_of.end() || !g_occ.mapped) return 0;
  const uint32_t qi = it->second;
  Occlusion::Slot& q = g_occ.slots[qi];
  const uint64_t current = g.frames + 1;
  uint64_t below = UINT64_MAX;  // newer frames not finished in time
  for (;;) {
    uint32_t best = UINT32_MAX;
    for (uint32_t s = 0; s < kFrames; ++s) {
      const uint64_t f = q.ended[s];
      if (f && f < current && f < below && f > q.result_frame && (best == UINT32_MAX || f > q.ended[best])) best = s;
    }
    if (best == UINT32_MAX) break;
    const uint64_t f = q.ended[best];
    bool done = frame_done(best, f);
    if (!done && g_occ.no_wait_frame != current) {
      const double t0 = now_ms();
      done = wait_for_slot(best, kQueryWaitMs);
      g_occ.wait_ms += now_ms() - t0;
      ++g_occ.waits;
      if (!done) { g_occ.no_wait_frame = current; ++g_occ.late; }
    }
    if (done) {
      const uint32_t index = best * kMaxQueries + qi;
      if (g_vulkan && q.empty[best]) {
        q.visible = 0;
      } else {
        if (g_vulkan)  // (readback memory need not be host-coherent)
          vmaInvalidateAllocation(static_cast<VulkanDevice*>(g.device.get())->allocator,
                                  static_cast<VulkanBuffer*>(g_occ.vk_readback.get())->allocation, uint64_t(index) * 8, 8);
        q.visible = g_occ.mapped[index] != 0;
      }
      q.result_frame = f;
      break;
    }
    below = f;
  }
  return q.result_frame ? q.visible : 0;
}

}  // namespace

// gpu_native as it was at the first D3D call (fixed for the run), and off for
// good once the native device failed to start. (GoldenEye's frame-limiter and
// gamma-ramp hooks have no counterpart: Perfect Dark's game code never calls
// SetGammaRamp, and its pacing comes from the guest vblank.)
bool NativeOn() {
  static const bool on = [] {
    const bool native = REXCVAR_GET(gpu_native);
    // Shaders are queued for translation from the game's first D3D calls on,
    // before the device starts: SPIR-V if the presenter runs on Vulkan
    // (OnPreSetup has set gpu by now; ensure_device confirms it).
    if (native) pd::native_shaders::SetSpirv(rex::cvar::GetFlagByName("gpu") == "vulkan");
    return native;
  }();
  return on && !g.failed;
}
#define NATIVE_ON() NativeOn()

// D3DDevice_CreateVertexShader / CreatePixelShader(pFunction) -> shader object.
extern "C" REX_FUNC(sub_8232F5A0) {
  const uint32_t blob = ctx.r3.u32;
  __imp__sub_8232F5A0(ctx, base);
  if (NATIVE_ON() && ctx.r3.u32) {
    g.created_blobs[ctx.r3.u32] = blob;
    g.shaders.erase(ctx.r3.u32);
    request_translation(ctx.r3.u32, true);
  }
}
extern "C" REX_FUNC(sub_8232F3B8) {
  const uint32_t blob = ctx.r3.u32;
  __imp__sub_8232F3B8(ctx, base);
  if (NATIVE_ON() && ctx.r3.u32) {
    g.created_blobs[ctx.r3.u32] = blob;
    g.shaders.erase(ctx.r3.u32);
    request_translation(ctx.r3.u32, false);
  }
}
// In-place shader registration: forget any host shader cached for that object,
// and watch it for translating ahead (warm_registered_shaders).
extern "C" REX_FUNC(sub_8232F4A8) {
  const uint32_t object = ctx.r3.u32;
  if (NATIVE_ON()) { g.shaders.erase(object); g.created_blobs.erase(object); }
  __imp__sub_8232F4A8(ctx, base);
  if (NATIVE_ON() && object) g.warm[object] = {true, 0, g.frames};
}
extern "C" REX_FUNC(sub_8232D9A0) {
  const uint32_t object = ctx.r3.u32;
  if (NATIVE_ON()) { g.shaders.erase(object); g.created_blobs.erase(object); }
  __imp__sub_8232D9A0(ctx, base);
  if (NATIVE_ON() && object) g.warm[object] = {false, 0, g.frames};
}

// D3DDevice_SetStreamSource(pDevice, Stream, pVB, Offset, Stride).
extern "C" REX_FUNC(sub_823372C0) {
  if (NATIVE_ON() && ctx.r4.u32 < 16) g_streams[ctx.r4.u32] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  __imp__sub_823372C0(ctx, base);
}

// D3DDevice_SetTexture(pDevice, Sampler, pTexture).
extern "C" REX_FUNC(sub_8232BB80) {
  if (NATIVE_ON() && ctx.r4.u32 < 32) g_textures[ctx.r4.u32] = ctx.r5.u32;
  __imp__sub_8232BB80(ctx, base);
}

// D3DDevice_DrawIndexedVertices(pDevice, PrimType, BaseVertexIndex, StartIndex, IndexCount).
extern "C" REX_FUNC(sub_823333F0) {
  if (NATIVE_ON()) {
    const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r7.u32, start = ctx.r6.u32;
    const int32_t base_vertex = ctx.r5.s32;
    const uint32_t ibo = g32(dev + 12612);
    if (ibo && count) {
      const bool wide = (g32(ibo) & 0x80000000u) != 0;
      const uint32_t addr = gpu_view(g32(ibo + 24) & ~3u);  // 0xE0000000+ buffers: data is 4 KB on
      native_draw(dev, prim, count, nullptr, base_vertex, 0, 0, 0, !wide, addr + start * (wide ? 4 : 2), wide);
    }
  }
  __imp__sub_823333F0(ctx, base);
}

// D3DDevice_DrawVerticesUP(pDevice, PrimType, VertexCount, pData, Stride). It
// draws through BeginVertices and ends inline (Perfect Dark's game code calls
// neither BeginVertices nor EndVertices itself).
extern "C" REX_FUNC(sub_82332DF0) {
  if (NATIVE_ON()) native_draw(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, nullptr, 0, 0, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_82332DF0(ctx, base);
}

// D3DDevice_DrawIndexedVerticesUP(pDevice, PrimType, MinVertexIndex, NumVertices,
// IndexCount, pIndexData, IndexDataFormat, pVertexStreamZeroData,
// VertexStreamZeroStride): read from its code (0x82333350, which copies the
// vertices and indices into the space 0x82332E38 reserves; the stride is the
// ninth argument, on the stack at r1+84). IndexDataFormat bit 2 = 32-bit
// indices. Perfect Dark's quads (6 indices each) all come this way.
extern "C" REX_FUNC(sub_82333350) {
  if (NATIVE_ON() && ctx.r7.u32 && ctx.r8.u32 && ctx.r10.u32) {
    const uint32_t stride = g32(ctx.r1.u32 + 84);
    native_draw(ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, nullptr, 0, 0, ctx.r10.u32, stride, true, ctx.r8.u32,
                (ctx.r9.u32 & 4) != 0);
  }
  __imp__sub_82333350(ctx, base);
}

// D3DDevice_Clear(pDevice, Count, pRects, Flags, Color, Z, Stencil).
extern "C" REX_FUNC(sub_82339480) {
  if (NATIVE_ON() && ensure_device()) {
    begin_frame();
    bind_target();
    if (g.bound_scene) apply_pending_clears();
    g.draws_since_resolve = true;
    const uint32_t flags = ctx.r6.u32, c = ctx.r7.u32;
    if (flags & 1) {
      g_clear_color.store(c);
      g.cmd->clearColor(0, RenderColor(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                                       (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f));
    }
    if ((flags & 0x30) && g.bound_scene) {
      depth_will_change();
      g.cmd->clearDepthStencil(true, true, float(ctx.f1.f64), ctx.r9.u32 & 0xFF);
      g.depth_clear = float(ctx.f1.f64);
      g.depth_written = false;
      ++g.depth_gen;
    }
    prof_mark(fmt::format("clear {:X}", flags));
    NTRACE("clear flags {:X} color {:08X} z {}", flags, c, ctx.f1.f64);
  }
  __imp__sub_82339480(ctx, base);
}

// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel,
// DestSliceOrFace, pClearColor, ClearZ (f1), ClearStencil, pParameters): copy the scene.
extern "C" REX_FUNC(sub_8233B638) {
  if (NATIVE_ON()) {
    NTRACE("resolve flags {:X} dest {:08X}", ctx.r4.u32, ctx.r6.u32);
    native_resolve(ctx.r6.u32, ctx.r4.u32, ctx.r5.u32, ctx.r10.u32, ctx.f1.f64);
  }
  __imp__sub_8233B638(ctx, base);
}

// D3DDevice_SetRenderTarget / SetDepthStencilSurface: traced only (one scene target for now).
extern "C" REX_FUNC(sub_82337B78) {
  if (NATIVE_ON() && tracing()) {
    std::string w;
    for (uint32_t i = 0; ctx.r5.u32 && i < 16; ++i) w += fmt::format(" {:08X}", g32(ctx.r5.u32 + i * 4));
    NTRACE("set rt{} {:08X}:{}", ctx.r4.u32, ctx.r5.u32, w);
  }
  const uint32_t dev = ctx.r3.u32;
  if (NATIVE_ON() && ctx.r4.u32 == 0) g.guest_rt = ctx.r5.u32;
  __imp__sub_82337B78(ctx, base);
  if (NATIVE_ON()) NTRACE("  after set rt: dev+10936 {:08X} {:08X} {:08X}", g32(dev + 10936), g32(dev + 10940), g32(dev + 10944));
}
extern "C" REX_FUNC(sub_82337EC8) {
  if (NATIVE_ON()) { NTRACE("set depth {:08X}", ctx.r4.u32); g.guest_depth = ctx.r4.u32; }
  __imp__sub_82337EC8(ctx, base);
}

// D3DDevice_Swap(pDevice, pFrontBuffer, pParameters).
// Frame-time log for comparing the renderers: guest Swap-to-Swap intervals and
// the time spent inside Swap (guest D3D + native present) on the game thread.
void frame_stats(double interval_ms, double swap_ms) {
  static std::vector<double> intervals;
  static double swap_total = 0, swap_max = 0;
  const int32_t every = REXCVAR_GET(gpu_native_frame_stats);
  if (every <= 0) return;
  if (interval_ms > 0) intervals.push_back(interval_ms);
  if (interval_ms > double(REXCVAR_GET(gpu_native_long_frame_ms)))
    REXKRNL_INFO("PDNATIVE long frame {:.1f} ms (swap {:.1f}; present: acquire {:.1f} submit {:.1f} present {:.1f} "
                 "fence {:.1f}): {} pipelines {:.1f} ms, {} textures {:.1f} ms; draws: "
                 "parse {:.1f} indices {:.1f} decode {:.1f} total {:.1f} ms; vertex cache {} hits {} misses; texture decode {:.1f} "
                 "mips {:.1f} ms; vertex decode {:.1f} ms; {} mid-frame flushes {:.1f} ms; pool alloc {:.1f} ms; vertex hash {:.1f} ms",
                 interval_ms,
                 swap_ms, g.present_ms[0], g.present_ms[1], g.present_ms[2], g.present_ms[3], g.last_pipe_n, g.last_pipe_ms, g.last_tex_n, g.last_tex_ms, g.last_sect[0], g.last_sect[1],
                 g.last_sect[2], g.last_sect[4], g.last_vcache_hits, g.last_vcache_misses, g.last_tex_decode_ms,
                 g.last_tex_mip_ms, g.last_vdecode_ms, g.last_flushes, g.last_flush_ms, g.last_alloc_ms, g.last_hash_ms);
  swap_total += swap_ms;
  swap_max = std::max(swap_max, swap_ms);
  if (intervals.size() < size_t(every)) return;
  std::vector<double> v = intervals;
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (double x : v) sum += x;
  const auto pct = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
  REXKRNL_INFO("PDNATIVE frame_stats {} {} frames: avg {:.3f} ms p50 {:.3f} p99 {:.3f} max {:.3f} | swap avg {:.3f} "
               "max {:.3f} ms | thread {}", NATIVE_ON() ? "native" : "xenos", v.size(), sum / double(v.size()),
               pct(0.5), pct(0.99), v.back(), swap_total / double(v.size()), swap_max, GetCurrentThreadId());
  // Spread of the intervals: <15.7, 15.7-16.2, 16.2-16.7, 16.7-17.2, 17.2-17.7, 17.7-19, 19-25, >25 ms.
  int hist[8] = {};
  for (double x : v) {
    const double edges[] = {15.7, 16.2, 16.7, 17.2, 17.7, 19.0, 25.0};
    int b = 0;
    while (b < 7 && x >= edges[b]) ++b;
    ++hist[b];
  }
  REXKRNL_INFO("PDNATIVE frame_hist {} {} {} {} {} {} {} {}", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6],
               hist[7]);
  if (!g.present_intervals.empty()) {
    int phist[8] = {};
    double pmax = 0;
    for (double x : g.present_intervals) {
      const double edges[] = {15.7, 16.2, 16.7, 17.2, 17.7, 19.0, 25.0};
      int b = 0;
      while (b < 7 && x >= edges[b]) ++b;
      ++phist[b];
      pmax = std::max(pmax, x);
    }
    double psum = 0, psq = 0;
    for (double x : g.present_intervals) { psum += x; psq += x * x; }
    const double pmean = psum / g.present_intervals.size();
    REXKRNL_INFO("PDNATIVE present_hist {} {} {} {} {} {} {} {} | interval mean {:.3f} sd {:.3f} max {:.2f} ms | pacing "
                 "wait avg {:.2f} ms, period {:.3f} ms | {} double steps", phist[0], phist[1], phist[2], phist[3], phist[4],
                 phist[5], phist[6], phist[7], pmean, std::sqrt(std::max(0.0, psq / g.present_intervals.size() - pmean * pmean)),
                 pmax, g.pace_wait_sum / g.present_intervals.size(), g.pace_period, g.double_steps);
    g.present_intervals.clear();
    g.pace_wait_sum = 0;
    g.double_steps = 0;
  }
  intervals.clear();
  swap_total = swap_max = 0;
}

// rngSetSeed(seed): the global random seed (0x825F4050) = seed + 1; called at
// boot with the clock and at a match's start with the match's seed. Test runs
// fix it (pd_test_rng_seed) so both renderers' runs pick the same spawns.
extern "C" REX_FUNC(sub_82250B98) {
  if (const int32_t seed = REXCVAR_GET(pd_test_rng_seed)) ctx.r3.u64 = uint32_t(seed);
  __imp__sub_82250B98(ctx, base);
}

// random() (the global seed at 0x825F4050; a second one at 0x825F4058): the
// menus draw numbers every frame, so a seed set there drifts with timing. With
// pd_test_rng_seed set both seeds are reset by the first call of each new
// stage, so a level's first draws (Combat Simulator spawns) are the same.
extern "C" REX_FUNC(sub_82250B30) {
  if (const int32_t seed = REXCVAR_GET(pd_test_rng_seed)) {
    static uint32_t last_stage = UINT32_MAX;
    if (const uint32_t stage = g32(kStageNum); stage != last_stage) {
      last_stage = stage;
      const uint64_t be = __builtin_bswap64(uint64_t(uint32_t(seed)) + 1);
      std::memcpy(gmem() + 0x825F4050u, &be, 8);
      std::memcpy(gmem() + 0x825F4058u, &be, 8);
    }
  }
  __imp__sub_82250B30(ctx, base);
}

// cheatsInit, at each stage's start: the active cheat masks (0x82638300 ids
// 0-31, 0x826382F8 ids 32+) become the enabled ones (0x82638304, 0x826382FC).
// pd_test_cheats ("<bank0 hex>[:<bank1 hex>]") adds to the enabled masks first,
// so a scenario can play with cheats on (Perfect Darkness, item cheats).
extern "C" REX_FUNC(sub_8219D5E0) {
  if (const std::string mask = REXCVAR_GET(pd_test_cheats); !mask.empty()) {
    const size_t colon = mask.find(':');
    const uint32_t add0 = uint32_t(std::strtoul(mask.substr(0, colon).c_str(), nullptr, 16));
    const uint32_t add1 = colon == std::string::npos ? 0 : uint32_t(std::strtoul(mask.c_str() + colon + 1, nullptr, 16));
    for (const auto& [addr, add] : {std::pair{0x82638304u, add0}, std::pair{0x826382FCu, add1}}) {
      const uint32_t be = __builtin_bswap32(g32(addr) | add);
      std::memcpy(gmem() + addr, &be, 4);
    }
    REXKRNL_INFO("PDNATIVE test cheats: stage {} enabled {:08X} {:08X}", g32(kStageNum), g32(0x82638304u), g32(0x826382FCu));
  }
  __imp__sub_8219D5E0(ctx, base);
}

// D3DQuery_Issue(pQuery, dwIssueFlags): D3DISSUE_BEGIN 2, D3DISSUE_END 1.
extern "C" REX_FUNC(sub_8233EE68) {
  if (NATIVE_ON() && g.out.presenter) {
    if (ctx.r4.u32 & 2) query_begin(ctx.r3.u32);
    if (ctx.r4.u32 & 1) query_end(ctx.r3.u32);
  }
  __imp__sub_8233EE68(ctx, base);
}

// D3DQuery_GetData(pQuery, pData, dwSize, dwGetDataFlags): S_OK with the
// object's newest result from an earlier frame (1 or 0 samples passed; see
// query_result). The guest's own GetData is not run in native mode.
extern "C" REX_FUNC(sub_8233F0F0) {
  if (NATIVE_ON() && g.out.presenter && g_occ.ready) {
    const uint32_t visible = query_result(ctx.r3.u32);
    g_occ.visible_reads += visible;
    if (ctx.r4.u32 && ctx.r5.u32 >= 4) {
      const uint32_t be = __builtin_bswap32(visible);
      std::memcpy(gmem() + ctx.r4.u32, &be, 4);
    }
    ++g_occ.reads;
    ctx.r3.u64 = 0;  // S_OK
    return;
  }
  __imp__sub_8233F0F0(ctx, base);
}

// D3DQuery_Release(pQuery): the reference count is at +12; the last one frees
// the object, so its slot goes back for the next query object. (Releases come
// from the thread that issues queries; one from elsewhere is only counted.)
extern "C" REX_FUNC(sub_8233ECF8) {
  if (NATIVE_ON() && g_occ.ready && ctx.r3.u32 && g32(ctx.r3.u32 + 12) == 1) {
    if (GetCurrentThreadId() == g_occ.thread) query_released(ctx.r3.u32);
    else ++g_occ.released_elsewhere;
  }
  __imp__sub_8233ECF8(ctx, base);
}

// The guest's GPU interrupt callback (D3D_GraphicsInterruptCallback, the
// address every log prints in SetInterruptCallback): the SDK's vsync worker
// calls it with source 0 on every guest vblank. Counted here, so Swap can wait
// for the vblank as the guest's presentation interval did through the GPU.
std::atomic<uint64_t> g_vblanks{0};
std::mutex g_vblank_mutex;
std::condition_variable g_vblank_cv;
extern "C" REX_FUNC(sub_8233C8D8) {
  if (ctx.r3.u32 == 0) {
    { std::lock_guard<std::mutex> lock(g_vblank_mutex); g_vblanks.fetch_add(1, std::memory_order_release); }
    g_vblank_cv.notify_all();
  }
  __imp__sub_8233C8D8(ctx, base);
}

// In hung mode the guest's Swap no longer waits for the GPU, which is what held
// the game to the display rate (the first run swapped ~800 times a second): wait
// for a guest vblank after the last Swap, as presentation interval 1 did. (The
// game's simulation runs on vblanks either way.) A missing vblank never holds
// the game for more than 50 ms.
void wait_for_vblank() {
  static uint64_t last = 0;
  std::unique_lock<std::mutex> lock(g_vblank_mutex);
  g_vblank_cv.wait_for(lock, std::chrono::milliseconds(50),
                       [] { return g_vblanks.load(std::memory_order_acquire) > last; });
  last = g_vblanks.load(std::memory_order_acquire);
}

// Test: with RenderDoc attached (renderdoccmd capture), the test pipe's
// "cvar gpu_native_rdc_capture true" captures the next whole frame: the native
// renderer's device Swap to Swap in native mode, the next presented frame (the
// Xenos path's) otherwise. As in the GoldenEye port (b2f09b5).
#if PD_HAS_RENDERDOC
RENDERDOC_API_1_0_1* renderdoc_api() {
  static RENDERDOC_API_1_0_1* api = []() -> RENDERDOC_API_1_0_1* {
    HMODULE m = GetModuleHandleA("renderdoc.dll");
    if (!m) return nullptr;
    auto get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(m, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_0_1* a = nullptr;
    if (!get || get(eRENDERDOC_API_Version_1_0_1, reinterpret_cast<void**>(&a)) != 1) return nullptr;
    return a;
  }();
  return api;
}

void renderdoc_tick() {
  static bool capturing = false;
  static void* device = nullptr;
  RENDERDOC_API_1_0_1* api = renderdoc_api();
  if (!api) return;
  if (capturing) {
    const uint32_t ok = api->EndFrameCapture(device, nullptr);
    capturing = false;
    REXKRNL_INFO("PDNATIVE RenderDoc capture {}", ok ? "written" : "FAILED");
  }
  if (!REXCVAR_GET(gpu_native_rdc_capture)) return;
  rex::cvar::SetFlagByName("gpu_native_rdc_capture", "false");
  api->MaskOverlayBits(0, 0);  // keep RenderDoc's overlay out of test screenshots
  device = NATIVE_ON() && g.device && !g_vulkan ? static_cast<void*>(static_cast<D3D12Device*>(g.device.get())->d3d) : nullptr;
  if (device) {
    api->StartFrameCapture(device, nullptr);
    capturing = true;
  } else {
    api->TriggerCapture();  // the Xenos path draws on its own thread: next present to present
  }
  REXKRNL_INFO("PDNATIVE RenderDoc capture started ({})", device ? "native device, swap to swap" : "next presented frame");
}
#else
void renderdoc_tick() {}
#endif

extern "C" REX_FUNC(sub_82339D50) {
  static LARGE_INTEGER freq{}, last{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  // Once the native device is up, the game's own "GPU hung" mode goes on
  // (device+10941 bit 2, as D3D_GpuHangBreak 0x82341430 sets it: fences are
  // faked on the CPU, D3D_CBlocker_Check 0x82329E08 returns at once and the
  // ring is no longer kicked), so the Xenos command processor goes idle and the
  // native renderer takes the frame at the D3D calls. Before that (or if the
  // device never starts) Xenos draws as usual.
  if (NATIVE_ON() && ctx.r3.u32 && ensure_device() && !(gmem()[ctx.r3.u32 + 10941] & 0x02)) {
    gmem()[ctx.r3.u32 + 10941] |= 0x02;
    REXKRNL_INFO("PDNATIVE native renderer on: guest D3D in its GPU-hung mode, Xenos idle from frame {}", g.frames + 1);
  }
  if (NATIVE_ON() && g.out.presenter) wait_for_vblank();
  if (NATIVE_ON()) { g.front_buffer = ctx.r4.u32; NTRACE("swap front {:08X}", ctx.r4.u32); }
  __imp__sub_82339D50(ctx, base);
  if (NATIVE_ON()) present_frame();
  renderdoc_tick();
  QueryPerformanceCounter(&t1);
  const auto ms = [&](int64_t ticks) { return double(ticks) * 1000.0 / double(freq.QuadPart); };
  frame_stats(last.QuadPart ? ms(t0.QuadPart - last.QuadPart) : 0.0, ms(t1.QuadPart - t0.QuadPart));
  last = t0;
}
