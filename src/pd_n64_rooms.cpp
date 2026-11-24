// N64 level rooms (Experimental, N64-NOTES.md, HANDOVER 4ct).
//
// The XBLA's BG (level) files are the N64 format, but the remaster extended or
// rebuilt ~10% of the rooms (Defection's skyline, Villa's cliff tops) and stores the
// room data uncompressed. In N64 mode each room is drawn from the player's ROM:
//
// - Loading: the game loads a room with bg_load_room (sub_820D88A0: allocate, read the
//   room's bytes from the BG file, inflate, relocate, load its textures, build the
//   hit-test vertex batches). A room drawn in N64 mode is loaded a second time from the
//   ROM's BG file through that same function: its room table entries (the room's and
//   the next one's offset) are swapped for the ROM's for the call, the BG read hands
//   over the ROM's (rarezip-compressed: the XBLA's inflate still handles it) bytes, the
//   allocation comes from our own guest memory instead of the room pool (no garbage
//   collection of other rooms), and the hit-test batches and the final shrink are
//   skipped; its animated textures register in private tables (see "Animated
//   textures"). The room's 140-byte record is put back afterwards: the game keeps its
//   own gfxdata for everything but drawing.
// - Drawing: the room pass (sub_820D2658, bg_render_room_pass) is handed the ROM
//   copy: at its top level, with the room's gfxdata pointing at the copy, its opaque /
//   translucent block root in place of the game's, the per-frame lit colours
//   recomputed from the copy (room_highlight runs inside the pass) and its own
//   animated textures ticked.
// - Hit tests (bullets, explosions), lights and collision use the game's data, so a
//   network match is unaffected by either PC's look.
// - Unloading the room (sub_820D2278) or a new stage frees the copy.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "generated/default/perfect_dark_init.h"  // PPCContext, generated function declarations
#include "pd_n64_decode.h"
#include "pd_n64_textures.h"
#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(pd_n64_rooms, true, "PerfectDark",
                    "With N64 textures on: draw the level rooms from the N64 ROM's BG files (the remaster "
                    "extended or rebuilt some). Drawing only: hits and collision stay the game's own");
REXCVAR_DECLARE(bool, pd_n64_trace);

namespace {

// Guest globals (from bg_load_room / bg_load_file, sub_820D88A0 / sub_820D13E0).
constexpr uint32_t kBgRoomsPtr = 0x82627A48u;   // g_BgRooms: 20-byte entries, +0 = the room's data offset
constexpr uint32_t kRoomsPtr = 0x82629620u;     // g_Rooms: 140-byte struct room
constexpr uint32_t kGVars = 0x82648BF8u;
constexpr uint32_t kRoomCount = kGVars + 1244;  // g_Vars.roomcount
constexpr uint32_t kStageIndex = 0x825C1150u;   // g_StageIndex
constexpr uint32_t kStages = 0x825C2790u;       // g_Stages: 56-byte entries, +8 = u16 bgfileid
constexpr uint32_t kStageNum = 0x826492E0u;     // the stage being played (as pd_n64_models.cpp)
constexpr uint32_t kRoomSize = 140;
// struct room offsets (as the N64's)
constexpr uint32_t kRoomFlags = 0x00, kRoomLoaded = 0x02, kRoomGfx = 0x14, kRoomHlFrame = 0x56,
                   kRoomColours = 0x58, kRoomGfxLen = 0x80;
constexpr uint16_t kFlagHasDyntex = 0x0002;
// struct roomgfxdata: +8 opaque block root, +12 translucent block root.

inline uint32_t ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return _byteswap_ulong(v);
}
inline uint16_t ld16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return _byteswap_ushort(v);
}
inline void st32(uint8_t* base, uint32_t a, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(base + a, &v, 4);
}
inline void st16(uint8_t* base, uint32_t a, uint16_t v) {
  v = _byteswap_ushort(v);
  std::memcpy(base + a, &v, 2);
}
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

rex::memory::Memory* guest_memory() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory() : nullptr;
}
bool plausible(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; }

// The ROM's BG file for one level: its room table (offsets in the 0x0F000000 space of
// the inflated primary data) and where a room's bytes sit in the file.
struct RomBg {
  int file = -1;
  bool ok = false;
  const std::vector<uint8_t>* raw = nullptr;
  uint32_t delta = 0;               // primary inflated size - compressed size
  std::vector<uint32_t> offsets;    // room table +0 per room (index 0 = room 0)
  // Room `r`'s compressed bytes: {file offset, length}; false when not in the table.
  bool room(int r, uint32_t* at, uint32_t* len) const {
    if (r < 1 || r + 1 >= int(offsets.size())) return false;
    const uint32_t a = offsets[size_t(r)], b = offsets[size_t(r) + 1];
    if (a < 0x0F000000u || b <= a) return false;
    const int64_t pos = int64_t(a) - 0x0F000000 - int64_t(delta) + 0xC;
    if (pos < 0 || uint64_t(pos) + (b - a) > raw->size()) return false;
    *at = uint32_t(pos);
    *len = b - a;
    return true;
  }
};

struct Copy {
  uint32_t gfx = 0;    // the ROM room's gfxdata (relocated, its textures loaded)
  uint32_t block = 0;  // our allocation holding it
  uint32_t size = 0;   // ...and its size
  bool failed = false;
  bool dyntex = false;  // the copy registered animated textures (in the private tables)
};

// Animated textures (water, Deep Sea's river, Attack Ship's arrows: the N64's
// dyntex). The game registers a room's animated vertices by offset while loading it
// (dyntex_set_current_room sub_8215F3C8, set_current_type sub_8215F340, add_vertex
// sub_8215F0D0) and animates them in the room pass (dyntex_tick_room sub_8215ED60,
// with ROOMFLAG_HASDYNTEX). The ROM copies get tables of their own: the dyntex
// globals are swapped for a private set while a copy loads (so the game's own
// registration fills them, with the copy's vertex offsets and the N64's own rules,
// e.g. Air Base's water stays still) and again around the copy's room pass (so the
// game's own tick animates the copy's vertices). The game's tables are untouched.
constexpr uint32_t kDyntexWords = 0x8262E29Cu;  // 11 words: rooms max, types max, vertices,
                                                // rooms, types, vertices max, room / type
                                                // populated, rooms / types / vertices count
constexpr int kDyntexWordCount = 11;
constexpr uint32_t kDyntexCur = 0x825CD3A8u;    // current room, current type
constexpr uint32_t kDtRoomsMax = 64, kDtTypesMax = 512, kDtVerticesMax = 16384;
struct PrivateDyntex {
  uint32_t words[kDyntexWordCount] = {};
  uint32_t cur[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  uint32_t mem = 0;
};
PrivateDyntex g_dt;  // game thread only (copies load and draw there)

bool dyntex_ready() {
  if (g_dt.mem) return true;
  auto* mem = guest_memory();
  if (!mem) return false;
  const uint32_t rooms = kDtRoomsMax * 16, types = kDtTypesMax * 16, verts = kDtVerticesMax * 8;
  g_dt.mem = mem->SystemHeapAlloc(rooms + types + verts, 0x40);
  if (!g_dt.mem) return false;
  g_dt.words[0] = kDtRoomsMax;
  g_dt.words[1] = kDtTypesMax;
  g_dt.words[2] = g_dt.mem + rooms + types;  // vertices
  g_dt.words[3] = g_dt.mem;                  // rooms
  g_dt.words[4] = g_dt.mem + rooms;          // types
  g_dt.words[5] = kDtVerticesMax;
  return true;
}

// Empties the private tables (a new stage).
void dyntex_reset() {
  for (int i = 6; i < kDyntexWordCount; ++i) g_dt.words[i] = 0;
  g_dt.cur[0] = g_dt.cur[1] = 0xFFFFFFFFu;
}

// Swaps the game's dyntex globals with the private ones (call twice to undo).
void dyntex_swap(uint8_t* base) {
  for (int i = 0; i < kDyntexWordCount; ++i) {
    const uint32_t a = kDyntexWords + uint32_t(i) * 4, v = ld32(base, a);
    st32(base, a, g_dt.words[i]);
    g_dt.words[i] = v;
  }
  for (int i = 0; i < 2; ++i) {
    const uint32_t a = kDyntexCur + uint32_t(i) * 4, v = ld32(base, a);
    st32(base, a, g_dt.cur[i]);
    g_dt.cur[i] = v;
  }
}

// Whether room `room` is in the private tables (12-byte entries, +0 u16 room).
bool dyntex_has_room(const uint8_t* base, int room) {
  if (!g_dt.mem) return false;
  const uint32_t count = std::min<uint32_t>(g_dt.words[8], kDtRoomsMax);
  for (uint32_t i = 0; i < count; ++i)
    if (ld16(base, g_dt.words[3] + i * 12) == uint16_t(room)) return true;
  return false;
}

std::mutex g_mutex;
std::unordered_map<int, std::unique_ptr<RomBg>> g_bg;  // BG file id -> parsed ROM BG
uint32_t g_rooms_base = 0;                             // g_Rooms when the copies were made
int g_rooms_file = -1;                                 // ...and the stage's BG file
uint32_t g_rooms_stage = 0;                            // ...and the stage
std::unordered_map<int, Copy> g_copies;                // room -> its ROM copy
std::atomic<bool> g_drawing{false};                    // a room was drawn from the ROM this stage

// While a room's ROM copy is loaded (the game thread, inside bg_load_room).
struct Loading {
  const RomBg* bg = nullptr;
  uint32_t at = 0, len = 0;  // the room's bytes in the ROM's BG file
  uint32_t alloc = 0;        // our allocation (the loader's mema_alloc)
  uint32_t alloc_size = 0;
};
thread_local Loading* t_loading = nullptr;
thread_local int t_pass_depth = 0;

int stage_bg_file(const uint8_t* base) {
  const uint32_t index = ld32(base, kStageIndex);
  if (index >= 128) return -1;
  return ld16(base, kStages + index * 56 + 8);
}

const RomBg* rom_bg(int file) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_bg.find(file);
  if (it != g_bg.end()) return it->second->ok ? it->second.get() : nullptr;
  auto bg = std::make_unique<RomBg>();
  bg->file = file;
  const std::vector<uint8_t>* raw = pd::n64tex::RomFile(file);
  std::vector<uint8_t> prim;
  if (raw && raw->size() > 0x20 && (*raw)[12] == 0x11 && (*raw)[13] == 0x73 &&
      pd::n64dec::Inflate(raw->data() + 17, raw->size() - 17, prim) && prim.size() >= 8) {
    const uint32_t infl = be32(raw->data()), comp = be32(raw->data() + 8);
    bg->raw = raw;
    bg->delta = infl - comp;
    const uint32_t table = be32(prim.data() + 4) - 0x0F000000u;
    for (uint32_t r = 0; table + 0x14 * (r + 1) <= prim.size() && r < 4096; ++r) {
      const uint32_t v = be32(prim.data() + table + 0x14 * r);
      if (r >= 2 && (v < 0x0F000000u || v < bg->offsets.back())) break;
      bg->offsets.push_back(v);
    }
    bg->ok = bg->offsets.size() > 2;
  }
  REXLOG_INFO("[n64] BG file {}: {}", file,
              bg->ok ? fmt::format("{} rooms in the ROM's copy", bg->offsets.size() - 2)
                     : std::string("not usable from the ROM: the level's rooms stay remastered"));
  const RomBg* out = bg->ok ? bg.get() : nullptr;
  g_bg[file] = std::move(bg);
  return out;
}

void free_copy(Copy& c) {
  if (c.block)
    if (auto* mem = guest_memory()) mem->SystemHeapFree(c.block);
  c = Copy{};
}

// Drops every copy when the stage (g_Rooms or its BG file) changed. Lock held.
void check_stage_locked(const uint8_t* base) {
  const uint32_t rooms = ld32(base, kRoomsPtr);
  const int file = stage_bg_file(base);
  const uint32_t stage = ld32(base, kStageNum);
  if (rooms == g_rooms_base && file == g_rooms_file && stage == g_rooms_stage) return;
  for (auto& [room, c] : g_copies) free_copy(c);
  g_copies.clear();
  g_rooms_base = rooms;
  g_rooms_file = file;
  g_rooms_stage = stage;
  g_drawing = false;
  dyntex_reset();
}

// Loads room `room`'s ROM copy through the game's own bg_load_room. Game thread.
void load_copy(PPCContext& ctx, uint8_t* base, int room) {
  const int file = stage_bg_file(base);
  const RomBg* bg = file > 0 ? rom_bg(file) : nullptr;
  Loading load;
  Copy c;
  c.failed = true;
  const uint32_t rooms = ld32(base, kRoomsPtr), bgrooms = ld32(base, kBgRoomsPtr);
  if (bg && plausible(rooms) && plausible(bgrooms) && bg->room(room, &load.at, &load.len)) {
    const uint8_t* src = bg->raw->data() + load.at;
    // rarezip: 0x1173, then the inflated size (24 bits).
    const uint32_t inflated = (src[0] == 0x11 && src[1] == 0x73) ? (uint32_t(src[2]) << 16 | uint32_t(src[3]) << 8 | src[4]) : load.len;
    const uint32_t room_at = rooms + uint32_t(room) * kRoomSize;
    const uint32_t entry = bgrooms + uint32_t(room) * 20;
    uint8_t saved[kRoomSize];
    std::memcpy(saved, base + room_at, kRoomSize);
    const uint32_t saved_a = ld32(base, entry), saved_b = ld32(base, entry + 20);
    // The ROM's offsets for this room and the next (its length and relocation base);
    // the loader sizes its allocation from gfxdatalen: room for the inflated data and
    // its display lists a second time, as the N64 needs.
    st32(base, entry, bg->offsets[size_t(room)]);
    st32(base, entry + 20, bg->offsets[size_t(room) + 1]);
    st16(base, room_at + kRoomLoaded, 0);
    st32(base, room_at + kRoomGfxLen, ((inflated * 2 + load.len + 0x400) + 15) & ~15u);
    load.bg = bg;
    PPCRegister* args[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    uint64_t regs[8];
    for (int i = 0; i < 8; ++i) regs[i] = args[i]->u64;
    ctx.r3.u64 = uint64_t(room);
    // A new stage empties the private animated-texture tables before this room registers.
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      check_stage_locked(base);
    }
    const bool dyntex = dyntex_ready();
    if (dyntex) dyntex_swap(base);
    t_loading = &load;
    __imp__sub_820D88A0(ctx, base);
    t_loading = nullptr;
    if (dyntex) dyntex_swap(base);
    c.dyntex = dyntex && dyntex_has_room(base, room);
    for (int i = 0; i < 8; ++i) args[i]->u64 = regs[i];
    const uint32_t gfx = ld32(base, room_at + kRoomGfx);
    const bool loaded = ld16(base, room_at + kRoomLoaded) != 0;
    std::memcpy(base + room_at, saved, kRoomSize);
    st32(base, entry, saved_a);
    st32(base, entry + 20, saved_b);
    if (loaded && load.alloc && gfx >= load.alloc && gfx < load.alloc + 0x1000000u) {
      c.gfx = gfx;
      c.block = load.alloc;
      c.size = load.alloc_size;
      c.failed = false;
    } else if (load.alloc) {
      if (auto* mem = guest_memory()) mem->SystemHeapFree(load.alloc);
    }
  }
  if (REXCVAR_GET(pd_n64_trace) || (c.failed && bg))
    REXLOG_INFO("[n64] room {}: {}", room,
                c.failed ? std::string("the ROM's copy didn't load; the game's is drawn")
                         : fmt::format("drawn from the ROM's BG file ({} bytes at {:#x}){}", load.len, load.at,
                                       c.dyntex ? ", animated textures" : ""));
  std::lock_guard<std::mutex> lock(g_mutex);
  check_stage_locked(base);
  Copy& slot = g_copies[room];
  free_copy(slot);
  slot = c;
}

}  // namespace

namespace pd::n64tex {
bool RoomsActive() { return Active() && REXCVAR_GET(pd_n64_rooms); }
bool RoomsWanted() { return REXCVAR_GET(pd_n64_rooms); }
bool RoomsDrawing() { return RoomsActive() && g_drawing.load(std::memory_order_relaxed); }
}  // namespace pd::n64tex

// bg_unload_room (r3 = the room): its ROM copy goes too.
extern "C" REX_FUNC(sub_820D2278) {
  const int room = int(ctx.r3.s32);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_copies.find(room);
    if (it != g_copies.end()) {
      free_copy(it->second);
      g_copies.erase(it);
    }
  }
  __imp__sub_820D2278(ctx, base);
}

// bg_garbage_collect_rooms: not for a ROM copy (it isn't in the room pool).
extern "C" REX_FUNC(sub_820D2370) {
  if (t_loading) return;
  __imp__sub_820D2370(ctx, base);
}

// mema_alloc (r3 = the size): a ROM copy's comes from our own guest memory.
extern "C" REX_FUNC(sub_8218D4B8) {
  if (t_loading && !t_loading->alloc) {
    auto* mem = guest_memory();
    const uint32_t size = ctx.r3.u32;
    t_loading->alloc = (mem && size && size < 0x1000000u) ? mem->SystemHeapAlloc(size, 0x40) : 0;
    t_loading->alloc_size = t_loading->alloc ? size : 0;
    if (t_loading->alloc) std::memset(base + t_loading->alloc, 0, size);
    ctx.r3.u64 = t_loading->alloc;
    return;
  }
  __imp__sub_8218D4B8(ctx, base);
}

// mema_realloc (shrinking the room's block): not for ours.
extern "C" REX_FUNC(sub_8218D7D8) {
  if (t_loading) return;
  __imp__sub_8218D7D8(ctx, base);
}


// bg_find_room_vtx_batches: hit tests keep the game's.
extern "C" REX_FUNC(sub_820D2D70) {
  if (t_loading) return;
  __imp__sub_820D2D70(ctx, base);
}

// bg_load_file (r3 = destination, r4 = file offset, r5 = length): the ROM's room bytes.
extern "C" REX_FUNC(sub_820D13E0) {
  if (t_loading && t_loading->bg) {
    const uint32_t dst = ctx.r3.u32, len = ctx.r5.u32;
    const auto& raw = *t_loading->bg->raw;
    const uint32_t have = uint32_t(std::min<size_t>(len, raw.size() - t_loading->at));
    std::memcpy(base + dst, raw.data() + t_loading->at, have);
    if (len > have) std::memset(base + dst + have, 0, len - have);
    return;
  }
  __imp__sub_820D13E0(ctx, base);
}

// bg_render_room_pass (r3 = gdl, r4 = room, r5 = block, r6 = recurse): at its top level,
// in N64 mode, the room's ROM copy is drawn.
extern "C" REX_FUNC(sub_820D2658) {
  if (t_pass_depth > 0 || !pd::n64tex::RoomsActive()) {
    ++t_pass_depth;
    __imp__sub_820D2658(ctx, base);
    --t_pass_depth;
    return;
  }
  const int room = int(ctx.r4.s32);
  const uint32_t rooms = ld32(base, kRoomsPtr);
  if (room <= 0 || room >= int(ld32(base, kRoomCount)) || !plausible(rooms) || !ctx.r5.u32) {
    __imp__sub_820D2658(ctx, base);
    return;
  }
  const uint32_t room_at = rooms + uint32_t(room) * kRoomSize;
  const uint32_t game_gfx = ld32(base, room_at + kRoomGfx);
  Copy c;
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    check_stage_locked(base);
    auto it = g_copies.find(room);
    if (it != g_copies.end()) {
      c = it->second;
      known = true;
    }
  }
  if (!known) {
    load_copy(ctx, base, room);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_copies.find(room);
    if (it != g_copies.end()) c = it->second;
  }
  uint32_t block = 0;
  if (!c.failed && c.gfx && plausible(game_gfx)) {
    if (ctx.r5.u32 == ld32(base, game_gfx + 8)) block = ld32(base, c.gfx + 8);
    else if (ctx.r5.u32 == ld32(base, game_gfx + 12)) block = ld32(base, c.gfx + 12);
  }
  if (!block) {
    // No copy, or a block the copy has no counterpart for (an empty layer): the
    // N64 room has nothing there, or the game's room is drawn.
    if (!c.failed && c.gfx && plausible(game_gfx)) {
      if (ctx.r5.u32 == ld32(base, game_gfx + 8) || ctx.r5.u32 == ld32(base, game_gfx + 12)) return;  // N64: empty layer
    }
    ++t_pass_depth;
    __imp__sub_820D2658(ctx, base);
    --t_pass_depth;
    return;
  }
  // Draw the copy: its gfxdata, its lit colours (recomputed: the pass runs
  // room_highlight, which does it once a frame), its own animated textures (the
  // private dyntex tables, ticked by the game's own dyntex_tick_room in the pass).
  const uint16_t flags = ld16(base, room_at + kRoomFlags);
  const uint16_t hlframe = ld16(base, room_at + kRoomHlFrame);
  const uint32_t colours = ld32(base, room_at + kRoomColours);
  st32(base, room_at + kRoomGfx, c.gfx);
  st16(base, room_at + kRoomFlags, uint16_t(c.dyntex ? (flags | kFlagHasDyntex) : (flags & ~kFlagHasDyntex)));
  st16(base, room_at + kRoomHlFrame, uint16_t(hlframe ^ 0x8000));
  st32(base, room_at + kRoomColours, 0);
  ctx.r5.u64 = block;
  ++t_pass_depth;
  if (c.dyntex) dyntex_swap(base);
  __imp__sub_820D2658(ctx, base);
  if (c.dyntex) dyntex_swap(base);
  --t_pass_depth;
  if (c.dyntex && REXCVAR_GET(pd_n64_trace)) {
    // Once per room and stage: the game's tick ran on the copy (its private entry's
    // updated frame, +8 of the 12-byte room entry, is set).
    static std::unordered_map<int, uint32_t> logged;
    static std::unordered_map<int, std::pair<uint32_t, std::vector<uint8_t>>> snapshots;
    const uint32_t count = std::min<uint32_t>(g_dt.words[8], kDtRoomsMax);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t e = g_dt.words[3] + i * 12;
      if (ld16(base, e) != uint16_t(room)) continue;
      const uint32_t frame = ld32(base, e + 8);
      if (frame && logged[room] != g_rooms_stage + 1) {
        logged[room] = g_rooms_stage + 1;
        REXLOG_INFO("[n64] room {}: animated textures ticking (frame {}, {} type(s))", room, frame, ld16(base, e + 4));
        snapshots[room] = {frame, std::vector<uint8_t>(base + c.block, base + c.block + c.size)};
      } else if (auto it = snapshots.find(room); it != snapshots.end() && frame >= it->second.first + 120 &&
                 it->second.second.size() == c.size) {
        // Two seconds on: how much of the copy the animation changed (its vertices' s / t).
        size_t changed = 0, first = SIZE_MAX;
        for (size_t k = 0; k < c.size; ++k)
          if (base[c.block + k] != it->second.second[k]) {
            ++changed;
            if (first == SIZE_MAX) first = k;
          }
        REXLOG_INFO("[n64] room {}: animated textures moved {} byte(s) of the copy in {} frames{}", room, changed,
                    frame - it->second.first, changed ? fmt::format(" (first at +{:#x})", first) : std::string());
        snapshots.erase(it);
      }
    }
  }
  st32(base, room_at + kRoomGfx, game_gfx);
  st16(base, room_at + kRoomFlags, uint16_t((ld16(base, room_at + kRoomFlags) & ~kFlagHasDyntex) | (flags & kFlagHasDyntex)));
  st16(base, room_at + kRoomHlFrame, hlframe);
  st32(base, room_at + kRoomColours, colours);
  g_drawing.store(true, std::memory_order_relaxed);
}
