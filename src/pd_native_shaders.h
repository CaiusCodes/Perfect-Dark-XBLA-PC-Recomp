// Perfect Dark native renderer (ported from GoldenEye XBLA's, 2026-11-02)
//
// Native renderer: the game's Xenos shaders translated on the player's PC.
// Each shader container is turned into HLSL by XenosRecomp, patched the way
// GoldenEye's native/translate_shaders.ps1 patches it and compiled to DXIL with DXC on a
// few worker threads. The results are cached as <hash>.vs|ps.final.dxil and
// .cbsize (constant-buffer bytes read), so each shader is translated once.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pd::native_shaders {

// <release>\resources\userdata\native_shaders\v<N> when the exe runs from a
// "runtime" folder, else <exe>\native_shaders\v<N> (dev builds and test
// copies); N = the translation version.
const std::wstring& CacheDir();

// Vulkan (SPIR-V, <hash>.vs|ps.final.spv) or Direct3D 12 (DXIL) output; set
// before the first Request (the renderer's API is chosen at start).
void SetSpirv(bool spirv);
bool Spirv();
// The compiled shader's file name in CacheDir ("final.spv" or "final.dxil").
const wchar_t* FinalExtension();

// Queues a translation of `container` (a shader container as the renderer
// rebuilds it from guest memory; `hash` = its cache name). Does nothing when
// translation is off, the shader is cached or it is already queued.
void Request(uint64_t hash, std::vector<uint8_t> container);

// Waits for a queued or running translation of `hash`. Returns whether one
// finished with the cache files written (false: none queued, or it failed).
bool Wait(uint64_t hash);

// Compiles HLSL to DXIL with DXC for `profile` (e.g. L"cs_6_0"): the
// renderer's own small passes. False (with `errors`) if DXC is missing.
bool CompileDxil(const std::string& source, const wchar_t* profile, std::vector<uint8_t>& dxil, std::string& errors);

}  // namespace pd::native_shaders
