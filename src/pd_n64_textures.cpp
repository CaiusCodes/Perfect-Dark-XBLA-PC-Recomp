// N64 textures: see pd_n64_textures.h.
#include "pd_n64_textures.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>

#include "pd_paths.h"

REXCVAR_DEFINE_BOOL(pd_n64_textures, false, "PerfectDark",
                    "Draw the N64 textures from your own N64 ROM (resources\\assets\\n64) instead of the remastered "
                    "ones; Plume only. The N64 Textures key switches it");
REXCVAR_DEFINE_STRING(pd_n64_textures_key, "G", "PerfectDark",
                      "The key(s) that switch between the N64 and the remastered graphics, comma-separated "
                      "(empty = none); Settings > Keyboard & Mouse > Switch Graphics");
REXCVAR_DEFINE_BOOL(pd_n64_textures_pad, true, "PerfectDark",
                    "A controller's D-pad Down switches between the N64 and the remastered graphics during play "
                    "(no control style uses the D-pad)");
REXCVAR_DEFINE_DOUBLE(pd_n64_texture_gamma, 1.17, "PerfectDark",
                      "N64 textures: gamma applied to their colours (1.17 = as the remaster converted the N64 art; "
                      "1 = the N64's own colours)");
REXCVAR_DEFINE_BOOL(pd_n64_models, true, "PerfectDark",
                    "With N64 textures on: draw the N64 models (characters, guns, props) instead of the remastered "
                    "ones. Drawing only: hits and collisions stay the game's own");
REXCVAR_DEFINE_BOOL(pd_n64_sky, true, "PerfectDark",
                    "With N64 textures on: draw the N64 sky (the game's own N64 sky code: colour, clouds, "
                    "water) instead of the remaster's skybox and distant backdrop");
REXCVAR_DEFINE_DOUBLE(pd_n64_texture_level, 0.98, "PerfectDark",
                      "N64 textures: brightness scale after the gamma (0.98 = as the remaster; 1 = none)");

namespace pd::n64tex {

namespace {

std::atomic<Status> g_status{Status::kIdle};
std::mutex g_mutex;  // g_detail
std::string g_detail;
std::vector<n64dec::Image> g_images;  // written by the loader before g_status becomes kReady
std::vector<uint8_t> g_rom;            // the ROM (.z64 order), likewise
std::vector<uint32_t> g_file_offsets;  // its file table
std::mutex g_files_mutex;
std::unordered_map<int, std::unique_ptr<std::vector<uint8_t>>> g_files;  // RomFile cache

void set_status(Status s, std::string detail) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_detail = std::move(detail);
  }
  g_status.store(s, std::memory_order_release);
}

std::string md5_hex(const std::vector<uint8_t>& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  uint8_t digest[16] = {};
  std::string out;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0) {
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
      if (BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0) == 0 &&
          BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
        static const char kHex[] = "0123456789abcdef";
        for (uint8_t b : digest) {
          out += kHex[b >> 4];
          out += kHex[b & 15];
        }
      }
      BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  return out;
}

// The ROM in RomFolder(): the first .z64 / .n64 / .v64 file by name. The test
// version of 2026-11-14 kept it in resources\assets\n64, which a new setup
// import replaces with the package's files: still looked in after RomFolder().
std::filesystem::path find_rom() {
  for (const std::filesystem::path& folder : {RomFolder(), pd_paths::DataRoot() / "assets" / "n64"}) {
    std::error_code ec;
    std::vector<std::filesystem::path> found;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
      if (!e.is_regular_file(ec)) continue;
      std::wstring ext = e.path().extension().wstring();
      for (wchar_t& c : ext) c = wchar_t(towlower(c));
      if (ext == L".z64" || ext == L".n64" || ext == L".v64") found.push_back(e.path());
    }
    std::sort(found.begin(), found.end());
    if (!found.empty()) return found.front();
  }
  return {};
}

void load() {
  const auto t0 = std::chrono::steady_clock::now();
  const std::filesystem::path path = find_rom();
  if (path.empty()) {
    REXLOG_INFO("[n64] no N64 ROM in {} (.z64, .n64 or .v64)", RomFolder().string());
    set_status(Status::kNoRom, "no ROM");
    return;
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size != 0x2000000) {
    REXLOG_WARN("[n64] {} is not a Perfect Dark ROM (size {} bytes, a Perfect Dark ROM is 32 MB)",
                path.filename().string(), ec ? 0 : uint64_t(size));
    set_status(Status::kBadRom, "not a Perfect Dark ROM");
    return;
  }
  std::vector<uint8_t> rom(static_cast<size_t>(size));
  {
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(rom.data()), std::streamsize(rom.size()))) {
      REXLOG_WARN("[n64] can't read {}", path.filename().string());
      set_status(Status::kFailed, "can't read the ROM");
      return;
    }
  }
  if (!n64dec::NormaliseRomByteOrder(rom)) {
    REXLOG_WARN("[n64] {} is not an N64 ROM", path.filename().string());
    set_status(Status::kBadRom, "not an N64 ROM");
    return;
  }
  const std::string md5 = md5_hex(rom);
  const n64dec::RomVersion* version = n64dec::FindRomVersion(md5);
  if (!version) {
    REXLOG_WARN("[n64] {}: md5 {} is not a Perfect Dark ROM this game knows (it takes NTSC 1.0, NTSC final and "
                "PAL final dumps)", path.filename().string(), md5);
    set_status(Status::kBadRom, "unknown ROM");
    return;
  }
  if (!version->texture_seg) {
    REXLOG_WARN("[n64] {} is Perfect Dark {}: its textures aren't supported yet", path.filename().string(),
                version->name);
    set_status(Status::kUnsupported, version->name);
    return;
  }
  std::vector<n64dec::Image> images;
  std::string error;
  const size_t count = n64dec::DecodeAll(rom, *version, images, &error);
  if (count != size_t(kTextureCount) || !error.empty()) {
    REXLOG_WARN("[n64] {} ({}): texture table not usable: {} textures{}{}", path.filename().string(), version->name,
                count, error.empty() ? "" : ", ", error);
    set_status(Status::kFailed, "texture table not usable");
    return;
  }
  size_t bytes = 0;
  for (const auto& im : images) bytes += im.rgba.size();
  g_images = std::move(images);
  // The ROM stays for the N64 model files (pd_n64_models.cpp), read on demand.
  g_file_offsets = n64dec::FileOffsets(rom, *version);
  if (g_file_offsets.empty()) REXLOG_WARN("[n64] {}: the file table wasn't found: N64 models that the remaster edited stay remastered", version->name);
  g_rom = std::move(rom);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[n64] {} loaded: Perfect Dark {}{}, {} textures ({:.1f} MB) in {:.0f} ms", path.filename().string(),
              version->name, version->tested ? "" : " (not tested with this version yet)", count,
              bytes / 1048576.0, ms);
  set_status(Status::kReady, version->name);
}

}  // namespace

std::filesystem::path RomFolder() { return pd_paths::DataRoot() / "n64"; }

bool RomPresent() { return !find_rom().empty(); }

void BeginLoad() {
  Status s = g_status.load(std::memory_order_acquire);
  // Retried while there was no ROM (the player may have added it since).
  if (s != Status::kIdle && s != Status::kNoRom) return;
  if (!g_status.compare_exchange_strong(s, Status::kLoading)) return;
  std::thread(load).detach();
}

void Reload() {
  // A ROM that loaded stays (the renderer reads its pictures); every other
  // outcome is tried again with the ROM now in the folder.
  Status s = g_status.load(std::memory_order_acquire);
  if (s == Status::kLoading || s == Status::kReady) return;
  if (!g_status.compare_exchange_strong(s, Status::kLoading)) return;
  std::thread(load).detach();
}

Status GetStatus() { return g_status.load(std::memory_order_acquire); }

std::string StatusDetail() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_detail;
}

const std::vector<uint8_t>* RomFile(int id) {
  if (g_status.load(std::memory_order_acquire) != Status::kReady || g_file_offsets.empty()) return nullptr;
  std::lock_guard<std::mutex> lock(g_files_mutex);
  auto it = g_files.find(id);
  if (it != g_files.end()) return it->second.get();
  auto file = std::make_unique<std::vector<uint8_t>>();
  if (!n64dec::ExtractFile(g_rom, g_file_offsets, id, *file)) file.reset();
  return (g_files[id] = std::move(file)).get();
}

namespace {

// Textures the N64 kept inside model files: the remaster made each a Textures.raw
// texture (id >= 3503) and the model's number (id << 17) | offset (pd_n64_models.cpp,
// tools/n64/models_embedded.py). id -> {N64 file, texconfig index}.
struct EmbeddedTexture {
  int file;
  int index;
  uint32_t number;
};
constexpr EmbeddedTexture kEmbedded[] = {
#include "pd_n64_embedded.inc"
};

const std::unordered_map<int, std::pair<int, int>>& model_texture_sources() {
  static const std::unordered_map<int, std::pair<int, int>> map = [] {
    std::unordered_map<int, std::pair<int, int>> m;
    for (const EmbeddedTexture& e : kEmbedded) m.emplace(int(e.number >> 17), std::make_pair(e.file, e.index));
    return m;
  }();
  return map;
}

std::mutex g_model_tex_mutex;
std::unordered_map<int, std::unique_ptr<n64dec::Image>> g_model_tex;  // decoded (null: couldn't be)

const n64dec::Image* model_texture(int id) {
  const auto& sources = model_texture_sources();
  const auto s = sources.find(id);
  if (s == sources.end()) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_model_tex_mutex);
    const auto it = g_model_tex.find(id);
    if (it != g_model_tex.end()) return it->second.get();
  }
  std::unique_ptr<n64dec::Image> im;
  if (const std::vector<uint8_t>* file = RomFile(s->second.first); file && file->size() >= 28) {
    const uint8_t* b = file->data();
    auto be32 = [&](size_t a) { return (uint32_t(b[a]) << 24) | (uint32_t(b[a + 1]) << 16) | (uint32_t(b[a + 2]) << 8) | b[a + 3]; };
    const int count = int16_t((b[22] << 8) | b[23]);
    const uint32_t configs = be32(24);
    const size_t at = size_t(configs - 0x05000000u) + size_t(s->second.second) * 12;
    if ((configs >> 24) == 5 && s->second.second < count && at + 12 <= file->size()) {
      const uint32_t number = be32(at);
      const size_t offset = number & 0xFFFFFF;
      const int w = b[at + 4], h = b[at + 5], fmt = b[at + 7], siz = b[at + 8];
      auto decoded = std::make_unique<n64dec::Image>();
      if ((number >> 24) == 5 && offset < file->size() &&
          n64dec::DecodeTmem(b + offset, file->size() - offset, fmt, siz, w, h, *decoded))
        im = std::move(decoded);
    }
  }
  std::lock_guard<std::mutex> lock(g_model_tex_mutex);
  auto& slot = g_model_tex[id];
  if (!slot && im) slot = std::move(im);
  return slot.get();
}

}  // namespace

bool IsModelTexture(int id) { return model_texture_sources().count(id) != 0; }

const n64dec::Image* Get(int id) {
  if (id < 0 || g_status.load(std::memory_order_acquire) != Status::kReady) return nullptr;
  if (size_t(id) >= g_images.size()) return model_texture(id);
  const n64dec::Image& im = g_images[size_t(id)];
  return im.rgba.empty() ? nullptr : &im;
}

bool Enabled() { return REXCVAR_GET(pd_n64_textures); }

void SetEnabled(bool on) { rex::cvar::SetFlagByName("pd_n64_textures", on ? "true" : "false"); }

bool Active() { return REXCVAR_GET(pd_n64_textures) && g_status.load(std::memory_order_acquire) == Status::kReady; }

bool ModelsActive() { return Active() && REXCVAR_GET(pd_n64_models); }

bool SkyActive() { return Active() && REXCVAR_GET(pd_n64_sky); }

bool RemasterOnlyModel(int file) {
  switch (file) {
    // The start-up logos: the remaster drew the Microsoft Game Studios and 4J Studios
    // logos over the N64 Nintendo logo models, and its own Rare logo over the N64 one
    // (whose animation doesn't fit the XBLA's intro: it came out tiny).
    case 221:   // PnintendologoZ
    case 222:   // Pnlogo2Z
    case 223:   // Pnlogo3Z
    case 224:   // PnlogoZ
    case 1376:  // PrarelogoZ
      return true;
    default:
      return false;
  }
}

const uint8_t* ColourLut(uint32_t* version) {
  static uint8_t lut[256];
  static double built_gamma = -1, built_level = -1;
  static uint32_t built_version = 0;
  const double gamma = std::clamp(double(REXCVAR_GET(pd_n64_texture_gamma)), 0.2, 5.0);
  const double level = std::clamp(double(REXCVAR_GET(pd_n64_texture_level)), 0.1, 2.0);
  if (gamma != built_gamma || level != built_level) {
    for (int v = 0; v < 256; ++v)
      lut[v] = uint8_t(std::clamp(std::lround(255.0 * level * std::pow(v / 255.0, gamma)), 0l, 255l));
    built_gamma = gamma;
    built_level = level;
    ++built_version;
  }
  if (version) *version = built_version;
  return lut;
}

}  // namespace pd::n64tex
