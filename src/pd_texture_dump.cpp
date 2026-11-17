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

std::atomic<bool> g_running{false};

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

std::string name_of(int id) {
  char name[16];
  std::snprintf(name, sizeof(name), "%04d.png", id);
  return name;
}

void dump_all(std::function<void(const std::string&)> notice) {
  const auto t0 = std::chrono::steady_clock::now();
  const fs::path out = pd::texpack::DumpFolder();
  std::error_code ec;
  fs::create_directories(out / "remastered", ec);
  int written = 0, kept = 0, failed = 0, n64_written = 0;

  // Textures.raw: count, 52-byte index entries, 52-byte D3D headers, XMemCompress LZX data.
  const fs::path raw = pd_paths::DataRoot() / "assets" / "DataFiles" / "Textures.raw";
  std::ifstream in(raw, std::ios::binary);
  uint8_t head[4] = {};
  if (!in || !in.read(reinterpret_cast<char*>(head), 4)) {
    notice("Dump All Textures: the game's Textures.raw could not be read.");
    g_running = false;
    return;
  }
  const uint32_t count = be32(head);
  std::vector<uint8_t> tables(size_t(count) * 104);
  in.read(reinterpret_cast<char*>(tables.data()), std::streamsize(tables.size()));
  const uint64_t database = 4 + uint64_t(count) * 104;
  std::vector<uint8_t> comp, stream, data, rgba;
  for (uint32_t id = 0; id < count && in; ++id) {
    const fs::path png = out / "remastered" / name_of(int(id));
    if (fs::exists(png, ec)) {
      ++kept;
      continue;
    }
    const uint8_t* e = tables.data() + size_t(id) * 52;
    const uint32_t offset = be32(e), w = be32(e + 4), h = be32(e + 8), alloc = be32(e + 20), comp_size = be32(e + 24);
    if (!alloc || !comp_size || !w || !h || w > 4096 || h > 4096) continue;
    comp.resize(comp_size);
    in.seekg(std::streamoff(database + offset));
    if (!in.read(reinterpret_cast<char*>(comp.data()), comp_size)) {
      in.clear();
      ++failed;
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
        !pd::texpack::WritePng(png, rgba.data(), w, h)) {
      ++failed;
      continue;
    }
    ++written;
  }

  // The N64 pictures, when a ROM is imported and loaded.
  if (pd::n64tex::GetStatus() == pd::n64tex::Status::kReady) {
    fs::create_directories(out / "n64", ec);
    std::vector<int> ids;
    for (int id = 0; id < pd::n64tex::kTextureCount; ++id) ids.push_back(id);
    for (const int id : pd::n64tex::ModelTextureIds()) ids.push_back(id);
    for (const int id : ids) {
      const fs::path png = out / "n64" / name_of(id);
      if (fs::exists(png, ec)) {
        ++kept;
        continue;
      }
      const pd::n64dec::Image* im = pd::n64tex::Get(id);
      if (!im || im->rgba.empty()) continue;
      if (pd::texpack::WritePng(png, im->rgba.data(), im->w, im->h)) ++n64_written;
      else ++failed;
    }
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[texdump] all: {} remastered + {} N64 written, {} already there, {} failed, {:.1f} s", written,
              n64_written, kept, failed, seconds);
  char text[256];
  std::snprintf(text, sizeof(text), "Dumped %d remastered%s textures in %.0f s%s%s.", written,
                n64_written ? (" and " + std::to_string(n64_written) + " N64").c_str() : "", seconds,
                kept ? (" (" + std::to_string(kept) + " already there)").c_str() : "",
                failed ? (", " + std::to_string(failed) + " failed").c_str() : "");
  notice(text);
  g_running = false;
}

}  // namespace

bool StartDumpAll(std::function<void(const std::string&)> notice) {
  if (g_running.exchange(true)) return false;
  REXLOG_INFO("[texdump] all: to {}", pd::texpack::DumpFolder().string());
  notice(pd::n64tex::GetStatus() == pd::n64tex::Status::kReady
             ? "Dumping all remastered and N64 textures to resources\\userdata\\texture-dump..."
             : "Dumping all textures to resources\\userdata\\texture-dump...");
  std::thread([notice]() { dump_all(notice); }).detach();
  return true;
}

bool Running() { return g_running.load(); }

}  // namespace pd::texdump
