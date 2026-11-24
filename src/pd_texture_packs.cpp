// Texture packs: see pd_texture_packs.h.
#include "pd_texture_packs.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/ui/image_decode.h>

#include <fmt/format.h>

#include "pd_assets_import.h"
#include "pd_n64_textures.h"
#include "pd_paths.h"

REXCVAR_DEFINE_STRING(pd_texture_pack, "", "PerfectDark",
                      "Experimental: the texture pack drawn over the game's textures, a folder name in "
                      "resources\\texture-packs (empty = none). Plume only");
REXCVAR_DEFINE_STRING(pd_texture_slot1, "", "PerfectDark",
                      "Experimental: Texture Pack 1, one of the two looks the Switch Graphics key switches between: "
                      "Remastered, N64 or a texture pack's name (empty = set from pd_n64_textures / pd_texture_pack "
                      "at start)");
REXCVAR_DEFINE_STRING(pd_texture_slot2, "", "PerfectDark",
                      "Experimental: Texture Pack 2, the other look: Remastered, N64, a texture pack's name, or None "
                      "(the Switch Graphics key then does nothing)");
REXCVAR_DEFINE_INT32(pd_texture_slot_active, 1, "PerfectDark",
                     "Experimental: which of Texture Pack 1 and 2 is shown (1 or 2)");
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
std::map<std::string, std::shared_ptr<const Index>> g_index_cache;  // IndexFor
Prepare g_prepare;                                                  // RequestPrepare
uint32_t g_prepare_serial = 0;
std::atomic<uint32_t> g_prepared_serial{0};
Standby g_standby;                          // SetStandby (under g_mutex)
uint32_t g_standby_serial = 0;
std::atomic<uint32_t> g_standby_ready{0};  // the standby serial Plume has ready (0: none)

bool SameIndex(const std::shared_ptr<const Index>& a, const std::shared_ptr<const Index>& b) {
  if (!a || !b) return !a && !b;
  return a->name == b->name && a->version == b->version;
}

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

// How many of the pack's pictures each look draws (Index::n64_base). The N64 look
// draws the N64 texture numbers and the N64 models' own (the remastered models, with
// the remaster's numbers, give way to the N64 ones there); the remastered look every
// number, but not a PC-port pack's top-folder picture for a reused number nor an N64
// emulator redraw. Per-model pictures count once the ROM has matched them.
// A PC-port pack with nothing for the remaster's own textures (no xbla\ folder, no
// picture only the remastered look draws) is an N64 pack even when the two counts
// tie (one without any of the 44 reused numbers).
void CountUse(Index& index) {
  const size_t n = index.IdCount();
  int remastered_only = 0;
  for (size_t i = 0; i < n; ++i) {
    const int id = int(i);
    const bool top = index.Picture(0, id) != nullptr, xbla = index.Picture(1, id) != nullptr;
    const bool own = index.Picture(2, id) != nullptr;  // <id>-xbla: the remastered look only
    if (!top && !xbla && !own) continue;
    const bool n64 = id < pd::n64tex::kTextureCount || pd::n64tex::IsModelTexture(id);
    if (n64 && (top || xbla)) ++index.used_n64;
    const bool top_over_remastered = top && !index.N64Only(id) && !(index.pc_port && RemasterReusedId(id));
    if (own || xbla || top_over_remastered) {
      ++index.used_remastered;
      if (!n64 || xbla || own) ++remastered_only;
    }
  }
  index.n64_base = index.used_n64 > index.used_remastered ||
                   (index.pc_port && remastered_only == 0 && index.used_n64 > 0);
}

// pd-pack.txt, the pack's own settings (Index::no_reflection has the format). Read with
// the index (a pack changed while the game runs is read again with it).
void ReadPackSettings(const fs::path& folder, Index* index) {
  std::ifstream in(folder / L"pd-pack.txt");
  if (!in) return;
  const auto trim = [](std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    const size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
  };
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (line_no == 1 && line.size() >= 3 && uint8_t(line[0]) == 0xEF) line = line.substr(3);  // UTF-8 BOM
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    std::string key = eq == std::string::npos ? line : trim(line.substr(0, eq));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<uint8_t>* ids = nullptr;
    int* ids_n = nullptr;
    if (key == "no_reflection") {
      ids = &index->no_reflection;
      ids_n = &index->no_reflection_n;
    } else if (key == "keep_original") {
      ids = &index->keep_original;
      ids_n = &index->keep_original_n;
    } else {
      REXLOG_WARN("[texpack] \"{}\" pd-pack.txt line {}: \"{}\" isn't a setting this version knows", index->name,
                  line_no, key);
      continue;
    }
    std::string list = eq == std::string::npos ? std::string() : line.substr(eq + 1);
    for (char& c : list)
      if (c == ',' || c == ';' || c == '\t') c = ' ';
    size_t pos = 0;
    while (pos < list.size()) {
      const size_t start = list.find_first_not_of(' ', pos);
      if (start == std::string::npos) break;
      size_t end = list.find(' ', start);
      if (end == std::string::npos) end = list.size();
      std::string word = list.substr(start, end - start);
      pos = end;
      // xbla/12A6 or 12A6.png: the picture's name, as in the pack's folders.
      if (const size_t slash = word.find_last_of("/\\"); slash != std::string::npos) word = word.substr(slash + 1);
      if (const size_t dot = word.find('.'); dot != std::string::npos) word = word.substr(0, dot);
      int base = index->pc_port ? 16 : 10;
      if (word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) {
        word = word.substr(2);
        base = 16;
      }
      char* stop = nullptr;
      const long id = word.empty() ? -1 : std::strtol(word.c_str(), &stop, base);
      if (word.empty() || *stop || id < 0 || id > 8191) {
        REXLOG_WARN("[texpack] \"{}\" pd-pack.txt line {}: \"{}\" isn't a texture number", index->name, line_no, word);
        continue;
      }
      if (size_t(id) >= ids->size()) ids->resize(size_t(id) + 1, 0);
      if (!(*ids)[size_t(id)]) ++*ids_n;
      (*ids)[size_t(id)] = 1;
    }
  }
  if (index->no_reflection_n)
    REXLOG_INFO("[texpack] \"{}\" pd-pack.txt: {} texture(s) drawn without reflections", index->name,
                index->no_reflection_n);
}

// keep_original (pd-pack.txt): the pack's pictures for those textures are left out of the
// index altogether, in both looks, so the game draws its own picture there and the look
// measurement (pd-look.txt, keyed on the picture count) counts without them.
void DropKeptOriginals(Index* index) {
  if (!index->keep_original_n) return;
  int dropped = 0;
  for (size_t id = 0; id < index->keep_original.size(); ++id) {
    if (!index->keep_original[id]) continue;
    if (id < index->by_id.size() && !index->by_id[id].empty()) {
      index->by_id[id].clear();
      ++dropped;
    }
    if (id < index->xbla_by_id.size() && !index->xbla_by_id[id].empty()) {
      index->xbla_by_id[id].clear();
      ++dropped;
    }
    if (id < index->remastered_by_id.size() && !index->remastered_by_id[id].empty()) {
      index->remastered_by_id[id].clear();
      --index->remastered_pictures;
      ++dropped;
    }
    if (id < index->n64_only.size()) index->n64_only[id] = 0;
  }
  index->pictures -= dropped;
  REXLOG_INFO("[texpack] \"{}\" pd-pack.txt: {} texture(s) keep the game's own picture ({} of the pack's pictures not used)",
              index->name, index->keep_original_n, dropped);
}

// <id>-reflect masks (pd_assets_import.h PictureSuffix): decoded once with the index, kept
// as luminance at most 256 on a side (only black / white matters, part by part).
void LoadReflectMasks(const std::vector<fs::path>& files, Index* index) {
  for (size_t id = 0; id < files.size(); ++id) {
    if (files[id].empty()) continue;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    std::string error;
    if (!LoadPicture(files[id], rgba, w, h, &error)) {
      REXLOG_WARN("[texpack] \"{}\" {}: not used: {}", index->name, files[id].filename().string(), error);
      continue;
    }
    auto m = std::make_shared<Index::ReflectMask>();
    const uint32_t step = std::max<uint32_t>(1, (std::max(w, h) + 255) / 256);
    m->w = (w + step - 1) / step;
    m->h = (h + step - 1) / step;
    m->lum.resize(size_t(m->w) * m->h);
    m->file = files[id].filename().string();
    for (uint32_t y = 0; y < m->h; ++y)
      for (uint32_t x = 0; x < m->w; ++x) {
        uint32_t sum = 0, n = 0;
        for (uint32_t yy = y * step; yy < std::min(h, (y + 1) * step); ++yy)
          for (uint32_t xx = x * step; xx < std::min(w, (x + 1) * step); ++xx) {
            const uint8_t* p = &rgba[(size_t(yy) * w + xx) * 4];
            sum += (uint32_t(p[0]) * 77 + uint32_t(p[1]) * 150 + uint32_t(p[2]) * 29) >> 8;
            ++n;
          }
        m->lum[size_t(y) * m->w + x] = uint8_t(n ? sum / n : 255);
      }
    if (id >= index->reflect_by_id.size()) index->reflect_by_id.resize(id + 1);
    index->reflect_by_id[id] = std::move(m);
    ++index->reflect_n;
  }
  if (index->reflect_n)
    REXLOG_INFO("[texpack] \"{}\": {} reflection mask(s) (-reflect pictures): parts of models on their black drawn "
                "without reflections", index->name, index->reflect_n);
}

std::shared_ptr<Index> BuildIndex(const std::string& name) {
  auto index = std::make_shared<Index>();
  index->name = name;
  if (name.empty()) return index;
  const fs::path folder = PacksFolder() / Wide(name);
  pd::assets::PackPictures scan = pd::assets::ScanPack(folder);
  index->pc_port = scan.pc_port;
  index->by_id = std::move(scan.top);
  index->xbla_by_id = std::move(scan.xbla);
  index->remastered_by_id = std::move(scan.remastered);
  index->ignored = scan.ignored;
  for (const auto& p : index->by_id) index->pictures += p.empty() ? 0 : 1;
  for (const auto& p : index->xbla_by_id) index->pictures += p.empty() ? 0 : 1;
  for (const auto& p : index->remastered_by_id) index->remastered_pictures += p.empty() ? 0 : 1;
  index->pictures += index->remastered_pictures;
  LoadReflectMasks(scan.reflect, index.get());
  ReadPackSettings(folder, index.get());
  std::vector<std::pair<pd::assets::HiresName, fs::path>> hires;
  for (const fs::path& f : scan.hires) {
    pd::assets::HiresName n;
    if (pd::assets::ParseHiresName(f, &n)) hires.emplace_back(n, f);
  }
  // PC-port per-model pictures: matched through the ROM's file names.
  index->model_files = int(scan.models.size());
  if (!scan.models.empty()) {
    if (pd::n64tex::GetStatus() != pd::n64tex::Status::kReady) {
      index->waiting_for_rom = true;
    } else {
      for (const pd::assets::ModelPicture& m : scan.models) {
        const int id = pd::n64tex::ModelTextureAt(pd::n64tex::RomFileByName(m.file), m.offset);
        if (id < 0) continue;
        if (size_t(id) >= index->by_id.size()) index->by_id.resize(size_t(id) + 1);
        if (!index->by_id[size_t(id)].empty()) continue;  // a picture named by id wins
        index->by_id[size_t(id)] = m.path;
        ++index->model_matched;
      }
    }
  }
  // N64 emulator naming: matched through the ROM's texture checksums.
  index->hires_files = int(hires.size());
  if (!hires.empty()) {
    const auto* table = pd::n64tex::HiresTable();
    if (!table) {
      index->waiting_for_rom = true;
    } else {
      std::sort(hires.begin(), hires.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
      std::unordered_multimap<uint32_t, size_t> by_crc;
      for (size_t i = 0; i < hires.size(); ++i) by_crc.emplace(hires[i].first.crc, i);
      std::vector<bool> used(hires.size(), false);
      for (const auto& [crc, entry] : *table) {
        // The best of the pack's pictures with this checksum: same format and size,
        // then a palette checksum the texture has.
        int best = -1, best_score = -1;
        const auto range = by_crc.equal_range(crc);
        for (auto it = range.first; it != range.second; ++it) {
          const pd::assets::HiresName& n = hires[it->second].first;
          int score = (n.fmt == entry.fmt && n.siz == entry.siz) ? 2 : (n.siz == entry.siz ? 1 : 0);
          if (n.has_palette)
            for (uint32_t p : entry.palette)
              if (p && p == n.palette) {
                score += 4;
                break;
              }
          if (score > best_score) {
            best_score = score;
            best = int(it->second);
          }
        }
        if (best < 0) continue;
        const size_t id = size_t(entry.id);
        if (id >= index->by_id.size()) index->by_id.resize(id + 1);
        if (id >= index->n64_only.size()) index->n64_only.resize(id + 1, 0);
        if (!index->by_id[id].empty()) continue;  // a picture named by id wins
        index->by_id[id] = hires[size_t(best)].second;
        index->n64_only[id] = 1;
        used[size_t(best)] = true;
      }
      for (bool u : used) index->hires_matched += u ? 1 : 0;
    }
  }
  DropKeptOriginals(index.get());
  CountUse(*index);
  return index;
}

}  // namespace

fs::path PacksFolder() { return pd_paths::DataRoot() / "texture-packs"; }

// The game's vertex texture coordinates land on a PC-port pack's picture as it is stored (right
// way up: measured on Joanna's suit, HANDOVER 4dw); a decimal pack's pictures are stored the
// other way up, so v is turned over for them.
uint8_t Index::ReflectMask::At(float u, float v, bool pc_port) const {
  if (!w || !h || !std::isfinite(u) || !std::isfinite(v)) return 255;
  u -= std::floor(u);
  v -= std::floor(v);
  if (!pc_port) v = 1.0f - v;
  const uint32_t x = std::min(w - 1, uint32_t(u * float(w)));
  const uint32_t y = std::min(h - 1, uint32_t(v * float(h)));
  return lum[size_t(y) * w + x];
}

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

namespace {

bool RomMatched() { return pd::n64tex::GetStatus() == pd::n64tex::Status::kReady && pd::n64tex::HiresTable(); }

void LogChosen(const Index& idx, bool n64) {
  const Index* index = &idx;
  const std::string& name = idx.name;
  int count = 0, xbla = 0;
  for (const auto& p : index->by_id) count += p.empty() ? 0 : 1;
  for (const auto& p : index->xbla_by_id) xbla += p.empty() ? 0 : 1;
  count += xbla;
  if (!name.empty())
    REXLOG_INFO("[texpack] \"{}\" chosen: {} pictures in {}{}{}", name, count, Utf8((PacksFolder() / Wide(name)).wstring()),
                index->pc_port ? fmt::format(" (PC-port naming: {} in the top folder, {} in xbla, {} in other folders "
                                             "not used)",
                                             count - xbla, xbla, index->ignored)
                               : std::string(" (decimal naming)"),
                (index->model_files == 0 ? std::string()
                 : index->waiting_for_rom
                     ? "; " + std::to_string(index->model_files) + " per-model pictures wait for the N64 ROM"
                     : "; per-model pictures: " + std::to_string(index->model_matched) + " of " +
                           std::to_string(index->model_files) + " matched to textures") +
                (index->hires_files == 0 ? std::string()
                : index->waiting_for_rom
                    ? "; " + std::to_string(index->hires_files) + " N64 emulator pictures wait for the N64 ROM"
                    : "; N64 emulator pictures: " + std::to_string(index->hires_matched) + " of " +
                          std::to_string(index->hires_files) + " matched to textures") +
                (index->remastered_pictures
                     ? fmt::format("; {} pictures with -xbla used{}", index->remastered_pictures,
                                   n64 ? " (in the remastered look only, so not now)" : "")
                     : std::string()) +
                fmt::format("; drawn over the {} look", n64 ? "N64" : "remastered"));
  else
    REXLOG_INFO("[texpack] no texture pack");
}

}  // namespace

std::shared_ptr<const Index> IndexFor(const std::string& name) {
  std::shared_ptr<const Index> old;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (name.empty()) return g_index && g_index->name.empty() ? g_index : std::make_shared<const Index>();
    const auto it = g_index_cache.find(name);
    if (it != g_index_cache.end()) {
      // Built before the N64 ROM was loaded (its per-model / emulator pictures unmatched): built again.
      if (!it->second->waiting_for_rom || !RomMatched()) return it->second;
      old = it->second;
    }
  }
  std::shared_ptr<Index> index = BuildIndex(name);  // (a directory listing: outside the lock)
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_index_cache.find(name);
  if (it != g_index_cache.end() && it->second != old) return it->second;  // (another thread built it meanwhile)
  index->version = ++g_version;
  if (old) index->keep_from = old->version;
  g_index_cache[name] = index;
  return index;
}

int ChoosePack(const std::string& name) {
  std::shared_ptr<const Index> index = IndexFor(name);
  SetLook(index, pd::n64tex::Enabled());
  return index->pictures;
}

Look CurrentLook() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return {g_index, pd::n64tex::Enabled()};
}

void SetLook(std::shared_ptr<const Index> index, bool n64) {
  if (!index) index = IndexFor("");
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    changed = !g_index || g_index->version != index->version || g_index->name != index->name;
    g_index = index;
    if (REXCVAR_GET(pd_texture_pack) != index->name) rex::cvar::SetFlagByName("pd_texture_pack", index->name);
    if (pd::n64tex::Enabled() != n64) pd::n64tex::SetEnabled(n64);
  }
  if (changed) LogChosen(*index, n64);
}

Looks CurrentLooks() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return {{g_index, pd::n64tex::Enabled()}, g_standby};
}

Standby CurrentStandby() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_standby;
}

void SetStandby(std::shared_ptr<const Index> index, bool n64, bool valid) {
  if (valid && !index) index = IndexFor("");
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!valid && !g_standby.valid) return;
  if (valid && g_standby.valid && g_standby.n64 == n64 && SameIndex(g_standby.index, index)) return;
  g_standby = {valid ? std::move(index) : nullptr, valid && n64, valid, ++g_standby_serial};
}

bool SwapToStandby(const std::shared_ptr<const Index>& index, bool n64) {
  bool changed = false;
  std::shared_ptr<const Index> shown;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_standby.valid || g_standby.n64 != n64 || !SameIndex(g_standby.index, index) ||
        g_standby_ready.load() != g_standby.serial)
      return false;
    const bool was_n64 = pd::n64tex::Enabled();
    const std::shared_ptr<const Index> old = g_index;
    changed = !SameIndex(g_index, g_standby.index);
    g_index = g_standby.index;
    if (REXCVAR_GET(pd_texture_pack) != g_index->name) rex::cvar::SetFlagByName("pd_texture_pack", g_index->name);
    if (was_n64 != n64) pd::n64tex::SetEnabled(n64);
    // The look shown until now becomes the standby (G back is at once too); ready as it was
    // being drawn (the renderer keeps its pictures as the standby's: same serial rule).
    g_standby = {old, was_n64, true, ++g_standby_serial};
    shown = g_index;
  }
  if (changed) LogChosen(*shown, n64);
  return true;
}

void MarkStandbyReady(uint32_t serial, bool ready) {
  if (ready) g_standby_ready.store(serial);
  else {
    uint32_t expected = serial;
    g_standby_ready.compare_exchange_strong(expected, 0u);
  }
}

bool StandbyReady(const std::shared_ptr<const Index>& index, bool n64) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_standby.valid && g_standby.n64 == n64 && SameIndex(g_standby.index, index) &&
         g_standby_ready.load() == g_standby.serial;
}

uint32_t RequestPrepare(std::shared_ptr<const Index> index, bool n64) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_prepare = {std::move(index), n64, ++g_prepare_serial};
  return g_prepare.serial;
}

void CancelPrepare() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_prepare = {nullptr, false, ++g_prepare_serial};
}

Prepare PendingPrepare() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_prepare;
}

void MarkPrepared(uint32_t serial) { g_prepared_serial.store(serial); }

uint32_t PreparedSerial() { return g_prepared_serial.load(); }

bool RebuildIfWaiting() {
  std::shared_ptr<const Index> index = CurrentIndex();
  if (!index || !index->waiting_for_rom || !RomMatched()) return false;
  // (IndexFor builds it again, keep_from the old one: the pictures shown stay)
  SetLook(IndexFor(index->name), pd::n64tex::Enabled());
  return true;
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

bool RemasterReusedId(int id) {
  static constexpr int16_t kIds[] = {123,  165,  169,  170,  212,  497,  498,  511,  534,  535,  537,
                                     538,  539,  545,  546,  547,  548,  550,  551,  559,  560,  1784,
                                     2206, 2207, 2210, 2211, 2220, 2221, 2232, 2235, 2239, 2240, 2241,
                                     2243, 2359, 2464, 2504, 2553, 2554, 2587, 2708, 2901, 2966, 3323};
  return std::binary_search(std::begin(kIds), std::end(kIds), int16_t(id));
}

std::string Slot(int slot) { return slot == 2 ? REXCVAR_GET(pd_texture_slot2) : REXCVAR_GET(pd_texture_slot1); }

void SetSlot(int slot, const std::string& value) {
  rex::cvar::SetFlagByName(slot == 2 ? "pd_texture_slot2" : "pd_texture_slot1", value);
}

int ActiveSlot() { return REXCVAR_GET(pd_texture_slot_active) == 2 ? 2 : 1; }

void SetActiveSlot(int slot) { rex::cvar::SetFlagByName("pd_texture_slot_active", slot == 2 ? "2" : "1"); }

namespace {

bool JpegSize(const std::vector<uint8_t>& b, uint32_t& w, uint32_t& h);  // (below)

std::atomic<RemasteredSizeFn> g_remastered_size{nullptr};
std::atomic<BankReadyFn> g_bank_ready{nullptr};
struct LookMeasure {
  int state = 0;  // 0 not measured (or not read yet), 1 measuring, 2 done
  bool n64 = false;
  bool rom = true;     // the N64 side measured (a ROM was loaded)
  int pictures = -1;   // the pack's pictures then (-1: not recorded, pd-look.txt from 0.9.2a-test3)
};
std::mutex g_look_mutex;
std::map<std::string, LookMeasure> g_looks;
std::atomic<uint32_t> g_look_generation{0};

// The measurement is kept in the pack's folder (pd-look.txt: "N64" or "Remastered", then
// the two counts, then "rom" / "no-rom" and the pictures counted), so it is made once per
// pack, not every session; a pack imported or downloaded again (ForgetLook) loses it.
fs::path LookFile(const std::string& name) { return PacksFolder() / Wide(name) / L"pd-look.txt"; }

// pd-look.txt into `m` (state 2), if it is there and readable.
bool ReadLookFile(const std::string& name, LookMeasure& m) {
  std::ifstream f(LookFile(name), std::ios::binary);
  std::string look, counts, third;
  if (!f || !std::getline(f, look)) return false;
  if (!look.empty() && look.back() == '\r') look.pop_back();
  if (look != "N64" && look != "Remastered") return false;
  std::getline(f, counts);
  m = {2, look == "N64", true, -1};
  if (std::getline(f, third)) {
    char rom[16] = {};
    int pictures = -1;
    if (std::sscanf(third.c_str(), "%15s %d", rom, &pictures) >= 1) {
      m.rom = std::strcmp(rom, "rom") == 0;
      m.pictures = pictures;
    }
  }
  return true;
}

// Measures which look draws more of pack `name`'s pictures (PackPrefersN64); with_rom:
// the N64 ROM is loaded (else only the remastered side counts).
void MeasureLook(const std::string& name, bool with_rom) {
  const auto t0 = std::chrono::steady_clock::now();
  const std::shared_ptr<const Index> index = IndexFor(name);
  const RemasteredSizeFn rem_size = g_remastered_size.load();
  // A picture's size from its header (the first 64 KB; a JPEG with a long header whole).
  const auto picture_size = [](std::vector<uint8_t>& head, const fs::path& p, uint32_t& w, uint32_t& h) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    head.assign(64 << 10, 0);
    f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
    head.resize(size_t(f.gcount()));
    if (head.size() >= 24 && head[0] == 0x89 && head[1] == 'P' && std::memcmp(&head[12], "IHDR", 4) == 0) {
      w = (uint32_t(head[16]) << 24) | (uint32_t(head[17]) << 16) | (uint32_t(head[18]) << 8) | head[19];
      h = (uint32_t(head[20]) << 24) | (uint32_t(head[21]) << 16) | (uint32_t(head[22]) << 8) | head[23];
      return w && h;
    }
    if (JpegSize(head, w, h)) return true;
    std::error_code ec;
    if (fs::file_size(p, ec) <= head.size() || ec) return false;
    std::ifstream all(p, std::ios::binary);
    head.assign(std::istreambuf_iterator<char>(all), std::istreambuf_iterator<char>());
    return JpegSize(head, w, h);
  };
  // The ids in turn on a few threads (a cold disk: reading thousands of headers one by one
  // took 12 s for a 4500-picture pack).
  std::atomic<int> used_n64{0}, used_rem{0};
  std::atomic<size_t> next{0};
  const size_t n = index->IdCount();
  const auto work = [&]() {
    std::vector<uint8_t> head;
    for (size_t i; (i = next.fetch_add(1)) < n;) {
      const int id = int(i);
      const fs::path* top = index->Picture(0, id);
      const fs::path* xbla = index->Picture(1, id);
      const fs::path* own = index->Picture(2, id);  // <id>-xbla: the remastered look only
      if (!top && !xbla && !own) continue;
      uint32_t tw = 0, th = 0, xw = 0, xh = 0, ow = 0, oh = 0;
      const bool top_ok = top && picture_size(head, *top, tw, th);
      const bool xbla_ok = xbla && picture_size(head, *xbla, xw, xh);
      const bool own_ok = own && picture_size(head, *own, ow, oh);
      // The N64 look: the N64 numbers and the N64 models' own, the top folder's first.
      if (with_rom && (id < pd::n64tex::kTextureCount || pd::n64tex::IsModelTexture(id))) {
        if (const pd::n64dec::Image* im = pd::n64tex::Get(id))
          if ((top_ok && SameShape(tw, th, im->w, im->h)) || (xbla_ok && SameShape(xw, xh, im->w, im->h))) ++used_n64;
      }
      // The remastered look: xbla\'s first, the top folder's but a reused number / an N64 redraw.
      uint32_t rw = 0, rh = 0;
      if (rem_size && rem_size(id, &rw, &rh)) {
        const bool top_here = top_ok && !index->N64Only(id) && !(index->pc_port && RemasterReusedId(id));
        if ((own_ok && SameShape(ow, oh, rw, rh)) || (xbla_ok && SameShape(xw, xh, rw, rh)) ||
            (top_here && SameShape(tw, th, rw, rh)))
          ++used_rem;
      }
    }
  };
  {
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) threads.emplace_back(work);
    work();
    for (std::thread& t : threads) t.join();
  }
  // Without a ROM the N64 look can't be drawn: the remastered (measured again with one).
  const bool n64 = with_rom && used_n64.load() > used_rem.load();
  {
    std::ofstream f(LookFile(name), std::ios::binary | std::ios::trunc);
    f << (n64 ? "N64" : "Remastered") << '\n'
      << used_n64.load() << ' ' << used_rem.load() << '\n'
      << (with_rom ? "rom" : "no-rom") << ' ' << index->pictures << '\n';
  }
  {
    std::lock_guard<std::mutex> lock(g_look_mutex);
    g_looks[name] = {2, n64, with_rom, index->pictures};
  }
  ++g_look_generation;
  REXLOG_INFO("[texpack] \"{}\" measured: {} of its pictures drawn in the {}, {} in the remastered; made for the "
              "{} look (by count it was {}) ({} ms)",
              name, used_n64.load(), with_rom ? "N64 look" : "N64 look (no N64 ROM: not counted)", used_rem.load(),
              n64 ? "N64" : "remastered", index->n64_base ? "N64" : "remastered",
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

// Under g_look_mutex: `m` read from pd-look.txt if not yet, and whether it is still good.
bool LookKnownLocked(const std::string& name, LookMeasure& m) {
  if (m.state == 0 && !ReadLookFile(name, m)) return false;
  if (m.state != 2) return false;
  // Measured without a ROM and there is one now (loaded, or there to load): the N64 side
  // wasn't counted.
  if (!m.rom) {
    using pd::n64tex::Status;
    const Status s = pd::n64tex::GetStatus();
    if (s == Status::kReady || ((s == Status::kIdle || s == Status::kLoading) && pd::n64tex::RomPresent())) return false;
  }
  return true;
}

}  // namespace

void SetRemasteredSizeSource(RemasteredSizeFn size, BankReadyFn ready) {
  g_remastered_size = size;
  g_bank_ready = ready;
}

uint32_t LookGeneration() { return g_look_generation.load(); }

LookState GetLookState(const std::string& name) {
  if (name.empty()) return LookState::kKnown;
  int pictures = -1;
  {
    std::lock_guard<std::mutex> lock(g_look_mutex);
    LookMeasure& m = g_looks[name];
    if (m.state == 1) return LookState::kMeasuring;
    if (!LookKnownLocked(name, m)) return LookState::kUnknown;
    pictures = m.pictures;
  }
  // The pack changed by hand since (pictures added or taken away): measured again.
  if (pictures >= 0 && IndexFor(name)->pictures != pictures) return LookState::kUnknown;
  return LookState::kKnown;
}

bool StartMeasure(const std::string& name) {
  if (name.empty()) return false;
  const BankReadyFn ready = g_bank_ready.load();
  if (!ready || !ready() || !g_remastered_size.load()) return false;
  using pd::n64tex::Status;
  const Status s = pd::n64tex::GetStatus();
  // A ROM there but not loaded yet: wait for it (the N64 side counts).
  if ((s == Status::kIdle || s == Status::kLoading) && pd::n64tex::RomPresent()) return false;
  const bool with_rom = s == Status::kReady;
  {
    std::lock_guard<std::mutex> lock(g_look_mutex);
    LookMeasure& m = g_looks[name];
    if (m.state == 1) return true;
    m.state = 1;
  }
  std::thread([name, with_rom]() { MeasureLook(name, with_rom); }).detach();
  return true;
}

bool PackPrefersN64(const std::string& name) {
  if (name.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(g_look_mutex);
    LookMeasure& m = g_looks[name];
    if (m.state == 0) ReadLookFile(name, m);
    if (m.state == 2) return m.n64;
  }
  return IndexFor(name)->n64_base;
}

void ForgetLook(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(g_look_mutex);
    const auto it = g_looks.find(name);
    if (it != g_looks.end() && it->second.state != 1) g_looks.erase(it);
    std::error_code ec;
    fs::remove(LookFile(name), ec);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_index_cache.erase(name);
}

namespace {

// A JPEG's size from its frame header (SOFn), without decoding it.
bool JpegSize(const std::vector<uint8_t>& b, uint32_t& w, uint32_t& h) {
  if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
  size_t p = 2;
  while (p + 4 <= b.size()) {
    if (b[p] != 0xFF) return false;
    const uint8_t m = b[p + 1];
    if (m == 0xFF) {  // fill byte
      ++p;
      continue;
    }
    p += 2;
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;  // markers without a length
    if (p + 2 > b.size()) return false;
    const size_t len = (size_t(b[p]) << 8) | b[p + 1];
    if (len < 2) return false;
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (p + 7 > b.size()) return false;
      h = (uint32_t(b[p + 3]) << 8) | b[p + 4];
      w = (uint32_t(b[p + 5]) << 8) | b[p + 6];
      return w && h;
    }
    p += len;
  }
  return false;
}

// Any picture Windows can read (used for JPEG) as RGBA8, through WIC.
bool DecodeWic(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
  // WIC wants COM on this thread (the texture worker).
  thread_local const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  (void)com;
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* convert = nullptr;
  UINT uw = 0, uh = 0;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), DWORD(bytes.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&uw, &uh)) && uw && uh &&
            uw <= 4096 && uh <= 4096 && SUCCEEDED(factory->CreateFormatConverter(&convert)) &&
            SUCCEEDED(convert->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom));
  if (ok) {
    rgba.assign(size_t(uw) * uh * 4, 0);
    ok = SUCCEEDED(convert->CopyPixels(nullptr, uw * 4, UINT(rgba.size()), rgba.data()));
  }
  if (convert) convert->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (!ok) {
    rgba.clear();
    return false;
  }
  w = uw;
  h = uh;
  return true;
}

void FlipRows(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  const size_t row = size_t(w) * 4;
  for (uint32_t y = 0; y < h / 2; ++y)
    std::swap_ranges(rgba.begin() + std::ptrdiff_t(y * row), rgba.begin() + std::ptrdiff_t((y + 1) * row),
                     rgba.begin() + std::ptrdiff_t((h - 1 - y) * row));
}

}  // namespace

bool SameShape(uint32_t w, uint32_t h, uint32_t want_w, uint32_t want_h) {
  if (!w || !h || !want_w || !want_h) return true;
  // |log2 of the ratio of the aspect ratios| below 0.2: the shapes made for these
  // numbers are either within ~10% (odd remaster sizes like 172x256) or 2x apart.
  const double r = (double(w) / double(h)) / (double(want_w) / double(want_h));
  return std::fabs(std::log2(r)) < 0.2;
}

const DetailLayer* DetailLayerFor(int id) {
  // tools/n64/bg_detail_tiles.py: ame (Defection) and Pdd_liftrZ: 0xC0 sub 1, 70 + 116,
  // shifts 15/15 (the detail at twice the rate: 64 / (32 / 2) = 4 repeats), min 240;
  // mp15 (Grid): 3494 / 3496 + 116 shifts 14/14 (32 / (32 / 4) = 4), min 248; 3497 (64x16)
  // + 116 shifts 15/13 (4 and 16 / (32 / 8) = 4), min 249. Close up the RDP's LOD fraction
  // is the prim LOD min (its low 5 bits / 32) and TRILERP gives the detail 1 - that.
  static const DetailLayer kLayers[] = {
      {70, 116, 4, 4, 0.5f},
      {3494, 116, 4, 4, 0.25f},
      {3496, 116, 4, 4, 0.25f},
      {3497, 116, 4, 4, 1.f - 25.f / 32.f},
  };
  for (const DetailLayer& d : kLayers)
    if (d.base == id) return &d;
  return nullptr;
}

namespace {

// Bilinear sample of an RGBA8 picture with wrapping, at texel coordinates (u, v).
void SampleWrap(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, float u, float v, float out[4]) {
  u -= 0.5f;
  v -= 0.5f;
  const float fu = std::floor(u), fv = std::floor(v);
  const float au = u - fu, av = v - fv;
  auto wrap = [](int64_t i, uint32_t n) { return uint32_t(((i % int64_t(n)) + int64_t(n)) % int64_t(n)); };
  const uint32_t x0 = wrap(int64_t(fu), w), x1 = wrap(int64_t(fu) + 1, w);
  const uint32_t y0 = wrap(int64_t(fv), h), y1 = wrap(int64_t(fv) + 1, h);
  const uint8_t* p00 = &img[(size_t(y0) * w + x0) * 4];
  const uint8_t* p10 = &img[(size_t(y0) * w + x1) * 4];
  const uint8_t* p01 = &img[(size_t(y1) * w + x0) * 4];
  const uint8_t* p11 = &img[(size_t(y1) * w + x1) * 4];
  for (int c = 0; c < 4; ++c)
    out[c] = (p00[c] * (1 - au) + p10[c] * au) * (1 - av) + (p01[c] * (1 - au) + p11[c] * au) * av;
}

// The picture at `scale` times its size (bilinear, edges wrapped as the texture repeats).
std::vector<uint8_t> Enlarge(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t scale) {
  const uint32_t nw = w * scale, nh = h * scale;
  std::vector<uint8_t> out(size_t(nw) * nh * 4);
  float px[4];
  for (uint32_t y = 0; y < nh; ++y)
    for (uint32_t x = 0; x < nw; ++x) {
      SampleWrap(img, w, h, (x + 0.5f) / float(scale), (y + 0.5f) / float(scale), px);
      for (int c = 0; c < 4; ++c) out[(size_t(y) * nw + x) * 4 + c] = uint8_t(std::clamp(px[c] + 0.5f, 0.f, 255.f));
    }
  return out;
}

// The picture box-averaged down to tw x th (each output texel the mean of its area).
std::vector<uint8_t> Shrink(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t tw, uint32_t th) {
  std::vector<uint8_t> out(size_t(tw) * th * 4);
  for (uint32_t y = 0; y < th; ++y) {
    const uint32_t y0 = uint32_t(uint64_t(y) * h / th), y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * h / th));
    for (uint32_t x = 0; x < tw; ++x) {
      const uint32_t x0 = uint32_t(uint64_t(x) * w / tw), x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * w / tw));
      uint32_t sum[4] = {};
      for (uint32_t yy = y0; yy < y1; ++yy)
        for (uint32_t xx = x0; xx < x1; ++xx)
          for (int c = 0; c < 4; ++c) sum[c] += img[(size_t(yy) * w + xx) * 4 + c];
      const uint32_t n = (y1 - y0) * (x1 - x0);
      for (int c = 0; c < 4; ++c) out[(size_t(y) * tw + x) * 4 + c] = uint8_t((sum[c] + n / 2) / n);
    }
  }
  return out;
}

}  // namespace

void BakeDetail(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, const std::vector<uint8_t>& detail,
                uint32_t dw, uint32_t dh, const DetailLayer& d) {
  if (!w || !h || !dw || !dh || rgba.size() < size_t(w) * h * 4 || detail.size() < size_t(dw) * dh * 4) return;
  // Enlarged (whole steps) until a repeat of the detail has its own resolution, at most 4096.
  uint32_t scale = 1;
  while (scale < 4 && w * scale < uint32_t(d.repeat_s) * dw && w * scale * 2 <= 4096 && h * scale * 2 <= 4096)
    scale *= 2;
  if (scale > 1) {
    rgba = Enlarge(rgba, w, h, scale);
    w *= scale;
    h *= scale;
  }
  // One repeat of the detail at the size it covers (box-averaged when that is smaller).
  const uint32_t tw = std::max<uint32_t>(1, (w + uint32_t(d.repeat_s) / 2) / uint32_t(d.repeat_s));
  const uint32_t th = std::max<uint32_t>(1, (h + uint32_t(d.repeat_t) / 2) / uint32_t(d.repeat_t));
  std::vector<uint8_t> tile;
  uint32_t tile_w = dw, tile_h = dh;
  if (tw < dw || th < dh) {
    tile = Shrink(detail, dw, dh, std::min(tw, dw), std::min(th, dh));
    tile_w = std::min(tw, dw);
    tile_h = std::min(th, dh);
  }
  const std::vector<uint8_t>& t = tile.empty() ? detail : tile;
  // The N64's TRILERP close up is detail + (base - detail) * LOD fraction: the detail's
  // grain plus a pull towards its average grey, which fades as the LOD fraction rises with
  // distance. Drawn here is the grain alone (the detail's difference from its own average,
  // at its close-up share), so the surface keeps its colours and lines at every distance
  // (a constant 50% mix greyed the carpet's dark lines everywhere). Alpha: the surface's own.
  double mean[3] = {};
  const size_t texels = size_t(tile_w) * tile_h;
  for (size_t i = 0; i < texels; ++i)
    for (int c = 0; c < 3; ++c) mean[c] += t[i * 4 + c];
  for (double& m : mean) m /= double(std::max<size_t>(1, texels));
  const float k = std::clamp(d.weight, 0.f, 1.f);
  float px[4];
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      SampleWrap(t, tile_w, tile_h, (x + 0.5f) * float(d.repeat_s) * float(tile_w) / float(w),
                 (y + 0.5f) * float(d.repeat_t) * float(tile_h) / float(h), px);
      uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
      for (int c = 0; c < 3; ++c)
        p[c] = uint8_t(std::clamp(p[c] + (px[c] - float(mean[c])) * k + 0.5f, 0.f, 255.f));
    }
}

bool LoadPicture(const fs::path& path, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, std::string* error,
                 bool flip, uint32_t want_w, uint32_t want_h, bool* wrong_shape) {
  constexpr uint32_t kMaxSide = 4096;
  if (wrong_shape) *wrong_shape = false;
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
  // The size from the header first, so a huge picture (or one of another shape) is
  // refused before anything is allocated for it.
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  const bool png = bytes.size() >= 24 && std::memcmp(bytes.data(), kSig, 8) == 0 && std::memcmp(&bytes[12], "IHDR", 4) == 0;
  uint32_t hw = 0, hh = 0;
  if (png) {
    hw = (uint32_t(bytes[16]) << 24) | (uint32_t(bytes[17]) << 16) | (uint32_t(bytes[18]) << 8) | bytes[19];
    hh = (uint32_t(bytes[20]) << 24) | (uint32_t(bytes[21]) << 16) | (uint32_t(bytes[22]) << 8) | bytes[23];
  } else if (!JpegSize(bytes, hw, hh)) {
    *error = "not a PNG or JPEG";
    return false;
  }
  if (!hw || !hh || hw > kMaxSide || hh > kMaxSide) {
    *error = "its size " + std::to_string(hw) + "x" + std::to_string(hh) + " is over 4096x4096";
    return false;
  }
  if (!SameShape(hw, hh, want_w, want_h)) {
    *error = "its shape " + std::to_string(hw) + "x" + std::to_string(hh) + " doesn't fit the texture's " +
             std::to_string(want_w) + "x" + std::to_string(want_h);
    if (wrong_shape) *wrong_shape = true;
    return false;
  }
  if (png) {
    int iw = 0, ih = 0;
    rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), iw, ih);
    if (rgba.empty() || iw <= 0 || ih <= 0) {
      *error = "the PNG is damaged";
      return false;
    }
    w = uint32_t(iw);
    h = uint32_t(ih);
  } else if (!DecodeWic(bytes, rgba, w, h)) {
    *error = "the JPEG is damaged";
    return false;
  }
  if (flip) FlipRows(rgba, w, h);
  // N64 emulator packs: "..._rgb.png" is opaque unless an "..._a.png" beside it
  // holds its alpha (grey).
  const std::wstring stem = path.stem().wstring();
  if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, L"_rgb") == 0) {
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    const fs::path alpha_path = path.parent_path() / (stem.substr(0, stem.size() - 4) + L"_a" + path.extension().wstring());
    std::vector<uint8_t> a;
    uint32_t aw = 0, ah = 0;
    std::string ignored;
    if (fs::exists(alpha_path, ec) && LoadPicture(alpha_path, a, aw, ah, &ignored, flip) && aw == w && ah == h)
      for (size_t i = 0; i < rgba.size(); i += 4) rgba[i + 3] = a[i];
  }
  return true;
}

fs::path DumpFolder() { return pd_paths::UserDataRoot() / "texture-dump"; }

fs::path DumpSubfolder(bool n64) {
  const fs::path ext_tex = DumpFolder() / "ext_tex";
  return n64 ? ext_tex : ext_tex / "xbla";
}

std::string DumpName(int id) {
  char name[16];
  std::snprintf(name, sizeof(name), "%04X.png", unsigned(id));
  return name;
}

bool WriteDumpPng(const fs::path& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  std::vector<uint8_t> flipped(rgba, rgba + size_t(w) * h * 4);
  FlipRows(flipped, w, h);
  return WritePng(path, flipped.data(), w, h);
}

bool Dumping() { return REXCVAR_GET(pd_texture_dump); }

void SetDumping(bool on) {
  rex::cvar::SetFlagByName("pd_texture_dump", on ? "true" : "false");
  if (on) {
    std::error_code ec;
    fs::create_directories(DumpSubfolder(false), ec);
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
