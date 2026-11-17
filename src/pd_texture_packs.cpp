// Texture packs: see pd_texture_packs.h.
#include "pd_texture_packs.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/ui/image_decode.h>

#include "pd_assets_import.h"
#include "pd_paths.h"

REXCVAR_DEFINE_STRING(pd_texture_pack, "", "PerfectDark",
                      "Experimental: the texture pack drawn over the game's textures, a folder name in "
                      "resources\\texture-packs (empty = none). Plume only");
REXCVAR_DEFINE_BOOL(pd_texture_dump, false, "PerfectDark",
                    "Experimental, for texture pack makers: save each texture the game draws as a PNG named by "
                    "its number in resources\\userdata\\texture-dump");
REXCVAR_DEFINE_STRING(pd_test_pick, "", "PerfectDark",
                      "Test: a path the Experimental page's Import rows use instead of opening the file picker");

namespace pd::texpack {

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;  // the members below
std::shared_ptr<const Index> g_index = std::make_shared<Index>();
uint32_t g_version = 0;
std::vector<std::string> g_list;
std::chrono::steady_clock::time_point g_listed{};
std::set<std::pair<int, int>> g_dumped;

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

std::shared_ptr<Index> BuildIndex(const std::string& name) {
  auto index = std::make_shared<Index>();
  index->name = name;
  if (name.empty()) return index;
  const fs::path folder = PacksFolder() / Wide(name);
  std::error_code ec;
  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec) && pd::assets::PackFileId(it->path()) >= 0) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& f : files) {
    const int id = pd::assets::PackFileId(f);
    if (size_t(id) >= index->by_id.size()) index->by_id.resize(size_t(id) + 1);
    if (index->by_id[size_t(id)].empty()) index->by_id[size_t(id)] = f;  // the first by name wins
  }
  return index;
}

}  // namespace

fs::path PacksFolder() { return pd_paths::DataRoot() / "texture-packs"; }

std::vector<std::string> ListPacks(bool rescan) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (!rescan && now - g_listed < std::chrono::seconds(1)) return g_list;
  g_listed = now;
  g_list.clear();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(PacksFolder(), ec)) {
    if (!e.is_directory(ec)) continue;
    const std::string name = Utf8(e.path().filename().wstring());
    // Only names the menu can show and the import would have made (a folder
    // made by hand with other characters still works by name in the toml).
    if (name.empty() || name[0] == '.' || pd::assets::SafePackName(name) != name) continue;
    g_list.push_back(name);
  }
  std::sort(g_list.begin(), g_list.end());
  if (g_list.size() > size_t(kMaxListed)) g_list.resize(size_t(kMaxListed));
  return g_list;
}

std::string ChosenPack() { return REXCVAR_GET(pd_texture_pack); }

int ChoosePack(const std::string& name) {
  rex::cvar::SetFlagByName("pd_texture_pack", name);
  std::shared_ptr<Index> index = BuildIndex(name);
  int count = 0;
  for (const auto& p : index->by_id) count += p.empty() ? 0 : 1;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    index->version = ++g_version;
    g_index = index;
  }
  if (!name.empty())
    REXLOG_INFO("[texpack] \"{}\" chosen: {} pictures in {}", name, count, Utf8((PacksFolder() / Wide(name)).wstring()));
  else
    REXLOG_INFO("[texpack] no texture pack");
  return count;
}

std::shared_ptr<const Index> CurrentIndex() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_index;
}

void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  const std::string name = ChosenPack();
  if (!name.empty()) ChoosePack(name);
}

bool LoadPicture(const fs::path& path, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, std::string* error) {
  constexpr uint32_t kMaxSide = 4096;
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size == 0 || size > (64ull << 20)) {
    *error = ec ? "can't read it" : "over 64 MB";
    return false;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  {
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) {
      *error = "can't read it";
      return false;
    }
  }
  // The size from the PNG header first, so a huge picture is refused before
  // anything is allocated for it.
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() < 24 || std::memcmp(bytes.data(), kSig, 8) != 0 || std::memcmp(&bytes[12], "IHDR", 4) != 0) {
    *error = "not a PNG";
    return false;
  }
  const uint32_t hw = (uint32_t(bytes[16]) << 24) | (uint32_t(bytes[17]) << 16) | (uint32_t(bytes[18]) << 8) | bytes[19];
  const uint32_t hh = (uint32_t(bytes[20]) << 24) | (uint32_t(bytes[21]) << 16) | (uint32_t(bytes[22]) << 8) | bytes[23];
  if (!hw || !hh || hw > kMaxSide || hh > kMaxSide) {
    *error = "its size " + std::to_string(hw) + "x" + std::to_string(hh) + " is over 4096x4096";
    return false;
  }
  int iw = 0, ih = 0;
  rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), iw, ih);
  if (rgba.empty() || iw <= 0 || ih <= 0) {
    *error = "the PNG is damaged";
    return false;
  }
  w = uint32_t(iw);
  h = uint32_t(ih);
  return true;
}

fs::path DumpFolder() { return pd_paths::UserDataRoot() / "texture-dump"; }

bool Dumping() { return REXCVAR_GET(pd_texture_dump); }

void SetDumping(bool on) {
  rex::cvar::SetFlagByName("pd_texture_dump", on ? "true" : "false");
  if (on) {
    std::error_code ec;
    fs::create_directories(DumpFolder() / "remastered", ec);
    REXLOG_INFO("[texpack] texture dump on: {}", Utf8(DumpFolder().wstring()));
  }
}

bool ClaimDump(int id, int kind) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_dumped.insert({id, kind}).second;
}

bool WritePng(const fs::path& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  // WIC wants COM on this thread (the texture worker).
  thread_local const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  (void)com;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  // RGBA -> the encoder's BGRA.
  const UINT stride = w * 4;
  std::vector<uint8_t> bgra(size_t(stride) * h);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = rgba[i + 2];
    bgra[i + 1] = rgba[i + 1];
    bgra[i + 2] = rgba[i];
    bgra[i + 3] = rgba[i + 3];
  }
  const fs::path tmp = fs::path(path).concat(L".tmp");
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
            IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(frame->WritePixels(h, stride, UINT(bgra.size()), bgra.data())) && SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit());
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (ok) {
    fs::rename(tmp, path, ec);
    ok = !ec;
  }
  if (!ok) fs::remove(tmp, ec);
  return ok;
}

}  // namespace pd::texpack
