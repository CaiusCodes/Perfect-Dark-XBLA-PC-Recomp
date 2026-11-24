// Texture packs (experimental; Settings > Graphics > Experimental): folders of
// pictures named by Textures.raw id in resources\texture-packs, one chosen at a
// time (pd_texture_pack), drawn by Plume over the base textures (remastered or
// N64) for the ids the pack has. pd_assets_import.h has the naming rules.
// Also the texture dump for pack makers (pd_texture_dump).
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pd::texpack {

// resources\texture-packs (a dev build: texture-packs beside the exe).
std::filesystem::path PacksFolder();
// The installed packs, by name (folder name), sorted; at most kMaxListed.
constexpr int kMaxListed = 32;
std::vector<std::string> ListPacks(bool rescan = false);

// The chosen pack's name ("" = none; pd_texture_pack, saved).
std::string ChosenPack();
// Chooses a pack ("" = none) and indexes its pictures (on the calling thread:
// a directory listing). Returns the number of pictures it has.
int ChoosePack(const std::string& name);

// The chosen pack's pictures by id (read by the renderer once a frame).
// Which picture is drawn (pd_native_gpu.cpp pack_choice): over an N64 picture, the
// top folder's then xbla\'s; over a remastered one, xbla\'s then the top folder's
// (a PC-port pack's top-folder pictures are made for the N64 textures, so not over
// the remastered pictures that are a different surface: remaster_only_id). A
// picture whose shape (aspect ratio) differs from the texture it would replace
// isn't drawn: it was made for another picture with that number.
struct Index {
  std::string name;
  bool pc_port = false;  // PC-port naming: pictures stored right way up (flipped on load)
  std::vector<std::filesystem::path> by_id;    // top folder (decimal packs: all); empty path: none
  std::vector<std::filesystem::path> xbla_by_id;  // xbla\ (PC-port packs)
  int ignored = 0;  // pictures in other sub-folders of a PC-port pack
  // Ids whose picture came from N64 emulator naming (a hi-res redraw of the N64
  // texture): drawn in N64 mode only.
  std::vector<uint8_t> n64_only;
  // The pack has emulator-named pictures but the N64 ROM isn't loaded yet, so they
  // aren't matched: the index is rebuilt once it is (RebuildIfWaiting).
  bool waiting_for_rom = false;
  int hires_files = 0, hires_matched = 0;
  // PC-port per-model pictures (pd_assets_import.h ModelPicture), matched to the
  // in-file texture ids through the ROM's file names; they wait for it like the
  // emulator pictures.
  int model_files = 0, model_matched = 0;
  uint32_t version = 0;
  // The look the pack was made for (texture slots, below): the one that draws more of
  // its pictures. N64: its pictures for the N64 texture numbers (0-3502) and the N64
  // models' own; remastered: xbla\'s, and the top folder's but the 44 numbers the
  // remaster reused for other surfaces (PC-port packs) and N64 emulator redraws. A tie
  // is remastered. Forever (N64 numbers only) is N64; Ultimate and XBLA Plus (an xbla\
  // folder with the remaster's own textures) are remastered. HANDOVER 4dn.
  bool n64_base = false;
  int used_n64 = 0, used_remastered = 0;
  int pictures = 0;  // pictures indexed (top + xbla\ + matched ones)
  // Rebuilt from another index of the same pack (its per-model / emulator pictures
  // matched once the N64 ROM loaded): that index's version. The renderer keeps the
  // pictures it has whose file is the same, instead of decoding them again.
  uint32_t keep_from = 0;
  // <id>-xbla pictures (top folder or xbla\, pd_assets_import.h PictureSuffix): this game's own
  // version, drawn in the remastered look before the others; the N64 look doesn't draw them.
  std::vector<std::filesystem::path> remastered_by_id;
  // source 0: top folder, 1: xbla\, 2: an <id>-xbla picture. nullptr if the pack hasn't that picture.
  const std::filesystem::path* Picture(int source, int id) const {
    const auto& v = source == 0 ? by_id : source == 1 ? xbla_by_id : remastered_by_id;
    return id >= 0 && size_t(id) < v.size() && !v[size_t(id)].empty() ? &v[size_t(id)] : nullptr;
  }
  bool Has(int id) const { return Picture(0, id) || Picture(1, id) || Picture(2, id); }
  size_t IdCount() const { return std::max({by_id.size(), xbla_by_id.size(), remastered_by_id.size()}); }
  int remastered_pictures = 0;  // <id>-xbla pictures indexed
  // <id>-reflect masks (pd_assets_import.h PictureSuffix), decoded small (luminance, at most
  // 256 on a side, laid out as the pack's pictures are stored). A part of a model drawn with
  // the remaster's reflections whose vertices lie mostly on black is drawn without them
  // (pd_native_gpu.cpp reflect_part_matte).
  struct ReflectMask {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> lum;
    std::string file;
    // Luminance at texture coordinates (u, v) of the game's vertices (wrapped).
    uint8_t At(float u, float v, bool pc_port) const;
  };
  std::vector<std::shared_ptr<const ReflectMask>> reflect_by_id;
  const ReflectMask* Reflect(int id) const {
    return id >= 0 && size_t(id) < reflect_by_id.size() ? reflect_by_id[size_t(id)].get() : nullptr;
  }
  int reflect_n = 0;
  bool N64Only(int id) const { return id >= 0 && size_t(id) < n64_only.size() && n64_only[size_t(id)]; }
  // The pack's own settings file, pd-pack.txt in its folder (optional; HANDOVER 4dq):
  //   no_reflection = 12A6, 0x1234   textures drawn without the remaster's reflections
  //     (the shine the game adds from its environment maps), e.g. a pack that repaints a
  //     costume whose shiny parts are no longer leather. Numbers as the pack names its
  //     pictures (hex in a PC-port pack, decimal in a decimal one; 0x... is always hex).
  //   keep_original = 0046           textures the game draws with its own picture, in both
  //     looks: the pack's pictures for them are left out (HANDOVER 4dr; e.g. a PC-port
  //     picture that is one layer of a surface the remaster draws from one baked texture,
  //     like the dataDyne carpet, texture 70).
  // Lines starting with # are comments; unknown keys are logged and skipped.
  std::vector<uint8_t> no_reflection;  // by id
  int no_reflection_n = 0;
  std::vector<uint8_t> keep_original;  // by id
  int keep_original_n = 0;
  bool NoReflection(int id) const { return id >= 0 && size_t(id) < no_reflection.size() && no_reflection[size_t(id)]; }
};
std::shared_ptr<const Index> CurrentIndex();
// Pack `name`'s index without choosing it: kept per pack (built once, a directory
// listing; again after ForgetLook). An empty index for "" or a missing pack.
std::shared_ptr<const Index> IndexFor(const std::string& name);

// ---- What Plume draws, switched in one go (HANDOVER 4do) ----
// The pack (its index) and the base look (pd_n64_textures) are set together under one
// lock, and the renderer reads them together once a frame, so no frame draws a pack
// over the other look. SetLook also sets pd_texture_pack.
struct Look {
  std::shared_ptr<const Index> index;
  bool n64 = false;  // the N64 look wanted (drawn once the ROM is loaded)
};
Look CurrentLook();
void SetLook(std::shared_ptr<const Index> index, bool n64);
// ---- The other Texture Pack slot kept ready (HANDOVER 4dp) ----
// The look the Switch Graphics key (G) would show: the slot not shown, when it can be drawn
// now (a pack whose look is known; N64 with the ROM loaded). Plume keeps that look's pictures
// for what is on screen decoded and uploaded beside the shown ones (pd_native_gpu.cpp
// standby_touch), so G switches in the next frame with no picture coming late. `valid` false
// (Texture Pack 2 None, the default): nothing is kept, no memory used for it.
struct Standby {
  std::shared_ptr<const Index> index;  // its pack (the empty index: Remastered / N64)
  bool n64 = false;
  bool valid = false;
  uint32_t serial = 0;  // new whenever the standby changes
};
// The shown look and the standby, read together (the renderer, once a frame).
struct Looks {
  Look look;
  Standby standby;
};
Looks CurrentLooks();
Standby CurrentStandby();
void SetStandby(std::shared_ptr<const Index> index, bool n64, bool valid);
// G with the standby ready: the standby shown and the look shown until now the standby, in
// one go (so the renderer keeps the old look's pictures as the new standby). False if the
// standby isn't `index`/`n64` or isn't ready (the caller applies it the usual way).
bool SwapToStandby(const std::shared_ptr<const Index>& index, bool n64);
// Plume: the standby with this serial has every picture the last frames drew (or not).
void MarkStandbyReady(uint32_t serial, bool ready);
bool StandbyReady(const std::shared_ptr<const Index>& index, bool n64);
// A pack about to be shown: Plume first decodes and uploads its pictures for the
// textures on screen (in the look `n64`), the old look drawn meanwhile, then says so
// (PreparedSerial); the app then sets the look (SetLook with this index), and Plume
// takes the prepared pictures over in that frame: the switch is one frame, no picture
// coming in late, no pause. A new request replaces the last; Cancel drops it.
struct Prepare {
  std::shared_ptr<const Index> index;
  bool n64 = false;
  uint32_t serial = 0;
};
uint32_t RequestPrepare(std::shared_ptr<const Index> index, bool n64);  // returns its serial
void CancelPrepare();
Prepare PendingPrepare();  // the renderer, once a frame
void MarkPrepared(uint32_t serial);
uint32_t PreparedSerial();
// Two of the remaster's font atlases (3515, 3516: the main menu's big font): PC-port packs
// store these as the game has them, not turned over like their other pictures (Dabs Mod
// reads them straight from the remaster; measured on parabolee's packs, HANDOVER 4dj).
// The others (3514, the dialogs' and settings pages' font; 3517) are turned over like the
// rest: 3512-3517 all left as stored drew the settings pages' text from the wrong glyphs
// (HANDOVER 4do; checked against the game's own dump: 3514 matches as stored, 3515 and 3516
// only turned over).
constexpr bool FontAtlas(int id) { return id == 3515 || id == 3516; }
// The 44 N64 texture numbers the remaster reused for other surfaces (pd_native_gpu.cpp
// remaster_reused_id has why).
bool RemasterReusedId(int id);
// Builds the index for the saved choice (once, at start).
void Init();

// ---- Texture slots (Settings > Graphics > Experimental, HANDOVER 4dn) ----
// Two looks the Switch Graphics key (G, D-pad Down) switches between: Texture Pack 1
// and Texture Pack 2 (pd_texture_slot1 / pd_texture_slot2, saved), each "Remastered",
// "N64", or an installed pack's name (drawn over the look it was made for:
// Index::n64_base); slot 2 may be "None" (the key then does nothing). Which one is
// shown: pd_texture_slot_active (1 or 2). pd_n64_textures and pd_texture_pack are
// what is drawn now, set from the shown slot. Empty slots (a settings file from
// before) are filled at start from those two: the look and pack in use become slot
// 1, the other base look slot 2.
inline constexpr const char* kSlotRemastered = "Remastered";
inline constexpr const char* kSlotN64 = "N64";
inline constexpr const char* kSlotNone = "None";
std::string Slot(int slot);  // 1 or 2; "" = not set yet
void SetSlot(int slot, const std::string& value);
int ActiveSlot();  // 1 or 2
void SetActiveSlot(int slot);
// The look a pack is made for, without choosing it: measured (StartMeasure) on a
// thread of its own once the game's texture bank is loaded (and the N64 ROM, when
// there is one): each picture's size read from its header against the texture it
// would replace in each look (a picture of another shape isn't drawn, LoadPicture),
// the look that draws more wins. Forever: the N64 look (the remaster reshaped many of
// the textures it covers); XBLA Plus (compact), whose pictures are the remaster's art
// at the N64 numbers: the remastered. Kept in the pack's folder (pd-look.txt: the look,
// the two counts, whether the N64 side was measured and the pictures counted), so once
// per pack. A pack is shown only once its look is known (HANDOVER 4do): never drawn
// over one look and then switched to the other.
// kKnown: measured, with the N64 side too if a ROM is loaded now (one measured without
// a ROM is measured again once there is one), and the pack has the pictures it had.
enum class LookState { kUnknown, kMeasuring, kKnown };
LookState GetLookState(const std::string& name);
// Starts measuring pack `name` (true if it is measuring now). False while it can't
// yet: the texture bank not loaded, or a ROM there but not loaded yet (the caller
// starts loading it).
bool StartMeasure(const std::string& name);
// The measured look (false = remastered); before it is known, from the counts above
// (a directory listing). False for a pack that isn't there.
bool PackPrefersN64(const std::string& name);
uint32_t LookGeneration();
// A pack imported or downloaded again: measured again, its index built again.
void ForgetLook(const std::string& name);
// The remastered size of bank texture `id` if a pack picture can stand in for it, and
// whether the bank is loaded (pd_native_gpu.cpp, which reads the game's memory).
using RemasteredSizeFn = bool (*)(int id, uint32_t* w, uint32_t* h);
using BankReadyFn = bool (*)();
void SetRemasteredSizeSource(RemasteredSizeFn size, BankReadyFn ready);
// Rebuilds the index when it waits for the N64 ROM and that is now loaded (each
// frame, cheap otherwise). True when it did.
bool RebuildIfWaiting();

// A pack picture as RGBA8 (any thread). False with `error` set if it can't be
// used: not a PNG or JPEG, damaged, or over 4096 on a side. `flip`: turned upside
// down (PC-port pictures). With `want_w` / `want_h` (the texture it replaces), a
// picture of another shape is refused from its header, before it is decoded
// (`wrong_shape` set): aspect ratios within about 15% of each other count as the
// same (the remaster's 172x256 for the N64's 512x768).
bool LoadPicture(const std::filesystem::path& path, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h,
                 std::string* error, bool flip = false, uint32_t want_w = 0, uint32_t want_h = 0,
                 bool* wrong_shape = nullptr);
// True when a w x h picture can stand in for a want_w x want_h texture (as above).
bool SameShape(uint32_t w, uint32_t h, uint32_t want_w, uint32_t want_h);

// ---- N64 detail textures (HANDOVER 4ds) ----
// A few N64 surfaces are drawn from two textures: the room's PD 0xC0 texture command,
// sub-command 1, loads a detail texture into tile 0 under the surface's own, and the
// RDP's detail mode (G_TD_DETAIL, combiner G_CC_TRILERP) blends it in close up. The
// remaster drew those surfaces from one texture with the detail built in, so a PC-port
// pack's picture for the surface (made to be drawn with its detail texture, e.g.
// Ultimate's flat dataDyne carpet, 0046, with its carpet pile at 0074) came out flat.
// Where the remastered look draws a PC-port pack's top-folder picture for one of these,
// the pack's detail picture is drawn into it (BakeDetail), as the N64 draws it close up.
// From the N64 BG files and models (tools/n64/bg_detail_tiles.py): texture 70 + 116
// (Defection's carpet and the dataDyne lift's floor, 4x4 repeats, the detail half the
// colour close up: prim LOD min 0xF0 = 0.5) and Grid's 3494 / 3496 / 3497 + 116 (4x4,
// prim LOD min 0xF8 / 0xF9: 0.75 / 0.78). 116 is drawn nowhere else.
struct DetailLayer {
  int base = -1, detail = -1;   // texture ids
  int repeat_s = 1, repeat_t = 1;  // detail repeats across the base texture
  float weight = 0.f;           // the detail's share of the colour, close up
};
const DetailLayer* DetailLayerFor(int id);  // nullptr: not drawn with a detail texture
// Draws `detail` (dw x dh RGBA8, tiled repeat_s x repeat_t) into `rgba` (w x h): its
// grain (difference from its own average) at `weight`, so the picture keeps its colours
// and lines. The picture may be enlarged (whole steps, at most 4096) so the detail keeps
// its own resolution; w / h are updated.
void BakeDetail(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, const std::vector<uint8_t>& detail,
                uint32_t dw, uint32_t dh, const DetailLayer& d);

// ---- Texture dump (pd_texture_dump) ----
// resources\userdata\texture-dump. Since the update after 0.9.2 it is written in the PC-port pack
// layout, so the dump is itself a pack: ext_tex\<hex id>.png the N64 pictures (an N64
// ROM imported), ext_tex\xbla\<hex id>.png the remastered ones, right way up. (0.9.2
// wrote remastered\ and n64\ with decimal names, upside down; those are left alone.)
std::filesystem::path DumpFolder();
// DumpFolder()\ext_tex (n64 = true) or ...\ext_tex\xbla, and a picture's name there.
std::filesystem::path DumpSubfolder(bool n64);
std::string DumpName(int id);
// Writes a dump picture (RGBA8 rows as the game has them) to `path`, right way up.
bool WriteDumpPng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t w, uint32_t h);
bool Dumping();
void SetDumping(bool on);
// True once per id and kind (0 remastered, 1 N64) per session: the caller writes it.
bool ClaimDump(int id, int kind);
// Writes RGBA8 rows (w * 4 bytes) as a PNG with alpha (any thread).
bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t w, uint32_t h);

}  // namespace pd::texpack
