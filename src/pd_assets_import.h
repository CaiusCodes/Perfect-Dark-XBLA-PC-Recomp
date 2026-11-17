// Experimental extras the player adds themselves: their own N64 Perfect Dark
// ROM (N64 textures) and texture packs. Shared by the game (Settings >
// Graphics > Experimental) and the launcher (--n64-rom / --texture-pack), so
// it has no game or SDK dependencies: plain C++17, Windows and pd_n64_decode.
//
// A texture pack is a folder of pictures named by the game's texture number
// (its id in DataFiles\Textures.raw, 0-5746): "0123.png", "123.png" and
// "0123 wall.png" all stand for id 123 (the digits at the start of the name;
// anything after a non-digit is ignored). PNG only (RGBA, RGB, grey, with or
// without alpha). Any size up to 4096x4096; the game makes the mips. Ids the
// pack doesn't have keep their picture.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pd::assets {

// The largest id a pack picture may have (Textures.raw holds 5747).
constexpr int kMaxTextureId = 8191;

// The Textures.raw id a pack file stands for, or -1 (not a picture named by id).
int PackFileId(const std::filesystem::path& file);

// N64 emulator hi-res packs (Rice / GLideN64 naming) also work, over the N64
// textures: "<game>#<texture crc>#<fmt>#<siz>[#<palette crc>]_<kind>.png" (crc in
// hex, kind all / rgb / a / ciByRGBA / ciByIA / allciByRGBA; "_a" is the alpha of
// the "_rgb" beside it), sub-folders allowed. They're matched to texture ids by the
// checksum of the player's own N64 ROM's textures (pd_n64_textures.h HiresTable).
struct HiresName {
  uint32_t crc = 0;
  int fmt = -1, siz = -1;
  bool has_palette = false;
  uint32_t palette = 0;
  std::string kind;  // "all", "rgb", "a", ...
};
bool ParseHiresName(const std::filesystem::path& file, HiresName* out);
// True for any file a pack keeps: named by id, or by hi-res checksum (the "_a"
// alpha pictures included).
bool IsPackFile(const std::filesystem::path& file);

// MD5 of `size` bytes, lower-case hex (empty on failure).
std::string Md5Hex(const uint8_t* data, size_t size);

// Checks `src` is an N64 Perfect Dark ROM whose textures the game can use
// (32 MB, any byte order, a known MD5 with a texture table) and writes it in
// .z64 byte order as `dest_folder`\pd.z64, removing any other ROM file there.
// `version`: the ROM's version ("pal-final"). `message`: a sentence for the
// player either way.
bool ImportN64Rom(const std::filesystem::path& src, const std::filesystem::path& dest_folder, std::string* version,
                  std::string* message);

// Copies a texture pack into `packs_folder`\<name>: from a folder (its pictures
// named by id, also in subfolders, plus any .txt / .md notes at its top) or a
// .zip of one. The pack's name is the folder's or the .zip's (without the
// extension); an older copy of the same name is replaced. `pictures`: how many
// pictures it has.
bool ImportTexturePack(const std::filesystem::path& src, const std::filesystem::path& packs_folder, std::string* name,
                       int* pictures, std::string* message);

// A pack name made safe as a folder name (letters, digits, space, - _ . ( ),
// at most 40 characters); empty if nothing is left.
std::string SafePackName(const std::string& name);

}  // namespace pd::assets
