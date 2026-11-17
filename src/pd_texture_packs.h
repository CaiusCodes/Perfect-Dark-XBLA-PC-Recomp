// Texture packs (experimental; Settings > Graphics > Experimental): folders of
// pictures named by Textures.raw id in resources\texture-packs, one chosen at a
// time (pd_texture_pack), drawn by Plume over the base textures (remastered or
// N64) for the ids the pack has. pd_assets_import.h has the naming rules.
// Also the texture dump for pack makers (pd_texture_dump).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pd::texpack {

// resources\texture-packs (a dev build: texture-packs beside the exe).
std::filesystem::path PacksFolder();
// The installed packs, by name (folder name), sorted; at most kMaxListed.
constexpr int kMaxListed = 15;
std::vector<std::string> ListPacks(bool rescan = false);

// The chosen pack's name ("" = none; pd_texture_pack, saved).
std::string ChosenPack();
// Chooses a pack ("" = none) and indexes its pictures (on the calling thread:
// a directory listing). Returns the number of pictures it has.
int ChoosePack(const std::string& name);

// The chosen pack's pictures by id (read by the renderer once a frame).
struct Index {
  std::string name;
  std::vector<std::filesystem::path> by_id;  // empty path: the pack hasn't that id
  // Ids whose picture came from N64 emulator naming (a hi-res redraw of the N64
  // texture): drawn in N64 mode only.
  std::vector<uint8_t> n64_only;
  // The pack has emulator-named pictures but the N64 ROM isn't loaded yet, so they
  // aren't matched: the index is rebuilt once it is (RebuildIfWaiting).
  bool waiting_for_rom = false;
  int hires_files = 0, hires_matched = 0;
  uint32_t version = 0;
  bool Has(int id) const { return id >= 0 && size_t(id) < by_id.size() && !by_id[size_t(id)].empty(); }
  bool N64Only(int id) const { return id >= 0 && size_t(id) < n64_only.size() && n64_only[size_t(id)]; }
};
std::shared_ptr<const Index> CurrentIndex();
// Builds the index for the saved choice (once, at start).
void Init();
// Rebuilds the index when it waits for the N64 ROM and that is now loaded (each
// frame, cheap otherwise). True when it did.
bool RebuildIfWaiting();

// A pack picture as RGBA8 (any thread). False with `error` set if it can't be
// used: not a PNG, damaged, or over 4096 on a side.
bool LoadPicture(const std::filesystem::path& path, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h,
                 std::string* error);

// ---- Texture dump (pd_texture_dump) ----
// resources\userdata\texture-dump (remastered\ and n64\ inside).
std::filesystem::path DumpFolder();
bool Dumping();
void SetDumping(bool on);
// True once per id and kind (0 remastered, 1 N64) per session: the caller writes it.
bool ClaimDump(int id, int kind);
// Writes RGBA8 rows (w * 4 bytes) as a PNG with alpha (any thread).
bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t w, uint32_t h);

}  // namespace pd::texpack
