// N64 textures (classic look): the player's own N64 Perfect Dark ROM, decoded
// once on a thread of its own, and the switch between them and the remaster's.
// Plume (pd_native_gpu.cpp) draws a Textures.raw id 0-3502 with the N64 picture
// while Active(); N64-NOTES.md has the background.
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "pd_n64_decode.h"

namespace pd::n64tex {

enum class Status : int {
  kIdle,         // nothing loaded yet
  kLoading,      // reading and decoding on the loader thread
  kReady,        // Get() returns the textures
  kNoRom,        // no ROM in RomFolder()
  kBadRom,       // a file that isn't a Perfect Dark ROM this game knows
  kUnsupported,  // a known Perfect Dark ROM whose textures can't be used (Japanese)
  kFailed,       // read or decode error
};

// Starts loading the ROM on a thread of its own (once; later calls do nothing
// unless the last attempt found no ROM, which is retried).
void BeginLoad();
// After a ROM was imported: loads it unless one is loading or loaded already.
void Reload();
Status GetStatus();
// The loaded ROM's version ("pal-final"), or why loading failed.
std::string StatusDetail();
// Where the player's ROM goes: resources\n64 (the dev build: n64 beside the
// exe); resources\assets\n64, the first test version's folder, is still read.
std::filesystem::path RomFolder();
// True when RomFolder() holds a .z64 / .n64 / .v64 file (not checked further).
bool RomPresent();

// File `id` of the loaded ROM (N64 file id = PackedSegFile entry + 1), inflated, or
// nullptr (not Ready, an unknown table, bad data). Cached; the pointer stays valid.
const std::vector<uint8_t>* RomFile(int id);

// The N64 picture of Textures.raw id `id` (nullptr unless Ready and the id has one):
// ids 0-3502 from the ROM's texture table, and the remaster ids that stand for a
// texture the N64 kept inside a model file (IsModelTexture), decoded from that file.
const n64dec::Image* Get(int id);
constexpr int kTextureCount = 3503;  // ids 0-3502 are the N64 ones
// True for a remaster id (>= 3503) made from a texture an N64 model file kept inside
// itself: in N64 mode it is drawn from the ROM's file like the N64 ids (raw colours:
// the remaster's own copies of these have no colour curve).
bool IsModelTexture(int id);
// Every such remaster id, ascending (Dump All Textures).
std::vector<int> ModelTextureIds();

// N64 emulator hi-res packs (Rice / GLideN64 naming, pd_texture_packs.cpp): every N64
// texture id (0-3502 and the in-file ones) by its hi-res checksum (n64dec::HiresCrc).
// Built once, when Ready; nullptr before.
struct HiresEntry {
  int id;
  uint8_t fmt, siz;      // G_IM_FMT / G_IM_SIZ
  uint32_t palette[4];   // CI only (n64dec::Image::hires_pal), else 0
};
const std::unordered_multimap<uint32_t, HiresEntry>* HiresTable();

// True while the N64 textures are switched on and loaded.
bool Active();
// True while Active() and pd_n64_models: the model renderer draws the N64 meshes
// instead of the remaster's companion meshes (PdHookN64ModelLink).
bool ModelsActive();
// True while Active() and pd_n64_rooms: the level rooms are drawn from the ROM's BG
// files (pd_n64_rooms.cpp). RoomsDrawing(): and at least one room was this stage.
bool RoomsActive();
bool RoomsDrawing();
// True while Active() and pd_n64_sky: the sky function (sub_8223B0C8) takes the game's
// own N64 sky path (sky_render) instead of the remaster's skybox (PdHookN64Sky).
bool SkyActive();
// Model files (N64 file ids) kept remastered in N64 mode: ones the remaster reused
// for something else (the start-up logos). -1 (an unknown file) is not kept.
bool RemasterOnlyModel(int file);
// The switch (pd_n64_textures, saved in perfect_dark.toml).
bool Enabled();
void SetEnabled(bool on);

// RGB lookup applied to the N64 colours: the XBLA's own N64-size copies have
// them through this curve (gamma 1.17, level 0.98; tools/n64/curve_fit.py).
// pd_n64_texture_gamma / pd_n64_texture_level change it (1 / 1 = the raw N64
// colours); `version` changes when they do.
const uint8_t* ColourLut(uint32_t* version);

}  // namespace pd::n64tex
