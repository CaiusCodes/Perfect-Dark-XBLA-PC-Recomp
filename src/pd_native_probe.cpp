// Perfect Dark native renderer, checkpoint 2: what the game asks D3D for, per frame.
//
// Counting hooks at the start of the D3D functions named in checkpoint 1
// (native\checkpoint1\d3d_functions_pd.toml; config in pd_native_probe.toml). Off
// unless pd_native_probe is true; then every 300 frames (at D3DDevice_Swap) one line
// gives the per-frame averages, and every shader created after the first 600 frames
// is logged (a native renderer translates shaders; mid-game creation would matter).

#include "generated/default/perfect_dark_init.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(pd_native_probe, false, "PerfectDark",
                    "Count the game's D3D calls per frame and log them every 300 frames "
                    "(native renderer research)");

namespace {
enum Counter {
  kSwap, kDrawUP, kDrawUPVerts, kDraw, kDrawVerts, kDrawIndexed, kDrawIndices, kIndexedWrapper,
  kBeginVertices, kSetTexture, kSetVS, kSetPS, kCreateVS, kCreatePS, kCreateTexture, kSetRT,
  kSetDS, kResolve, kClear, kBeginTiling, kEndTiling, kQueryIssue, kGetData, kSetDecl, kSetStream,
  kSetIndices, kMakeSpace, kOutsideRing, kCount
};
const char* const kNames[kCount] = {
    "swaps", "DrawVerticesUP", "  UP vertices", "DrawVertices", "  vertices", "DrawIndexedVertices",
    "  indices", "indexed wrapper", "BeginVertices", "SetTexture", "SetVertexShader", "SetPixelShader",
    "CreateVertexShader", "CreatePixelShader", "CreateTexture", "SetRenderTarget",
    "SetDepthStencilSurface", "Resolve", "Clear", "BeginTiling", "EndTiling", "Query_Issue", "GetData",
    "SetVertexDeclaration", "SetStreamSource", "SetIndices", "D3D_MakeSpace", "0x8234DF00"};
std::array<std::atomic<uint64_t>, kCount> g_counts{};
std::array<std::atomic<uint64_t>, 16> g_prim_types{};
std::atomic<uint64_t> g_frames{0};

inline bool On() { return REXCVAR_GET(pd_native_probe); }
inline void Add(Counter c, uint64_t n = 1) { g_counts[c].fetch_add(n, std::memory_order_relaxed); }
void Prim(uint32_t type) { g_prim_types[type & 15].fetch_add(1, std::memory_order_relaxed); }

void ShaderCreated(const char* kind) {
  const uint64_t frame = g_frames.load(std::memory_order_relaxed);
  if (frame > 600) REXLOG_INFO("[native_probe] {} created mid-game at frame {}", kind, frame);
}
}  // namespace

void PdProbeSwap() {
  if (!On()) return;
  Add(kSwap);
  const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frame % 300 != 0) return;
  std::string line;
  const double n = double(g_counts[kSwap].exchange(0));
  for (int i = 1; i < kCount; ++i) {
    const uint64_t v = g_counts[i].exchange(0);
    if (v) line += fmt::format("{}{} {:.1f}", line.empty() ? "" : ", ", kNames[i], double(v) / n);
  }
  std::string prims;
  static const char* const kPrim[16] = {"?0", "points", "lines", "linestrip", "tris", "fan", "strip",
                                        "?7", "rects", "?9", "?10", "?11", "?12", "quads", "?14",
                                        "?15"};
  for (int i = 0; i < 16; ++i) {
    const uint64_t v = g_prim_types[i].exchange(0);
    if (v) prims += fmt::format("{}{} {:.1f}", prims.empty() ? "" : ", ", kPrim[i], double(v) / n);
  }
  REXLOG_INFO("[native_probe] per frame over {:.0f} frames (frame {}): {}; draws by primitive: {}", n,
              frame, line, prims);
}

void PdProbeDrawUP(PPCRegister& r4, PPCRegister& r5) {
  if (!On()) return;
  Add(kDrawUP);
  Add(kDrawUPVerts, r5.u32);
  Prim(r4.u32);
}
void PdProbeDraw(PPCRegister& r4, PPCRegister& r6) {
  if (!On()) return;
  Add(kDraw);
  Add(kDrawVerts, r6.u32);
  Prim(r4.u32);
}
void PdProbeDrawIndexed(PPCRegister& r4, PPCRegister& r7) {
  if (!On()) return;
  Add(kDrawIndexed);
  Add(kDrawIndices, r7.u32);
  Prim(r4.u32);
}
#define PD_PROBE_COUNT(fn, counter) \
  void fn() {                       \
    if (On()) Add(counter);         \
  }
PD_PROBE_COUNT(PdProbeIndexedWrapper, kIndexedWrapper)
PD_PROBE_COUNT(PdProbeBeginVertices, kBeginVertices)
PD_PROBE_COUNT(PdProbeSetTexture, kSetTexture)
PD_PROBE_COUNT(PdProbeSetVS, kSetVS)
PD_PROBE_COUNT(PdProbeSetPS, kSetPS)
PD_PROBE_COUNT(PdProbeCreateTexture, kCreateTexture)
PD_PROBE_COUNT(PdProbeSetRT, kSetRT)
PD_PROBE_COUNT(PdProbeSetDS, kSetDS)
PD_PROBE_COUNT(PdProbeResolve, kResolve)
PD_PROBE_COUNT(PdProbeClear, kClear)
PD_PROBE_COUNT(PdProbeBeginTiling, kBeginTiling)
PD_PROBE_COUNT(PdProbeEndTiling, kEndTiling)
PD_PROBE_COUNT(PdProbeQueryIssue, kQueryIssue)
PD_PROBE_COUNT(PdProbeGetData, kGetData)
PD_PROBE_COUNT(PdProbeSetDecl, kSetDecl)
PD_PROBE_COUNT(PdProbeSetStream, kSetStream)
PD_PROBE_COUNT(PdProbeSetIndices, kSetIndices)
PD_PROBE_COUNT(PdProbeMakeSpace, kMakeSpace)
PD_PROBE_COUNT(PdProbeOutsideRing, kOutsideRing)
#undef PD_PROBE_COUNT

void PdProbeCreateVS() {
  if (!On()) return;
  Add(kCreateVS);
  ShaderCreated("vertex shader");
}
void PdProbeCreatePS() {
  if (!On()) return;
  Add(kCreatePS);
  ShaderCreated("pixel shader");
}
