// Perfect Dark native renderer (ported from GoldenEye XBLA's, 2026-11-02)
//
// Native renderer shader translation on the player's PC. See pd_native_shaders.h.

#include "pd_native_shaders.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>  // IStream and friends, left out by WIN32_LEAN_AND_MEAN, for dxcapi.h

#include <dxcapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <regex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>

#include "pd_paths.h"  // the saves folder (userdata-location.txt)

#include "pch.h"  // XenosRecomp's common header (its sources build with it precompiled)
#include "shader_recompiler.h"

REXCVAR_DEFINE_BOOL(gpu_native_translate, true, "GPU",
                    "Native renderer: translate the game's shaders on this PC when they are first created "
                    "(cached in native_shaders)");

REXCVAR_DEFINE_BOOL(gpu_native_keep_hlsl, false, "GPU",
                    "Native renderer test: also save each translated shader's HLSL in the shader cache");

namespace pd::native_shaders {
namespace {

// Bumped whenever a translated shader changes: XenosRecomp fixes or patch_hlsl.
// 2: bool-conditioned exec blocks (cexec bN) honour their condition.
// 3: Perfect Dark: normal/tangent/binormal/fog interpolators; overlapping
//    constant arrays cut at the next constant (native\refs\XenosRecomp).
// 4: vertex shaders' texture fetches as SampleLevel.
// 5: the CUBE instruction's source swizzle (environment-map directions).
// 6: constants as one register file (no per-name members).
constexpr int kTranslationVersion = 6;

// XenosRecomp's shared definitions, prepended to every translated shader.
constexpr char kShaderCommon[] =
#include "pd_shader_common.inc"
    ;

// Containers known to send XenosRecomp into a runaway loop (52 GB in an
// earlier run): recorded as failed without translating.
constexpr uint64_t kSkip[] = {0x0C3F44DB1C250822ull};

std::wstring exe_folder() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s(path, path + n);
  const size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::atomic<bool> g_spirv{false};

std::wstring file_name(uint64_t hash, bool vertex, const wchar_t* ext) {
  wchar_t name[64];
  swprintf(name, 64, L"\\%016llX.%s.%s", static_cast<unsigned long long>(hash), vertex ? L"vs" : L"ps", ext);
  return CacheDir() + name;
}

bool write_file(const std::wstring& path, const void* data, size_t size) {
  // Written under a temporary name and renamed, so a cut-off write never
  // leaves a broken shader in the cache.
  const std::wstring tmp = path + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, size, f) == size;
  std::fclose(f);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

void replace_once(std::string& s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  if (at != std::string::npos) s.replace(at, from.size(), to);
}

// The HLSL the renderer needs, as GoldenEye's native/translate_shaders.ps1 makes it: the
// spec constants and position scale/offset come from the shared constants
// (c17.y, c18), alpha test is always compiled in (threshold 0 = off).
std::string patch_hlsl(std::string text, bool spirv) {
  if (spirv) {
    // XenosRecomp's Vulkan side: constants through buffer addresses in push
    // constants. The same shared fields (c17.y, c18) as below, and the spec
    // constants from there instead of a pipeline specialisation constant.
    replace_once(text, "[[vk::constant_id(0)]] const uint g_SpecConstants = 0;\n\n#define g_SpecConstants() g_SpecConstants",
                 "#define g_NativeSpecConstants vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276)\n"
                 "#define g_NativePosScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288)\n"
                 "#define g_NativePosOffset vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 296)\n"
                 "#define g_SpecConstants() g_NativeSpecConstants");
    replace_once(text, "oPos.xy += g_HalfPixelOffset * oPos.w;",
                 "oPos.xy = oPos.xy * g_NativePosScale + g_NativePosOffset * oPos.w;");
    return text;
  }
  replace_once(text, "float g_AlphaThreshold : packoffset(c17.x);",
               "float g_AlphaThreshold : packoffset(c17.x); \\\n    uint g_NativeSpecConstants : packoffset(c17.y); \\\n"
               "    float2 g_NativePosScale : packoffset(c18.x); \\\n    float2 g_NativePosOffset : packoffset(c18.z);");
  replace_once(text, "oPos.xy += g_HalfPixelOffset * oPos.w;",
               "oPos.xy = oPos.xy * g_NativePosScale + g_NativePosOffset * oPos.w;");
  return text + "\nuint g_SpecConstants() { return g_NativeSpecConstants; }\n";
}

// Bytes of its constant buffer a shader reads: the highest register its
// VertexShaderConstants / PixelShaderConstants cbuffer declares (as
// GoldenEye's native/cb_sizes.py).
uint32_t cb_bytes(const std::string& text, bool vertex) {
  // Perfect Dark's XenosRecomp declares one register file and says how much of
  // it the shader's constants cover.
  const std::string marker = "// xr-registers-used ";
  if (const size_t m = text.find(marker); m != std::string::npos)
    return uint32_t(std::stoul(text.substr(m + marker.size(), 8))) * 16;
  const std::string name = vertex ? "cbuffer VertexShaderConstants" : "cbuffer PixelShaderConstants";
  const size_t at = text.find(name);
  if (at == std::string::npos) return 0;
  const size_t open = text.find('{', at), close = text.find('}', at);
  if (open == std::string::npos || close == std::string::npos || close < open) return 0;
  static const std::regex decl(R"(^\s*\w+\s+\w+(?:\[(\d+)\])?\s*:\s*packoffset\(c(\d+))");
  uint32_t top = 0;
  size_t line = open + 1;
  while (line < close) {
    size_t end = text.find('\n', line);
    if (end == std::string::npos || end > close) end = close;
    std::smatch m;
    const std::string l = text.substr(line, end - line);
    if (std::regex_search(l, m, decl))
      top = std::max<uint32_t>(top, uint32_t(std::stoul(m[2].str()) + (m[1].matched ? std::stoul(m[1].str()) : 1)));
    line = end + 1;
  }
  return top * 16;
}

struct Dxc {
  IDxcCompiler3* compiler = nullptr;
  IDxcUtils* utils = nullptr;
  // dxcompiler.dll (and the dxil.dll it signs with) is loaded only here, when a
  // shader is translated: the exe must still start without it (Xenos mode).
  Dxc() {
    static const auto create = [] {
      HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
      return dll ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance")) : nullptr;
    }();
    if (!create) return;
    create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    create(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
  }
  ~Dxc() {
    if (compiler) compiler->Release();
    if (utils) utils->Release();
  }
  bool compile(const std::string& source, bool vertex, bool spirv, std::vector<uint8_t>& dxil, std::string& errors) {
    return compile(source, vertex ? L"vs_6_0" : L"ps_6_0", dxil, errors, spirv, vertex);
  }
  // SPIR-V as XenosRecomp builds it (DX buffer layout; vertex shaders flip Y
  // into Vulkan's clip space).
  bool compile(const std::string& source, const wchar_t* profile, std::vector<uint8_t>& dxil, std::string& errors,
               bool spirv = false, bool vertex = false) {
    if (!compiler) {
      errors = "dxcompiler.dll could not be loaded";
      return false;
    }
    DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    std::vector<const wchar_t*> args = {L"-T", profile, L"-HV", L"2021", L"-all-resources-bound",
                                        L"-Wno-ignored-attributes", L"-Qstrip_debug"};
    if (spirv) {
      args.insert(args.end(), {L"-spirv", L"-fvk-use-dx-layout", L"-fspv-target-env=vulkan1.2"});
      if (vertex) args.push_back(L"-fvk-invert-y");
    } else {
      args.push_back(L"-Qstrip_reflect");
    }
    IDxcResult* result = nullptr;
    if (FAILED(compiler->Compile(&buffer, args.data(), UINT32(args.size()), nullptr, IID_PPV_ARGS(&result))) || !result) {
      errors = "DXC Compile failed";
      return false;
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    IDxcBlobUtf8* messages = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr)) && messages) {
      if (messages->GetStringLength()) errors.assign(messages->GetStringPointer(), messages->GetStringLength());
      messages->Release();
    }
    IDxcBlob* object = nullptr;
    if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object) {
      const auto* p = static_cast<const uint8_t*>(object->GetBufferPointer());
      dxil.assign(p, p + object->GetBufferSize());
      object->Release();
    }
    result->Release();
    return SUCCEEDED(status) && !dxil.empty();
  }
};

struct Job {
  uint64_t hash = 0;
  bool spirv = false;  // the output the renderer asked for (jobs are keyed by hash and this)
  std::vector<uint8_t> container;
  bool done = false, ok = false;
};

struct Pool {
  std::mutex mutex;
  std::condition_variable work, finished;
  std::deque<std::shared_ptr<Job>> queue;
  std::unordered_map<uint64_t, std::shared_ptr<Job>> jobs;  // queued, running or done
  bool started = false;

  void start() {
    if (started) return;
    started = true;
    // Leave the game thread and one more core alone.
    const unsigned n = std::max(1u, std::min(6u, std::thread::hardware_concurrency() > 2
                                                     ? std::thread::hardware_concurrency() - 2
                                                     : 1u));
    for (unsigned i = 0; i < n; ++i) std::thread([this] { run(); }).detach();
  }

  void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Dxc dxc;
    for (;;) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock lock(mutex);
        work.wait(lock, [this] { return !queue.empty(); });
        job = std::move(queue.front());
        queue.pop_front();
      }
      const bool ok = translate(*job, dxc);
      {
        std::lock_guard lock(mutex);
        job->ok = ok;
        job->done = true;
        job->container.clear();
        job->container.shrink_to_fit();
      }
      finished.notify_all();
    }
  }

  static bool translate(const Job& job, Dxc& dxc) {
    const auto& c = job.container;
    if (c.size() < 36 || std::find(std::begin(kSkip), std::end(kSkip), job.hash) != std::end(kSkip)) return false;
    const bool vertex = (c[3] & 1) != 0;  // container flags 0x102A11xx: bit 0 = vertex shader
    const LARGE_INTEGER t0 = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();
    std::string hlsl;
    try {
      ShaderRecompiler recompiler;
      recompiler.recompile(c.data(), std::string_view(kShaderCommon, sizeof(kShaderCommon) - 1));
      hlsl = patch_hlsl(std::move(recompiler.out), job.spirv);
      // Vertex shaders' texture fetches as SampleLevel (shader_common.h's XR_SAMPLE).
      if (vertex) hlsl = "#define XR_VERTEX_SHADER 1\n" + hlsl;
    } catch (...) {
      REXKRNL_WARN("PDNATIVE shader {:016X}: translation failed", job.hash);
      return false;
    }
    if (REXCVAR_GET(gpu_native_keep_hlsl)) write_file(file_name(job.hash, vertex, L"hlsl"), hlsl.data(), hlsl.size());
    std::vector<uint8_t> dxil;
    std::string errors;
    if (!dxc.compile(hlsl, vertex, job.spirv, dxil, errors)) {
      REXKRNL_WARN("PDNATIVE shader {:016X}: compile failed: {}", job.hash, errors.substr(0, 400));
      return false;
    }
    const std::string cb = std::to_string(cb_bytes(hlsl, vertex));
    if (!write_file(file_name(job.hash, vertex, L"cbsize"), cb.data(), cb.size()) ||
        !write_file(file_name(job.hash, vertex, job.spirv ? L"final.spv" : L"final.dxil"), dxil.data(), dxil.size()))
      return false;
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    REXKRNL_INFO("PDNATIVE shader {:016X} ({}) translated in {:.0f} ms", job.hash, vertex ? "vs" : "ps",
                 double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
    return true;
  }
};

Pool& pool() {
  static Pool* p = new Pool;  // never destroyed: its threads run for the life of the process
  return *p;
}

}  // namespace

const std::wstring& CacheDir() {
  static const std::wstring dir = [] {
    std::wstring exe = exe_folder();
    const size_t slash = exe.find_last_of(L"\\/");
    std::wstring leaf = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    for (wchar_t& ch : leaf) ch = wchar_t(towlower(ch));
    // In a release: the saves folder (resources\userdata, or the one chosen in setup).
    const std::wstring userdata = pd_paths::UserDataRoot().wstring();
    std::wstring d = leaf == L"runtime" ? userdata + L"\\native_shaders" : exe + L"\\native_shaders";
    if (leaf == L"runtime") CreateDirectoryW(userdata.c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    // One folder per translation version: shaders from an older translator or
    // patch_hlsl are never picked up.
    d += L"\\v" + std::to_wstring(kTranslationVersion);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
  }();
  return dir;
}

void Request(uint64_t hash, std::vector<uint8_t> container) {
  if (!REXCVAR_GET(gpu_native_translate) || container.size() < 36) return;
  const bool vertex = (container[3] & 1) != 0;
  Pool& p = pool();
  {
    std::lock_guard lock(p.mutex);
    const uint64_t key = hash ^ (g_spirv.load() ? 0x5350495256ull : 0);
    if (p.jobs.count(key)) return;
    if (GetFileAttributesW(file_name(hash, vertex, FinalExtension()).c_str()) != INVALID_FILE_ATTRIBUTES) return;
    auto job = std::make_shared<Job>();
    job->hash = hash;
    job->spirv = g_spirv.load();
    job->container = std::move(container);
    p.jobs[key] = job;
    p.queue.push_back(std::move(job));
    p.start();
  }
  p.work.notify_one();
}

bool CompileDxil(const std::string& source, const wchar_t* profile, std::vector<uint8_t>& dxil, std::string& errors) {
  Dxc dxc;
  return dxc.compile(source, profile, dxil, errors, g_spirv.load());
}

void SetSpirv(bool spirv) { g_spirv = spirv; }
bool Spirv() { return g_spirv.load(); }
const wchar_t* FinalExtension() { return g_spirv.load() ? L"final.spv" : L"final.dxil"; }

bool Wait(uint64_t hash) {
  Pool& p = pool();
  std::unique_lock lock(p.mutex);
  const auto it = p.jobs.find(hash ^ (g_spirv.load() ? 0x5350495256ull : 0));
  if (it == p.jobs.end()) return false;
  const std::shared_ptr<Job> job = it->second;
  p.finished.wait(lock, [&] { return job->done; });
  return job->ok;
}

}  // namespace pd::native_shaders
