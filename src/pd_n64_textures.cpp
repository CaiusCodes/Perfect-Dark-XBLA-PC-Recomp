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
                    "ones; Plume only. Set at start from the Texture Pack slot shown (pd_texture_slot1/2)");
REXCVAR_DEFINE_STRING(pd_n64_textures_key, "G", "PerfectDark",
                      "The key(s) that switch between Texture Pack 1 and Texture Pack 2 (Settings > Graphics > "
                      "Texture Packs), comma-separated (empty = none); Settings > Keyboard & Mouse > Switch Graphics");
REXCVAR_DEFINE_BOOL(pd_n64_textures_pad, true, "PerfectDark",
                    "A controller's D-pad Down switches between Texture Pack 1 and Texture Pack 2 during play "
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
REXCVAR_DEFINE_BOOL(pd_n64_stars, true, "PerfectDark",
                    "With N64 textures on: draw the N64's star field in the stages that have one (Defection, "
                    "Extraction, Infiltration, Escape, Attack Ship); the remaster compiled it out");
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


// ---- A texture table with more entries than the XBLA numbers (the Japanese release) ----
// The Japanese ROM has 3511 textures, 8 more than the 3503 the XBLA numbers. Where they sit
// isn't documented, so the ROM's list is matched to the XBLA numbers by shape: the
// remaster's textures (Textures.raw's index, no decoding) keep their N64 picture's aspect
// ratio, mostly at a power-of-two multiple of its size. Dynamic programming over (ROM entry,
// entries left out so far) finds the `extra` entries to leave out that give the most
// matching shapes; map[xbla id] = ROM entry. HANDOVER 4du.
using Size = std::pair<uint32_t, uint32_t>;

int shape_score(const Size& n64, const Size& xbla) {
  if (!n64.first || !n64.second || !xbla.first || !xbla.second) return 0;
  if (uint64_t(n64.first) * xbla.second != uint64_t(n64.second) * xbla.first) return 0;
  const uint32_t k = xbla.first / n64.first;
  return xbla.first % n64.first == 0 && k && (k & (k - 1)) == 0 ? 2 : 1;
}

int align_by_shape(const std::vector<Size>& rom, const std::vector<Size>& xbla, std::vector<int>& map) {
  const int n = int(rom.size()), m = int(xbla.size()), k = n - m;
  map.assign(size_t(std::max(m, 0)), -1);
  if (k < 0 || k > 64 || m == 0) return -1;
  // dp[i][j]: the best score with i ROM entries seen, j of them left out (XBLA id i - j).
  const int W = k + 1;
  std::vector<int> dp(size_t(n + 1) * W, -1);
  std::vector<uint8_t> took(size_t(n + 1) * W, 0);  // 1: entry i-1 matched, 2: left out
  dp[0] = 0;
  for (int i = 0; i < n; ++i)
    for (int j = 0; j <= k; ++j) {
      const int here = dp[size_t(i) * W + j];
      if (here < 0) continue;
      const int x = i - j;
      if (x < m) {  // match ROM entry i with XBLA id x
        const int s = here + shape_score(rom[size_t(i)], xbla[size_t(x)]);
        int& to = dp[size_t(i + 1) * W + j];
        if (s > to) {
          to = s;
          took[size_t(i + 1) * W + j] = 1;
        }
      }
      if (j < k) {  // leave ROM entry i out
        int& to = dp[size_t(i + 1) * W + j + 1];
        if (here > to) {
          to = here;
          took[size_t(i + 1) * W + j + 1] = 2;
        }
      }
    }
  const int best = dp[size_t(n) * W + k];
  if (best < 0) return -1;
  for (int i = n, j = k; i > 0;) {
    const uint8_t t = took[size_t(i) * W + j];
    if (t == 1) map[size_t(i - 1 - j)] = i - 1;
    else if (t == 2) --j;
    else return -1;
    --i;
  }
  int matched = 0;
  for (int x = 0; x < m; ++x)
    if (map[size_t(x)] >= 0 && shape_score(rom[size_t(map[size_t(x)])], xbla[size_t(x)]) > 0) ++matched;
  return matched;
}

// Textures.raw's index: the remaster's size of each texture (ids 0..count-1).
bool xbla_sizes(std::vector<Size>& out) {
  const std::filesystem::path raw = pd_paths::DataRoot() / "assets" / "DataFiles" / "Textures.raw";
  std::ifstream in(raw, std::ios::binary);
  uint8_t head[4] = {};
  if (!in || !in.read(reinterpret_cast<char*>(head), 4)) return false;
  const uint32_t count = (uint32_t(head[0]) << 24) | (uint32_t(head[1]) << 16) | (uint32_t(head[2]) << 8) | head[3];
  if (!count || count > 65536) return false;
  std::vector<uint8_t> index(size_t(count) * 52);
  if (!in.read(reinterpret_cast<char*>(index.data()), std::streamsize(index.size()))) return false;
  auto be32 = [](const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  };
  out.resize(count);
  for (uint32_t id = 0; id < count; ++id) out[id] = {be32(&index[size_t(id) * 52 + 4]), be32(&index[size_t(id) * 52 + 8])};
  return true;
}

// Brings a decoded table with more entries than kTextureCount to the XBLA numbers. False
// (with `why`) when the shapes don't match well enough to trust the result.
bool align_images(std::vector<n64dec::Image>& images, std::string* why) {
  std::vector<Size> xbla;
  if (!xbla_sizes(xbla) || xbla.size() < size_t(kTextureCount)) {
    *why = "the game's Textures.raw couldn't be read to match the textures";
    return false;
  }
  xbla.resize(size_t(kTextureCount));
  std::vector<Size> rom;
  for (const auto& im : images) rom.push_back({im.w, im.h});
  std::vector<int> map;
  const int matched = align_by_shape(rom, xbla, map);
  int comparable = 0;
  for (int x = 0; x < kTextureCount; ++x)
    if (map[size_t(x)] >= 0 && images[size_t(map[size_t(x)])].w && xbla[size_t(x)].first) ++comparable;
  // The PAL ROM's own table, as it is, has 3202 of its 3503 shapes matching the remaster's
  // (91%: the remaster redrew some textures in other shapes, and reused 44 numbers); with 8
  // made-up entries put in, the matching put 3500-3503 of the 3503 numbers right (HANDOVER
  // 4du, n64aligntest). Under 85% the result is not trusted.
  if (matched < 0 || comparable == 0 || matched * 100 < comparable * 85) {
    *why = "its textures couldn't be matched to the remaster's numbers (" + std::to_string(std::max(matched, 0)) +
           " of " + std::to_string(comparable) + " shapes)";
    return false;
  }
  std::vector<int> left_out;
  std::vector<uint8_t> used(images.size(), 0);
  for (int x = 0; x < kTextureCount; ++x) used[size_t(map[size_t(x)])] = 1;
  for (size_t i = 0; i < used.size(); ++i)
    if (!used[i]) left_out.push_back(int(i));
  std::vector<n64dec::Image> out;
  out.resize(static_cast<size_t>(kTextureCount));
  for (int x = 0; x < kTextureCount; ++x) out[size_t(x)] = std::move(images[size_t(map[size_t(x)])]);
  images = std::move(out);
  std::string list;
  for (const int i : left_out) list += (list.empty() ? "" : ", ") + std::to_string(i);
  REXLOG_INFO("[n64] texture table matched to the XBLA numbers by shape: {} of {} shapes agree; ROM entries left "
              "out: {}", matched, comparable, list);
  return true;
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
  size_t count = n64dec::DecodeAll(rom, *version, images, &error);
  if (count > size_t(kTextureCount) && error.empty()) {
    // The Japanese release's longer table: matched to the XBLA numbers (align_images).
    std::string why;
    if (!align_images(images, &why)) {
      REXLOG_WARN("[n64] {} ({}): {}", path.filename().string(), version->name, why);
      set_status(Status::kUnsupported, version->name);
      return;
    }
    count = images.size();
  }
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

std::string AlignSelfTest() {
  if (g_status.load(std::memory_order_acquire) != Status::kReady) return "no N64 ROM loaded";
  std::vector<Size> xbla;
  if (!xbla_sizes(xbla) || xbla.size() < size_t(kTextureCount)) return "Textures.raw not readable";
  xbla.resize(size_t(kTextureCount));
  std::vector<Size> real;
  for (const auto& im : g_images) real.push_back({im.w, im.h});
  std::string out;
  {  // as it is: how many shapes agree
    std::vector<int> map;
    const int m = align_by_shape(real, xbla, map);
    int same = 0;
    for (int x = 0; x < kTextureCount; ++x) same += map[size_t(x)] == x;
    out += "as is: " + std::to_string(m) + " shapes agree, " + std::to_string(same) + " ids kept";
  }
  // 8 made-up entries put in, twice: odd sizes, then copies of the entry before (the worst
  // case: nothing tells them apart from their neighbour).
  const int at[8] = {120, 707, 1500, 2203, 2890, 3100, 3333, 3500};
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<Size> rom;
    std::vector<int> truth;  // ROM entry -> XBLA id (-1: made up)
    int next = 0;
    for (int x = 0; x < kTextureCount; ++x) {
      if (next < 8 && x == at[next]) {
        rom.push_back(pass == 0 ? Size{24, 40} : real[size_t(x - 1)]);
        truth.push_back(-1);
        ++next;
      }
      rom.push_back(real[size_t(x)]);
      truth.push_back(x);
    }
    std::vector<int> map;
    const int m = align_by_shape(rom, xbla, map);
    int right = 0;
    for (int x = 0; x < kTextureCount; ++x) right += map[size_t(x)] >= 0 && truth[size_t(map[size_t(x)])] == x;
    out += std::string(pass == 0 ? "; 8 odd entries put in: " : "; 8 copies put in: ") + std::to_string(m) +
           " shapes agree, " + std::to_string(right) + " of " + std::to_string(kTextureCount) + " ids right";
  }
  return out;
}

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

// Whether the model's display lists load in-file texture `number` with a palette:
// G_SETTIMG of the number, a G_LOADTLUT before the next G_SETTIMG, and the render
// tile (0) 4- or 8-bit; G_SETOTHERMODE_H TEXTLUT 0xC000 = IA16 palette. Some of
// these have an RGBA16 texconfig (the door's handles came out as noise:
// tools/n64/models_embedded_ci.py found 7, all matching the remaster's conversions).
bool palette_load(const uint8_t* b, size_t n, uint32_t number, int* bits, bool* ia) {
  auto be32 = [&](size_t a) { return (uint32_t(b[a]) << 24) | (uint32_t(b[a + 1]) << 16) | (uint32_t(b[a + 2]) << 8) | b[a + 3]; };
  for (size_t o = 0; o + 8 <= n; o += 8) {
    if ((be32(o) >> 24) != 0xFD || be32(o + 4) != number) continue;
    bool tlut = false, ia_mode = false;
    int siz = -1;
    for (size_t k = o + 8; k + 8 <= n && k < o + 8 * 48; k += 8) {
      const uint32_t w0 = be32(k), w1 = be32(k + 4);
      const uint32_t op = w0 >> 24;
      if (op == 0xFD) break;
      if (op == 0xF0) tlut = true;
      if (op == 0xBA && (w0 & 0xFFFF) == 0x0E02) ia_mode = w1 == 0xC000;
      if (op == 0xF5 && ((w1 >> 24) & 7) == 0 && siz < 0) siz = int((w0 >> 19) & 3);
    }
    if (tlut && (siz == 0 || siz == 1)) {
      *bits = 4 << siz;
      *ia = ia_mode;
      return true;
    }
  }
  return false;
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
      const int w = b[at + 4], h = b[at + 5], levels = b[at + 6], fmt = b[at + 7], siz = b[at + 8];
      auto decoded = std::make_unique<n64dec::Image>();
      int bits = 0;
      bool ia = false;
      if ((number >> 24) == 5 && offset < file->size()) {
        const bool ok = palette_load(b, file->size(), number, &bits, &ia)
                            ? n64dec::DecodeTmemCI(b + offset, file->size() - offset, bits, w, h, levels, ia, *decoded)
                            : n64dec::DecodeTmem(b + offset, file->size() - offset, fmt, siz, w, h, *decoded);
        if (ok) im = std::move(decoded);
      }
    }
  }
  std::lock_guard<std::mutex> lock(g_model_tex_mutex);
  auto& slot = g_model_tex[id];
  if (!slot && im) slot = std::move(im);
  return slot.get();
}

}  // namespace

bool IsModelTexture(int id) { return model_texture_sources().count(id) != 0; }

std::vector<int> ModelTextureIds() {
  std::vector<int> ids;
  for (const auto& [id, source] : model_texture_sources()) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

int ModelTextureAt(int file, uint32_t offset) {
  for (const EmbeddedTexture& e : kEmbedded)
    if (e.file == file && (e.number & 0x1FFFF) == offset) return int(e.number >> 17);
  return -1;
}

int RomFileByName(const std::string& name) {
  if (g_status.load(std::memory_order_acquire) != Status::kReady) return -1;
  // The ROM's own file name table (read from the player's ROM, nothing kept in the
  // game): u32 offsets from the table's start, one per file number (0 unused), then
  // the names. Found from its first name, which every version has; the entry for
  // file 1 is the offset that leads back to it.
  static const std::unordered_map<std::string, int> names = [] {
    std::unordered_map<std::string, int> m;
    const std::vector<uint8_t>& rom = g_rom;
    static const char kFirst[] = "bgdata/bg_sev.seg";
    auto be32 = [&](size_t a) {
      return (uint32_t(rom[a]) << 24) | (uint32_t(rom[a + 1]) << 16) | (uint32_t(rom[a + 2]) << 8) | rom[a + 3];
    };
    const auto it = std::search(rom.begin(), rom.end(), kFirst, kFirst + sizeof(kFirst) - 1);
    if (it == rom.end()) return m;
    const size_t first = size_t(it - rom.begin());
    size_t base = SIZE_MAX;
    for (size_t p = first & ~size_t(3); p >= 8 && first - p < 0x10000; p -= 4)
      if (be32(p) == first - (p - 4)) {
        base = p - 4;
        break;
      }
    if (base == SIZE_MAX) return m;
    for (int i = 1; i < 8192; ++i) {
      const size_t entry = base + size_t(i) * 4;
      if (entry + 4 > first) break;
      const size_t at = base + be32(entry);
      if (at < first || at >= rom.size()) break;
      size_t end = at;
      while (end < rom.size() && end - at < 64 && rom[end]) ++end;
      m.emplace(std::string(reinterpret_cast<const char*>(&rom[at]), end - at), i);
    }
    return m;
  }();
  const auto found = names.find(name);
  return found == names.end() ? -1 : found->second;
}

namespace {

// An in-file texture's hi-res checksum: the model file is RDRAM as loaded (TMEM
// layout already), so the checksum runs over its bytes at the texture's offset.
bool in_file_hires(int id, uint32_t& crc, HiresEntry& e) {
  const auto& sources = model_texture_sources();
  const auto s = sources.find(id);
  if (s == sources.end()) return false;
  const std::vector<uint8_t>* file = RomFile(s->second.first);
  if (!file || file->size() < 28) return false;
  const uint8_t* b = file->data();
  auto be32 = [&](size_t a) { return (uint32_t(b[a]) << 24) | (uint32_t(b[a + 1]) << 16) | (uint32_t(b[a + 2]) << 8) | b[a + 3]; };
  const int count = int16_t((b[22] << 8) | b[23]);
  const uint32_t configs = be32(24);
  const size_t at = size_t(configs - 0x05000000u) + size_t(s->second.second) * 12;
  if ((configs >> 24) != 5 || s->second.second >= count || at + 12 > file->size()) return false;
  const uint32_t number = be32(at);
  const size_t offset = number & 0xFFFFFF;
  int w = b[at + 4], h = b[at + 5], levels = b[at + 6], fmt = b[at + 7], siz = b[at + 8];
  if ((number >> 24) != 5 || !w || !h || offset >= file->size() || siz > 3) return false;
  int bits = 0;
  bool ia = false;
  const bool ci = palette_load(b, file->size(), number, &bits, &ia);
  if (ci) {
    fmt = 2;
    siz = bits == 4 ? 0 : 1;
  }
  const size_t row = siz == 3 ? ((size_t(w) * 4 + 7) & ~size_t(7)) : ((((size_t(w) << siz) >> 1) + 7) & ~size_t(7));
  crc = n64dec::HiresCrc(b + offset, file->size() - offset, w, h, siz, row);
  e = HiresEntry{id, uint8_t(fmt), uint8_t(siz), {}};
  if (ci) {
    // The palette follows the index data (every level).
    size_t index_bytes = 0;
    for (int l = 0; l < std::max(1, levels); ++l) {
      const size_t lw = size_t(std::max(1, w >> l)), lh = size_t(std::max(1, h >> l));
      index_bytes += (lw * size_t(bits) >= 8 ? ((lw * size_t(bits) / 8 + 7) & ~size_t(7)) : 8) * lh;
    }
    const size_t entries = bits == 4 ? 16 : 256;
    if (offset + index_bytes + entries * 2 <= file->size()) {
      std::vector<uint16_t> pal(256, 0);
      for (size_t i = 0; i < entries; ++i)
        pal[i] = uint16_t((b[offset + index_bytes + 2 * i] << 8) | b[offset + index_bytes + 2 * i + 1]);
      const size_t counts[4] = {16, 256, entries, entries};
      for (int k = 0; k < 4; ++k) e.palette[k] = n64dec::HiresPaletteCrc(pal.data(), counts[k]);
    }
  }
  return true;
}

std::mutex g_hires_mutex;
std::unique_ptr<std::unordered_multimap<uint32_t, HiresEntry>> g_hires;

}  // namespace

const std::unordered_multimap<uint32_t, HiresEntry>* HiresTable() {
  if (g_status.load(std::memory_order_acquire) != Status::kReady) return nullptr;
  std::lock_guard<std::mutex> lock(g_hires_mutex);
  if (g_hires) return g_hires.get();
  auto table = std::make_unique<std::unordered_multimap<uint32_t, HiresEntry>>();
  for (size_t id = 0; id < g_images.size(); ++id) {
    const n64dec::Image& im = g_images[id];
    if (im.rgba.empty() || !im.hires_crc) continue;
    HiresEntry e{int(id), im.gbi_fmt, im.gbi_siz, {}};
    std::copy(std::begin(im.hires_pal), std::end(im.hires_pal), std::begin(e.palette));
    table->emplace(im.hires_crc, e);
  }
  for (const int id : ModelTextureIds()) {
    uint32_t crc = 0;
    HiresEntry e{};
    if (in_file_hires(id, crc, e)) table->emplace(crc, e);
  }
  REXLOG_INFO("[n64] hi-res pack checksums: {} textures", table->size());
  g_hires = std::move(table);
  return g_hires.get();
}

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
bool StarsActive() { return Active() && REXCVAR_GET(pd_n64_stars); }

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
