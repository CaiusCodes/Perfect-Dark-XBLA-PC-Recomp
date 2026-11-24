// N64 Perfect Dark texture decoder (the player's own N64 ROM; see N64-NOTES.md).
//
// Pure functions with no game or renderer dependencies, so a test tool can
// build this file alone (tools/n64/n64dec_test.cpp). Our own implementation of
// the format as documented in N64-NOTES.md; tools/n64/n64_tex.py is the
// reference it is checked against texture by texture.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pd::n64dec {

// One texture's base level as the N64 samples it: RGBA8, rows of w * 4 bytes.
// I formats are opaque grey, IA grey with alpha, CI the palette colour (as the
// XBLA's own N64-size copies have them).
struct Image {
  uint16_t w = 0, h = 0;
  // The N64 format: 0 RGBA32, 1 RGBA16, 2 RGB24, 3 RGB15, 4 IA16, 5 IA8, 6 IA4,
  // 7 I8, 8 I4, 9 CI8 (RGBA16 palette), 10 CI4 (RGBA16), 11 CI8 (IA16), 12 CI4 (IA16).
  uint8_t format = 0;
  std::vector<uint8_t> rgba;
  // For N64 emulator hi-res packs (Rice / GLideN64 naming): the texture's checksum as
  // the game leaves it in RDRAM (HiresCrc), its G_IM_FMT / G_IM_SIZ, and for CI the
  // palette's checksums over 16, 256, its own colour count and (highest index used + 1)
  // entries (packs differ in which). 0 when not computed.
  uint32_t hires_crc = 0;
  uint8_t gbi_fmt = 0, gbi_siz = 0;
  uint32_t hires_pal[4] = {};
};

// The classic hi-res texture checksum: over `h` rows of `pitch` bytes of RDRAM data
// (big-endian bytes, as the N64 has them), each row's ((w << siz) + 1) / 2 bytes as
// 32-bit words read the way emulators keep RDRAM, last word first:
// crc = rol4(crc) + (word ^ x), then + (last word ^ y) per row, y counting down from
// h - 1 while the rows go forward. `data` must hold h * pitch bytes.
uint32_t HiresCrc(const uint8_t* data, size_t size, int w, int h, int siz, size_t pitch);
// The checksum of a 16-bit palette of `count` entries (big-endian, as in RDRAM).
uint32_t HiresPaletteCrc(const uint16_t* palette, size_t count);

// The ROM versions the texture table is known for (the decomp's tools/extract).
struct RomVersion {
  const char* name;      // "pal-final", ...
  const char* md5;       // of the big-endian (.z64) ROM
  uint32_t texture_seg;  // texture data in the ROM
  uint32_t data_len;     // length of the data; the offset table follows
  bool tested;           // decoded and compared on a real ROM
  // The game's file table (models, levels): the 'data' segment (rarezip) in the ROM,
  // and the offset of the file table (u32 ROM offsets, ending in 0) inside it once
  // inflated (the decomp's tools/extract). 0 = not known.
  uint32_t data_seg = 0;
  uint32_t files_table = 0;
};
const RomVersion* FindRomVersion(const std::string& md5_hex);
// Every known Perfect Dark ROM (also the unsupported ones, to name them).
const std::vector<RomVersion>& KnownRomVersions();

// Brings a .v64 (16-bit swapped) or .n64 (little-endian) ROM to .z64 order in
// place; false if it isn't an N64 ROM at all.
bool NormaliseRomByteOrder(std::vector<uint8_t>& rom);

// Raw deflate (RFC 1951) of `in` into `out` (appended); false on bad data, or when it
// would make more than `max_out` bytes (a .zip bomb: the importer passes the entry's size).
bool Inflate(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out, size_t* consumed = nullptr,
             size_t max_out = SIZE_MAX);

// Decodes texture data (header byte first) to its base level. False on bad data.
bool DecodeTexture(const uint8_t* data, size_t size, Image& out, std::string* error = nullptr);

// Decodes every texture of a .z64 ROM's texture table. `out[id]` stays empty
// for textures without data (id 3394). Returns the number of ids in the table
// (0 on a malformed table; `error` says why).
size_t DecodeAll(const std::vector<uint8_t>& rom, const RomVersion& version, std::vector<Image>& out, std::string* error);

// The ROM offsets of the game's files (index = N64 file id; entry i's data runs to
// entry i + 1). Empty when the version's table isn't known or doesn't parse.
std::vector<uint32_t> FileOffsets(const std::vector<uint8_t>& rom, const RomVersion& version);
// File `id`'s contents, inflated when it is rarezip-compressed (0x1173 header). False
// for an id outside the table or bad data.
bool ExtractFile(const std::vector<uint8_t>& rom, const std::vector<uint32_t>& offsets, int id,
                 std::vector<uint8_t>& out);

// A texture kept inside a model file (texconfig number 0x05000000 + offset), base
// level, as it sits in the file: TMEM layout (rows padded to 8 bytes; on odd rows
// the two 32-bit words of every 8 bytes swapped). `fmt` / `siz` are the N64's
// G_IM_FMT (0 RGBA, 3 IA, 4 I) and G_IM_SIZ (0 4-bit, 1 8-bit, 2 16-bit, 3 32-bit).
// Checked against the remaster's own N64-size conversions of these textures
// (N64-NOTES.md). False for a format not handled or data too short.
bool DecodeTmem(const uint8_t* data, size_t size, int fmt, int siz, int w, int h, Image& out);

// The same for a paletted (CI) in-file texture: `bits` 4 or 8 index bits, the index
// data for `levels` mip levels (TMEM layout as above), then the 16-bit palette (16 or
// 256 entries; RGBA16, or IA16 with `ia_palette`). Some texconfigs call these RGBA16:
// the model's display list loads them with a palette (models_embedded_ci.py). Base
// level only.
bool DecodeTmemCI(const uint8_t* data, size_t size, int bits, int w, int h, int levels, bool ia_palette,
                  Image& out);

}  // namespace pd::n64dec
