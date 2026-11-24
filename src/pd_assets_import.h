// What the player adds themselves: their own N64 Perfect Dark ROM (N64 textures) and
// texture packs. Shared by the game (Settings > Graphics > Texture Packs > Import Texture
// Pack..., HANDOVER 4du) and the launcher (--n64-rom / --texture-pack), so it has no game or
// SDK dependencies: plain C++17, Windows and pd_n64_decode.
//
// Everything imported is the player's own, untrusted file: it is identified by its content
// (Identify), never run, and only pictures (checked to be real PNG / JPEG files) and .txt /
// .md notes are copied, into a staging folder inside the packs folder that is renamed into
// place when complete (removed on any failure). A .zip with an entry whose path is absolute,
// has a ".." part, a drive or a colon is refused whole; its entries are inflated no further
// than the size they state (.zip bombs), at most 4 GB in all and 64 MB a picture; links (in a
// folder or a .zip) are never followed. An installed pack is never replaced: the same files
// again is "already installed", other files under a taken name get "Name (2)".
//
// A texture pack is a folder of pictures named by the game's texture number
// (its id in DataFiles\Textures.raw, 0-5746). PNG or JPEG (.png / .jpg / .jpeg;
// RGBA, RGB, grey). Any size up to 4096x4096; the game makes the mips. Ids the
// pack doesn't have keep their picture. Two naming conventions, decided per pack:
//
// - PC-port packs (the N64 PC port's / Dabs Mod's ext_tex packs, and this game's
//   own dump since the update after 0.9.2): names of exactly four hex digits, any case ("00C6.png",
//   "000f.jpg" = id 15), pictures stored right way up (two of the remaster's font atlases,
//   3515 and 3516, as the game has them: texpack::FontAtlas). The top folder holds pictures made for the N64
//   textures, an "xbla" folder ones made for the remaster's (the PC port's XBLA
//   model support uses the same numbers as this game), and folders named after N64
//   model files hold the textures those files keep inside themselves, by byte offset
//   (ModelPicture). The PC port's font folders (N64 font glyphs) are left out.
// - Decimal packs (0.9.2's dump and packs made from it): "0123.png", "123.png" and
//   "0123 wall.png" all stand for id 123 (the digits at the start of the name;
//   anything after a non-digit is ignored), pictures stored as 0.9.2's dump wrote
//   them (upside down compared with the PC port's), sub-folders read too.
//
// A pack is a PC-port pack when it has an "xbla" folder (even an empty one: the way
// to mark a small pack whose names happen to be all digits) or a top-folder picture
// whose four-hex-digit name has a letter a-f in it; otherwise it is a decimal pack.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pd::assets {

// The largest id a pack picture may have (Textures.raw holds 5747).
constexpr int kMaxTextureId = 8191;

// .png, .jpg or .jpeg.
bool IsPicture(const std::filesystem::path& file);
// Decimal naming: the Textures.raw id a pack file stands for, or -1.
int PackFileId(const std::filesystem::path& file);
// PC-port naming (exactly four hex digits): the id, or -1.
int HexPackFileId(const std::filesystem::path& file);
// True when `file` is a picture named by id in either convention.
bool IsIdPicture(const std::filesystem::path& file);
// A suffix after the id (either convention; HANDOVER 4dw), read by this game only, so a pack
// stays as the PC port and Dabs Mod read it:
//   <id>-xbla.png    this game's own version of the picture: it replaces the plain one in the
//                    remastered look (the N64 look keeps the plain one), from the top folder
//                    or xbla\ alike (e.g. 0046-xbla.png, xbla\12A6-xbla.png);
//   <id>-reflect.png a reflection mask for that texture: each part of a model drawn with the
//                    remaster's reflections (the environment map's shine) is drawn without them
//                    when most of it lies on black in the mask (white: shiny as the game has it).
enum class PictureSuffix { kNone, kXbla, kReflect, kOther };
PictureSuffix SuffixOf(const std::filesystem::path& file);
// The pack folder a picked picture belongs to (its folder; for a picture in a
// pack's xbla or ext_tex folder, the folder above).
std::filesystem::path PackFolderOf(const std::filesystem::path& picture);

// A PC-port pack's per-model picture: "<N64 model file name>\<byte offset in hex>.png"
// in a folder of the pack's own (the PC port's naming for the textures an N64 model
// file keeps inside itself, e.g. PcaseZ\00b78.png). The game matches them to texture
// ids through the player's N64 ROM (its file names); without it they wait.
struct ModelPicture {
  std::string file;  // the model file's name (the folder's)
  uint32_t offset = 0;
  std::filesystem::path path;
};
// The PC port's font folders ("fonthandelgothicsm", "fontnumeric"...): pictures of the
// N64 fonts' glyphs, which this game doesn't draw (it draws the remaster's font atlases,
// 3512-3517, which a pack can replace in its xbla folder).
bool IsPcPortFontFolder(const std::filesystem::path& name);

// An installed pack's pictures (see the naming rules above).
struct PackPictures {
  // The pack's own folder inside `folder` (empty: `folder` itself): the folder that
  // directly holds the most pictures named by id (an xbla folder counting for the
  // one it is in), so a README beside it or an ext_tex folder around it is fine.
  std::filesystem::path root;
  bool pc_port = false;
  std::vector<std::filesystem::path> top, xbla;  // by id; an empty path: none
  std::vector<std::filesystem::path> remastered;  // <id>-xbla pictures, by id
  std::vector<std::filesystem::path> reflect;     // <id>-reflect masks, by id
  std::vector<std::filesystem::path> hires;      // N64 emulator names, the "_a" alpha pictures left out
  std::vector<ModelPicture> models;              // PC-port per-model pictures
  int pictures = 0;  // id-named pictures used + hires + models
  int ignored = 0;   // id-named pictures in other sub-folders of a PC-port pack (fonts)
};
PackPictures ScanPack(const std::filesystem::path& folder);

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

// What a picked path is, by its content (not its name): a folder, a .zip (PK signature),
// an N64 ROM (its first word in any of the three byte orders), a PNG or JPEG picture.
enum class InputKind { kUnknown, kFolder, kZip, kRom, kPicture };
InputKind Identify(const std::filesystem::path& path, std::string* detail);  // detail: why unknown
// True for the start of a real PNG or JPEG file.
bool LooksLikePicture(const uint8_t* data, size_t size);
// An N64 ROM (.z64 order) as its header names it: "Perfect Dark PAL (Europe/Australia),
// revision 0 (NPDP)", or another game's title.
std::string DescribeRom(const std::vector<uint8_t>& rom);

// Checks `src` is an N64 Perfect Dark ROM whose textures the game can use
// (32 MB, any byte order, a known MD5 with a texture table: NTSC-U 1.0 and 1.1 ("final"),
// PAL, NTSC-J) and writes it in .z64 byte order as `dest_folder`\pd.z64, removing any
// other ROM file there. `version`: the ROM's version ("pal-final"). `message`: a sentence
// for the player either way, naming the ROM by its header (region, revision).
bool ImportN64Rom(const std::filesystem::path& src, const std::filesystem::path& dest_folder, std::string* version,
                  std::string* message);

// Copies a texture pack into `packs_folder`\<name>: from a folder or a .zip of one
// (the pack's own folder found inside it as ScanPack does). Kept: the
// pictures the naming rules above read (a PC-port pack's top and xbla\ pictures,
// keeping xbla\; a decimal pack's, flattened), N64 emulator pictures (flattened)
// and any .txt / .md notes at its top. The pack's name is the folder's or the
// .zip's (without the extension); an installed pack is never replaced (see above).
// `name`: the name it was installed under. `pictures`: how many pictures it has.
bool ImportTexturePack(const std::filesystem::path& src, const std::filesystem::path& packs_folder, std::string* name,
                       int* pictures, std::string* message);

// Loose pictures picked one by one (or several at once): a new pack of those whose names
// say which texture they replace (by id in either convention, or an N64 emulator checksum),
// named after their folder; the others are left out, and if none can be placed nothing is
// installed and `message` says why.
bool ImportPictures(const std::vector<std::filesystem::path>& files, const std::filesystem::path& packs_folder,
                    std::string* name, int* pictures, std::string* message);

// A pack name made safe as a folder name (letters, digits, space, - _ . ( ),
// at most 48 characters); empty if nothing is left.
std::string SafePackName(const std::string& name);
// A file or folder name made readable for a pack's name: dots / underscores between words
// become spaces, version numbers stay ("PD.Forever.Plus.HD.v0.10" -> "PD Forever Plus HD v0.10").
std::string ReadablePackName(const std::string& stem);

}  // namespace pd::assets
