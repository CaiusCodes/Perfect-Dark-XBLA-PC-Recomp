// Dump All Textures: see pd_texture_dump.h.
#include "pd_texture_dump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/system/lzx.h>

#include "pd_n64_decode.h"
#include "pd_n64_textures.h"
#include "pd_paths.h"
#include "pd_texture_packs.h"

// pd_native_gpu.cpp: Plume's DXT block decoder (fmt 18/19/20, little-endian block).
void PdDecodeTextureBlock(uint32_t fmt, const uint8_t* block, uint8_t out[16][4]);

namespace pd::texdump {
namespace {

namespace fs = std::filesystem;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Xenos 2D tiled address of element (x, y) (in elements) for an aligned width (the
// public XGAddress2DTiledOffset formula); log_bpp = log2(bytes per element).
uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t aligned_width, uint32_t log_bpp) {
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return ((((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
          log_bpp);
}

uint32_t log2ceil(uint32_t v) {
  uint32_t n = 0;
  while ((1u << n) < v) ++n;
  return n;
}

// Packed mips: a base level whose smaller side is 16 or less sits inside the 32x32 tile
// it shares with its mips (at y 16 when wider than tall, else at x 16).
void packed_offset(uint32_t w, uint32_t h, uint32_t& ox, uint32_t& oy) {
  ox = oy = 0;
  if (std::min(w, h) > 16) return;
  if (log2ceil(w) > log2ceil(h)) oy = 16;
  else ox = 16;
}

// One Textures.raw entry's base level as RGBA8; false for a format not handled.
bool decode_base(const uint8_t* fc_words, const std::vector<uint8_t>& data, uint32_t w, uint32_t h,
                 std::vector<uint8_t>& rgba) {
  const uint32_t fc0 = be32(fc_words), fc1 = be32(fc_words + 4);
  const uint32_t fmt = fc1 & 0x3F;
  const bool tiled = (fc0 >> 31) != 0;
  const uint32_t pitch = ((fc0 >> 22) & 0x1FF) * 32;
  rgba.assign(size_t(w) * h * 4, 0);
  if (fmt == 6) {  // 8_8_8_8: big-endian dwords 0xAARRGGBB
    uint32_t ox = 0, oy = 0;
    if (tiled) packed_offset(w, h, ox, oy);
    const uint32_t aw = std::max({32u, pitch, (w + ox + 31) & ~31u});
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const size_t o = size_t(tiled ? tiled_offset(x + ox, y + oy, aw, 2) : (y * std::max(pitch, w) + x)) * 4;
        if (o + 4 > data.size()) return false;
        uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
        p[0] = data[o + 1];
        p[1] = data[o + 2];
        p[2] = data[o + 3];
        p[3] = data[o];
      }
    return true;
  }
  if (fmt == 18 || fmt == 19 || fmt == 20) {  // DXT1 / DXT2-3 / DXT4-5, 16-bit words swapped
    const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    const uint32_t bsize = fmt == 18 ? 8 : 16;
    const uint32_t pitch_blocks = std::max(pitch / 4, bw);
    const uint32_t aligned_bw = (pitch_blocks + 31) & ~31u;
    uint8_t block[16], texels[16][4];
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const size_t o = size_t(tiled ? tiled_offset(bx, by, aligned_bw, bsize == 8 ? 3 : 4)
                                      : (by * pitch_blocks + bx)) * bsize;
        if (o + bsize > data.size()) return false;
        for (uint32_t k = 0; k < bsize; k += 2) {
          block[k] = data[o + k + 1];
          block[k + 1] = data[o + k];
        }
        PdDecodeTextureBlock(fmt, block, texels);
        for (uint32_t t = 0; t < 16; ++t) {
          const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
          if (x < w && y < h) std::memcpy(&rgba[(size_t(y) * w + x) * 4], texels[t], 4);
        }
      }
    return true;
  }
  return false;
}

struct Counts {
  int written = 0, kept = 0, failed = 0;
};

// The remaster's textures (Textures.raw): ext_tex\xbla, as Dump All Textures always did.
bool dump_remastered(Counts& c, std::string* error) {
  std::error_code ec;
  fs::create_directories(pd::texpack::DumpSubfolder(false), ec);
  // Textures.raw: count, 52-byte index entries, 52-byte D3D headers, XMemCompress LZX data.
  const fs::path raw = pd_paths::DataRoot() / "assets" / "DataFiles" / "Textures.raw";
  std::ifstream in(raw, std::ios::binary);
  uint8_t head[4] = {};
  if (!in || !in.read(reinterpret_cast<char*>(head), 4)) {
    *error = "the game's Textures.raw could not be read";
    return false;
  }
  const uint32_t count = be32(head);
  if (count == 0 || count > 65536) {
    *error = "the game's Textures.raw is damaged";
    return false;
  }
  std::vector<uint8_t> tables(size_t(count) * 104);
  in.read(reinterpret_cast<char*>(tables.data()), std::streamsize(tables.size()));
  const uint64_t database = 4 + uint64_t(count) * 104;
  std::vector<uint8_t> comp, stream, data, rgba;
  for (uint32_t id = 0; id < count && in; ++id) {
    const fs::path png = pd::texpack::DumpSubfolder(false) / pd::texpack::DumpName(int(id));
    if (fs::exists(png, ec)) {
      ++c.kept;
      continue;
    }
    const uint8_t* e = tables.data() + size_t(id) * 52;
    const uint32_t offset = be32(e), w = be32(e + 4), h = be32(e + 8), alloc = be32(e + 20), comp_size = be32(e + 24);
    if (!alloc || !comp_size || !w || !h || w > 4096 || h > 4096 || alloc > (64u << 20) || comp_size > (64u << 20))
      continue;
    comp.resize(comp_size);
    in.seekg(std::streamoff(database + offset));
    if (!in.read(reinterpret_cast<char*>(comp.data()), comp_size)) {
      in.clear();
      ++c.failed;
      continue;
    }
    // Chunks: [u16 length] or [0xFF, u16 uncompressed, u16 length], then the data; 0 ends.
    stream.clear();
    for (size_t p = 0; p + 2 <= comp.size();) {
      size_t len;
      if (comp[p] == 0xFF) {
        if (p + 5 > comp.size()) break;
        len = (size_t(comp[p + 3]) << 8) | comp[p + 4];
        p += 5;
      } else {
        len = (size_t(comp[p]) << 8) | comp[p + 1];
        p += 2;
      }
      if (!len || p + len > comp.size()) break;
      stream.insert(stream.end(), comp.begin() + std::ptrdiff_t(p), comp.begin() + std::ptrdiff_t(p + len));
      p += len;
    }
    data.assign(alloc, 0);
    if (stream.empty() || lzx_decompress(stream.data(), stream.size(), data.data(), data.size(), 0x20000, nullptr, 0) != 0 ||
        !decode_base(tables.data() + size_t(count) * 52 + size_t(id) * 52 + 28, data, w, h, rgba) ||
        !pd::texpack::WriteDumpPng(png, rgba.data(), w, h)) {
      ++c.failed;
      continue;
    }
    ++c.written;
  }
  return true;
}

// The N64 pictures (ids 0-3502 and the N64 model files' own): ext_tex, as before. The ROM
// is loaded first when it isn't (up to two minutes).
bool dump_n64(Counts& c, bool load, std::string* error) {
  using pd::n64tex::Status;
  if (load && pd::n64tex::GetStatus() != Status::kReady) {
    if (!pd::n64tex::RomPresent()) {
      *error = "no N64 ROM is imported (Import Texture Pack... takes one)";
      return false;
    }
    pd::n64tex::BeginLoad();
    for (int i = 0; i < 1200; ++i) {
      const Status s = pd::n64tex::GetStatus();
      if (s != Status::kLoading && s != Status::kIdle) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (pd::n64tex::GetStatus() != Status::kReady) {
    *error = "the N64 ROM couldn't be loaded (" + pd::n64tex::StatusDetail() + ")";
    return false;
  }
  std::error_code ec;
  fs::create_directories(pd::texpack::DumpSubfolder(true), ec);
  std::vector<int> ids;
  for (int id = 0; id < pd::n64tex::kTextureCount; ++id) ids.push_back(id);
  for (const int id : pd::n64tex::ModelTextureIds()) ids.push_back(id);
  for (const int id : ids) {
    const fs::path png = pd::texpack::DumpSubfolder(true) / pd::texpack::DumpName(id);
    if (fs::exists(png, ec)) {
      ++c.kept;
      continue;
    }
    const pd::n64dec::Image* im = pd::n64tex::Get(id);
    if (!im || im->rgba.empty()) continue;
    if (pd::texpack::WriteDumpPng(png, im->rgba.data(), im->w, im->h)) ++c.written;
    else ++c.failed;
  }
  return true;
}

fs::path PathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

// A folder name for a pack's dump: the pack's own folder name (it is one already) with
// anything a folder name can't hold replaced.
std::string DumpFolderName(const std::string& name) {
  std::string out;
  for (const unsigned char ch : name) out += (ch < 32 || std::strchr("<>:\"/\\|?*", ch)) ? '_' : char(ch);
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? std::string("pack") : out;
}

// An installed pack: what the game takes from it, each picture as the game turns it, written
// in the dump's naming and orientation (so the folder is a PC-port pack itself).
bool dump_pack(const std::string& name, Counts& c, std::string* summary_path, std::string* error) {
  const auto packs = pd::texpack::ListPacks(true);
  if (std::find(packs.begin(), packs.end(), name) == packs.end()) {
    *error = "\"" + name + "\" isn't an installed texture pack";
    return false;
  }
  const std::shared_ptr<const pd::texpack::Index> index = pd::texpack::IndexFor(name);
  if (!index) {
    *error = "\"" + name + "\" couldn't be read";
    return false;
  }
  const fs::path folder = SourceFolder(name);
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec) {
    *error = "can't write to " + folder.string();
    return false;
  }
  std::vector<std::string> problems;
  int top = 0, xbla = 0, own = 0;
  std::vector<uint8_t> rgba;
  for (int source = 0; source < 3; ++source) {
    const auto& list = source == 0 ? index->by_id : source == 1 ? index->xbla_by_id : index->remastered_by_id;
    for (size_t i = 0; i < list.size(); ++i) {
      const int id = int(i);
      const fs::path* path = index->Picture(source, id);
      if (!path) continue;
      // An <id>-xbla picture (this game's own version, the remastered look's) keeps its suffix,
      // in the folder it came from.
      const bool in_xbla = source == 1 || (source == 2 && _wcsicmp(path->parent_path().filename().c_str(), L"xbla") == 0);
      const fs::path out_dir = in_xbla ? folder / "xbla" : folder;
      fs::path png = out_dir / pd::texpack::DumpName(id);
      if (source == 2) png = out_dir / (png.stem().wstring() + L"-xbla" + png.extension().wstring());
      if (fs::exists(png, ec)) {
        ++c.kept;
        continue;
      }
      if (in_xbla) fs::create_directories(out_dir, ec);
      // As the renderer reads it (pd_native_gpu.cpp): a PC-port picture is turned over unless
      // it is an N64 emulator redraw or one of the two font atlases stored as the game has them.
      const bool flip = index->pc_port && !index->N64Only(id) && !pd::texpack::FontAtlas(id);
      uint32_t w = 0, h = 0;
      std::string why;
      if (!pd::texpack::LoadPicture(*path, rgba, w, h, &why, flip)) {
        ++c.failed;
        if (problems.size() < 200) problems.push_back(path->filename().string() + ": " + why);
        continue;
      }
      if (!pd::texpack::WriteDumpPng(png, rgba.data(), w, h)) {
        ++c.failed;
        if (problems.size() < 200) problems.push_back(png.filename().string() + ": can't be written");
        continue;
      }
      ++c.written;
      ++(source == 0 ? top : source == 1 ? xbla : own);
    }
  }
  // What was left out, and why.
  const fs::path summary = folder / "dump-summary.txt";
  std::ofstream s(summary, std::ios::binary | std::ios::trunc);
  if (s) {
    s << "Dump Game Textures: \"" << name << "\"\r\n";
    s << "Naming: " << (index->pc_port ? "PC-port (four hex digits; top folder = N64 numbers, xbla = the remaster's)"
                                       : "decimal (0.9.2's dump)")
      << "\r\n";
    s << "Written now: " << c.written << " (" << top << " top folder, " << xbla << " xbla";
    if (own) s << ", " << own << " -xbla (this game's own, the remastered look's)";
    s << ")";
    if (c.kept) s << "; " << c.kept << " already here (left alone)";
    s << "\r\n";
    s << "These are the pack's pictures as the game reads them, in the dump's naming and orientation\r\n"
         "(four hex digits, right way up): this folder is a texture pack itself.\r\n";
    if (index->reflect_n)
      s << "Not dumped: " << index->reflect_n << " reflection mask(s) (-reflect pictures; settings, not textures).\r\n";
    if (index->keep_original_n)
      s << "Left out: " << index->keep_original_n << " texture(s) pd-pack.txt keeps as the game's own (keep_original).\r\n";
    if (index->ignored)
      s << "Left out: " << index->ignored << " picture(s) in other folders (the PC port's N64 font glyphs).\r\n";
    if (index->waiting_for_rom)
      s << "Left out: " << (index->hires_files + index->model_files)
        << " N64 emulator / per-model picture(s): they are matched through the N64 ROM, which isn't loaded.\r\n";
    else if (index->hires_files || index->model_files)
      s << "N64 emulator / per-model pictures matched through the ROM: " << index->hires_matched + index->model_matched
        << " of " << index->hires_files + index->model_files << ".\r\n";
    s << "In play, a picture whose shape doesn't fit the texture it would replace isn't drawn (logged);\r\n"
         "in the remastered look, a PC-port pack's top-folder pictures for the 44 numbers the remaster\r\n"
         "reused for other surfaces aren't drawn either.\r\n";
    if (!problems.empty()) {
      s << "Not readable (" << c.failed << "):\r\n";
      for (const std::string& p : problems) s << "  " << p << "\r\n";
    }
  }
  if (summary_path) *summary_path = summary.string();
  return true;
}

std::atomic<bool> g_any_running{false};

void run(const std::string& source, std::function<void(const std::string&)> notice) {
  const auto t0 = std::chrono::steady_clock::now();
  Counts c;
  std::string error, summary;
  bool ok;
  std::string what;
  if (source == "Remastered") {
    what = "remastered";
    ok = dump_remastered(c, &error);
  } else if (source == "N64") {
    what = "N64";
    ok = dump_n64(c, true, &error);
  } else {
    what = "\"" + source + "\"";
    ok = dump_pack(source, c, &summary, &error);
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[texdump] {}: {} written, {} already there, {} failed, {:.1f} s{}{}", source, c.written, c.kept,
              c.failed, seconds, ok ? "" : ": ", error);
  if (!ok) {
    notice("Dump Game Textures: " + error + ".");
  } else {
    char text[512];
    std::snprintf(text, sizeof(text), "Dumped %d %s textures in %.0f s%s%s.", c.written, what.c_str(), seconds,
                  c.kept ? (" (" + std::to_string(c.kept) + " already there)").c_str() : "",
                  c.failed ? (", " + std::to_string(c.failed) + " couldn't be read").c_str() : "");
    notice(text);
  }
  g_any_running = false;
}

void dump_all(std::function<void(const std::string&)> notice) {
  const auto t0 = std::chrono::steady_clock::now();
  Counts c, n;
  std::string error;
  if (!dump_remastered(c, &error)) {
    notice("Dump All Textures: " + error + ".");
    g_any_running = false;
    return;
  }
  std::string ignored;
  if (pd::n64tex::GetStatus() == pd::n64tex::Status::kReady) dump_n64(n, false, &ignored);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[texdump] all: {} remastered + {} N64 written, {} already there, {} failed, {:.1f} s", c.written,
              n.written, c.kept + n.kept, c.failed + n.failed, seconds);
  char text[256];
  std::snprintf(text, sizeof(text), "Dumped %d remastered%s textures in %.0f s%s%s.", c.written,
                n.written ? (" and " + std::to_string(n.written) + " N64").c_str() : "", seconds,
                c.kept + n.kept ? (" (" + std::to_string(c.kept + n.kept) + " already there)").c_str() : "",
                c.failed + n.failed ? (", " + std::to_string(c.failed + n.failed) + " failed").c_str() : "");
  notice(text);
  g_any_running = false;
}

}  // namespace

fs::path SourceFolder(const std::string& source) {
  if (source == "Remastered") return pd::texpack::DumpSubfolder(false);
  if (source == "N64") return pd::texpack::DumpSubfolder(true);
  return pd::texpack::DumpFolder() / "packs" / PathOf(DumpFolderName(source));
}

bool StartDumpAll(std::function<void(const std::string&)> notice) {
  if (g_any_running.exchange(true)) return false;
  REXLOG_INFO("[texdump] all: to {}", pd::texpack::DumpFolder().string());
  notice(pd::n64tex::GetStatus() == pd::n64tex::Status::kReady
             ? "Dumping all remastered and N64 textures to resources\\userdata\\texture-dump..."
             : "Dumping all textures to resources\\userdata\\texture-dump...");
  std::thread([notice]() { dump_all(notice); }).detach();
  return true;
}

bool StartDump(const std::string& source, std::function<void(const std::string&)> notice) {
  if (source.empty() || g_any_running.exchange(true)) return false;
  const fs::path to = SourceFolder(source);
  REXLOG_INFO("[texdump] {}: to {}", source, to.string());
  const std::string shown = source == "Remastered" || source == "N64" ? source + " textures" : "\"" + source + "\"";
  // (resources\userdata\texture-dump\...: the folder as the player finds it)
  notice("Dumping " + shown + " to resources\\" +
         to.lexically_relative(pd::texpack::DumpFolder().parent_path().parent_path()).string() + "...");
  std::thread([source, notice]() { run(source, notice); }).detach();
  return true;
}

bool Running() { return g_any_running.load(); }

}  // namespace pd::texdump