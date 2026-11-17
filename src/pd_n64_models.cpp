// N64 models the remaster edited (Experimental, N64-NOTES.md, HANDOVER 4cs).
//
// 4cr draws the N64 display lists of every model in N64 mode, but 113 model files
// (tools/n64/models_edited.py) were edited by the remaster beyond the node links:
// Joanna's heads and bodies rebuilt with the remaster's own textures, guards whose
// embedded textures moved, doors, CI furniture, Skedar props. For those, N64 mode
// draws the ROM's copy of the file instead:
//
// - Loading: when the game loads one of them (sub_82196A58, the N64
//   modeldef_load_to_new: file read, relocation, texture set-up), it is loaded a
//   second time from the player's ROM through the game's own loader: the file size
//   (sub_822549A0) and file read (sub_822549C0) calls of that second load hand over
//   the ROM file's bytes, so the copy is relocated and its textures set up exactly as
//   the game does for its own. The game keeps using its own copy for everything but
//   drawing (hit tests, collision), so a network match is unaffected by either PC's
//   look.
// - Drawing: the two node trees are walked side by side; where they have the same
//   shape, the display-list (sub_82193B68) and gun display-list (sub_82193998) node
//   renderers are handed the ROM copy's node. A DL node's per-instance data
//   (rwdata: vertices, display list, colours) is looked up by the renderer from the
//   model instance; for the ROM node it is a block of our own, set up as the N64's
//   model_init_rw_data does (PdHookN64ModelRwData / ...Xlu hand it over).
//   The matrices are the instance's: the renderer's caller sets them from the game's
//   own nodes, and the skeletons are the N64 ones in both files.
// A file whose tree differs from the ROM's keeps the game's copy (logged once).

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "generated/default/perfect_dark_init.h"  // PPCContext, generated function declarations
#include "pd_n64_textures.h"
#include <rex/cvar.h>
#include <rex/logging/api.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, pd_n64_trace);
REXCVAR_DECLARE(bool, pd_n64_models);
int PdFileAt(uint32_t address);  // main.cpp: load address -> file id
// After the game's set-up of a ROM copy: its in-file textures' real ids (below).
void PdN64FixEmbeddedIds(uint8_t* base, int file, const std::vector<uint32_t>& gdls);

namespace {

// tools/n64/models_edited.py: the model files whose XBLA copy differs from the ROM's
// beyond the node links. The start-up logos (221-224, 1376) are kept remastered
// (pd::n64tex::RemasterOnlyModel) and skipped below.
const std::unordered_set<int> kEdited = {
    62,   66,   67,   68,   73,   85,   88,   89,   90,   91,   92,   123,  124,  125,  127,  129,  130,
    132,  134,  211,  217,  221,  222,  223,  224,  226,  227,  228,  229,  230,  234,  235,  237,  238,
    239,  247,  248,  255,  412,  420,  433,  449,  454,  458,  577,  828,  831,  1182, 1183, 1184, 1185,
    1197, 1200, 1241, 1242, 1243, 1266, 1267, 1268, 1269, 1270, 1271, 1279, 1285, 1286, 1287, 1288, 1340,
    1341, 1342, 1343, 1344, 1345, 1346, 1349, 1352, 1355, 1356, 1357, 1358, 1359, 1360, 1361, 1362, 1368,
    1369, 1370, 1372, 1373, 1374, 1376, 1380, 1383, 1384, 1385, 1386, 1387, 1868, 1871, 1872, 1874, 1879,
    1905, 1909, 1911, 1912, 1942, 1944, 1946, 1947, 1972, 1973, 1991,
};

// Node types (modelnode.type & 0xff) with display lists.
constexpr uint32_t kTypeGunDl = 0x04;
constexpr uint32_t kTypeDl = 0x18;

struct Model {
  int file = -1;
  uint32_t rom = 0;                                  // the ROM copy's modeldef
  bool tried = false;                                // the ROM copy was loaded (or tried)
  uint32_t stage = 0;                                // ...in this stage (its memory is the stage's)
  uint32_t noted = 0;                                // the stage the game loaded it in (or drew it)
  uint32_t sig[2] = {};                              // the modeldef's root and skeleton when loaded
  bool usable = false;                               // same node tree as the game's copy, or `whole`
  bool whole = false;                                // a head drawn whole from the ROM's copy
  uint32_t game_root = 0, rom_root = 0;              // ...its root nodes
  std::unordered_map<uint32_t, uint32_t> nodes;      // game node -> ROM node (DL / GUNDL)
  std::unordered_map<uint32_t, uint32_t> rwdata;     // ROM DL node -> our rwdata block
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Model> g_models;  // the game's modeldef -> its ROM copy

thread_local int t_rom_file = 0;          // the file being loaded from the ROM
thread_local uint32_t t_rom_rwdata = 0;   // rwdata for the ROM node being drawn

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

rex::memory::Memory* guest_memory() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory() : nullptr;
}

bool plausible(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; }

constexpr uint32_t kStageNum = 0x826492E0u;  // the stage being played

// A modeldef's root node and skeleton pointers (the rest of the header changes at run
// time: attaching a head adds to the body's counts): when they no longer match, that
// memory holds something else.
void read_sig(const uint8_t* base, uint32_t def, uint32_t* sig) {
  for (int i = 0; i < 2; ++i) sig[i] = ld32(base, def + 4 * i);
}
bool same_sig(const uint8_t* base, uint32_t def, const uint32_t* sig) {
  for (int i = 0; i < 2; ++i)
    if (ld32(base, def + 4 * i) != sig[i]) return false;
  return true;
}

// Game node -> {ROM node, the game's modeldef}: keyed by the node alone, as a model
// can draw nodes of another (a head hangs below the body's head spot).
struct NodeRef {
  uint32_t rom;
  uint32_t def;
};
std::unordered_map<uint32_t, NodeRef> g_nodes;
bool g_pending = false;  // a model waits for its ROM copy (under g_mutex)
// A head the remaster rebuilt (another node tree): the game's head root -> the ROM
// copy's, swapped in below the body's head spot while the body is drawn.
std::unordered_map<uint32_t, uint32_t> g_heads;
// Each drawn modeldef's head spot nodes (none for most models).
std::unordered_map<uint32_t, std::vector<uint32_t>> g_headspots;

constexpr uint32_t kTypeHeadSpot = 0x17;

void drop_copy_locked(Model& m) {
  if (auto* mem = guest_memory())
    for (auto& [node, block] : m.rwdata) mem->SystemHeapFree(block);
  m.rwdata.clear();
  for (auto& [node, rom] : m.nodes) g_nodes.erase(node);
  m.nodes.clear();
  if (m.whole) g_heads.erase(m.game_root);
  m.whole = false;
  m.game_root = m.rom_root = 0;
  m.rom = 0;
  m.usable = false;
}

void forget_locked(uint32_t modeldef) {
  g_headspots.erase(modeldef);
  auto it = g_models.find(modeldef);
  if (it == g_models.end()) return;
  drop_copy_locked(it->second);
  g_models.erase(it);
}

// Walks both node trees in the same order (node, child, next) and pairs them. False
// when their shapes differ; `why` says where. What hangs below a head spot (the head,
// attached at run time) is a model of its own and isn't walked.
bool pair_nodes(const uint8_t* base, Model& m, uint32_t game_def, uint32_t rom_def, std::string* why) {
  struct Pair { uint32_t a, b; };
  std::vector<Pair> stack{{ld32(base, game_def), ld32(base, rom_def)}};
  int count = 0;
  while (!stack.empty()) {
    const Pair p = stack.back();
    stack.pop_back();
    if (!p.a && !p.b) continue;
    if (!p.a || !p.b || !plausible(p.a) || !plausible(p.b) || ++count > 4096) {
      *why = "node " + std::to_string(count) + " missing in one copy";
      return false;
    }
    const uint32_t ta = ld16(base, p.a) & 0xFF, tb = ld16(base, p.b) & 0xFF;
    if (ta != tb) {
      *why = "node " + std::to_string(count) + " is type " + std::to_string(ta) + " in the game's copy, " +
             std::to_string(tb) + " in the ROM's";
      return false;
    }
    if (ta == kTypeDl || ta == kTypeGunDl) m.nodes[p.a] = p.b;
    stack.push_back({ld32(base, p.a + 12), ld32(base, p.b + 12)});  // next
    if (ta != kTypeHeadSpot) stack.push_back({ld32(base, p.a + 20), ld32(base, p.b + 20)});  // child
  }
  return true;
}

// Our rwdata block for ROM DL node `node`, set up as the N64's model_init_rw_data:
// {vertices, opagdl, colours after the vertices (8-byte aligned)}.
uint32_t rwdata_for(uint8_t* base, Model& m, uint32_t node) {
  auto it = m.rwdata.find(node);
  if (it != m.rwdata.end()) return it->second;
  auto* mem = guest_memory();
  const uint32_t rodata = ld32(base, node + 4);
  uint32_t block = 0;
  if (mem && plausible(rodata) && (block = mem->SystemHeapAlloc(16))) {
    const uint32_t vertices = ld32(base, rodata + 12);
    const int16_t count = int16_t(ld16(base, rodata + 16));
    st32(base, block + 0, vertices);
    st32(base, block + 4, ld32(base, rodata + 0));
    st32(base, block + 8, (vertices + uint32_t(std::max<int16_t>(count, 0)) * 12 + 7) & ~7u);
    st32(base, block + 12, 0);
  }
  m.rwdata[node] = block;
  return block;
}

// Loads the ROM copy of `file` (the game's copy at `def`) through the game's own
// loader and pairs the node trees. Called from guest code (ctx), with no lock held;
// the volatile argument registers the caller still needs are kept.
void load_copy(PPCContext& ctx, uint8_t* base, uint32_t def, int file) {
  const std::vector<uint8_t>* romfile = pd::n64tex::RomFile(file);
  uint32_t rom = 0;
  if (romfile && romfile->size() >= 32) {
    PPCRegister* args[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    uint64_t saved[8];
    for (int i = 0; i < 8; ++i) saved[i] = args[i]->u64;
    ctx.r3.u64 = uint64_t(file);
    t_rom_file = file;
    __imp__sub_82196A58(ctx, base);
    t_rom_file = 0;
    rom = ctx.r3.u32;
    for (int i = 0; i < 8; ++i) args[i]->u64 = saved[i];
  }
  Model m;
  m.file = file;
  m.rom = rom;
  m.tried = true;
  m.stage = m.noted = ld32(base, kStageNum);
  std::string why;
  m.usable = rom && plausible(rom) && pair_nodes(base, m, def, rom, &why);
  constexpr uint32_t kTypeBbox = 0x0A;
  if (!m.usable && rom && plausible(rom) && plausible(ld32(base, def)) && plausible(ld32(base, rom)) &&
      (ld16(base, ld32(base, def)) & 0xFF) == kTypeBbox && (ld16(base, ld32(base, rom)) & 0xFF) == kTypeBbox) {
    // A head (a bounding box at the root, hung below a body's head spot) the remaster
    // rebuilt: the ROM's head is drawn whole in its place. Its DL nodes map to
    // themselves, so they get rwdata blocks of their own.
    m.nodes.clear();
    std::vector<uint32_t> stack{ld32(base, rom)};
    int count = 0;
    while (!stack.empty() && ++count < 4096) {
      const uint32_t n = stack.back();
      stack.pop_back();
      if (!plausible(n)) continue;
      const uint32_t t = ld16(base, n) & 0xFF;
      if (t == kTypeDl || t == kTypeGunDl) m.nodes[n] = n;
      stack.push_back(ld32(base, n + 12));
      if (t != kTypeHeadSpot) stack.push_back(ld32(base, n + 20));
    }
    m.whole = m.usable = true;
    m.game_root = ld32(base, def);
    m.rom_root = ld32(base, rom);
    // Its distance (LOD) and toggle nodes read their on/off flag from the body's
    // rwdata by index; the game sets the flags for its own head's nodes. The ROM
    // head's nodes take the indices of the game's matching nodes (the remaster
    // reordered the same nodes); with other counts they keep their own.
    auto list = [&](uint32_t root, uint32_t type) {
      std::vector<uint32_t> out, st{root};
      int k = 0;
      while (!st.empty() && ++k < 4096) {
        const uint32_t n = st.back();
        st.pop_back();
        if (!plausible(n)) continue;
        const uint32_t t = ld16(base, n) & 0xFF;
        if (t == type) out.push_back(ld32(base, n + 4));  // its rodata
        st.push_back(ld32(base, n + 12));
        if (t != kTypeHeadSpot) st.push_back(ld32(base, n + 20));
      }
      return out;
    };
    constexpr uint32_t kTypeDistance = 0x08, kTypeToggle = 0x12;
    // Distance nodes pair by their near / far range (rodata +0 / +4), toggles in order.
    for (const auto& [type, at] : {std::pair<uint32_t, uint32_t>{kTypeDistance, 12}, {kTypeToggle, 4}}) {
      const std::vector<uint32_t> g = list(m.game_root, type), r = list(m.rom_root, type);
      if (g.size() != r.size()) continue;
      for (size_t i = 0; i < r.size(); ++i) {
        uint32_t match = g[i];
        if (type == kTypeDistance)
          for (const uint32_t c : g)
            if (plausible(c) && plausible(r[i]) && ld32(base, c) == ld32(base, r[i]) &&
                ld32(base, c + 4) == ld32(base, r[i] + 4))
              match = c;
        if (!plausible(match) || !plausible(r[i])) continue;
        const uint16_t be = _byteswap_ushort(ld16(base, match + at));
        std::memcpy(base + r[i] + at, &be, 2);
      }
    }
  }
  if (m.usable) {
    std::vector<uint32_t> gdls;
    for (const auto& [node, romnode] : m.nodes) {
      const uint32_t rodata = ld32(base, romnode + 4);
      if (!plausible(rodata)) continue;
      for (uint32_t gdl : {ld32(base, rodata + 0), ld32(base, rodata + 4)}) {
        if ((gdl >> 24) == 5) gdl = m.rom + (gdl & 0xFFFFFF);  // segment 5 = the model file
        if (plausible(gdl)) gdls.push_back(gdl);
      }
    }
    PdN64FixEmbeddedIds(base, file, gdls);
  }
  if (REXCVAR_GET(pd_n64_trace) && m.usable) {
    // The texture commands of the copy's display lists after the game's set-up.
    for (const auto& [node, romnode] : m.nodes) {
      const uint32_t rodata = ld32(base, romnode + 4);
      if (!plausible(rodata)) continue;
      for (uint32_t gdl : {ld32(base, rodata + 0), ld32(base, rodata + 4)}) {
        if ((gdl >> 24) == 5) gdl = m.rom + (gdl & 0xFFFFFF);  // segment 5 = the model file
        if (!plausible(gdl)) continue;
        std::string cmds;
        for (uint32_t a = gdl, n = 0; n < 4096; a += 8, ++n) {
          const uint32_t w0 = ld32(base, a);
          if ((w0 >> 24) == 0xB8) break;
          if ((w0 >> 24) == 0xFD || (w0 >> 24) == 0xC0 || (w0 >> 24) == 0xA5) cmds += fmt::format(" {:08X}:{:08X}", w0, ld32(base, a + 4));
        }
        REXLOG_INFO("[n64] model file {} ROM node {:08X} gdl {:08X} textures{}", file, romnode, gdl, cmds);
      }
    }
  }
  static std::mutex log_mutex;
  static std::unordered_set<int> logged;
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (logged.insert(file).second || REXCVAR_GET(pd_n64_trace)) {
      if (m.whole)
        REXLOG_INFO("[n64] model file {}: a head with another node tree ({}): the ROM's is drawn in its place", file,
                    why);
      else if (m.usable)
        REXLOG_INFO("[n64] model file {}: drawn from the ROM's copy ({} display-list nodes, {} bytes)", file,
                    m.nodes.size(), romfile->size());
      else
        REXLOG_INFO("[n64] model file {}: the ROM's copy {}; the game's copy is drawn", file,
                    !romfile ? std::string("isn't in the ROM's file table")
                    : rom    ? "has another node tree (" + why + ")"
                             : std::string("didn't load"));
    }
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_models.find(def);
  if (it == g_models.end() || it->second.file != file) return;
  std::memcpy(m.sig, it->second.sig, sizeof(m.sig));
  drop_copy_locked(it->second);
  it->second = std::move(m);
  if (it->second.usable)
    for (auto& [node, romnode] : it->second.nodes) g_nodes[node] = {romnode, def};
  if (it->second.whole) g_heads[it->second.game_root] = it->second.rom_root;
}

// Loads the ROM copies still missing: models noted before the ROM was ready, and
// drawn ones whose copy went with an earlier stage's memory (rom_node).
void load_pending(PPCContext& ctx, uint8_t* base) {
  std::vector<std::pair<uint32_t, int>> todo;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pending) return;
    g_pending = false;
    const uint32_t stage = ld32(base, kStageNum);
    for (auto& [def, m] : g_models) {
      // Models of an earlier stage whose memory nothing reused stay until forgotten.
      if (m.tried || m.noted != stage) continue;
      if (PdFileAt(def) != m.file || !same_sig(base, def, m.sig)) {  // gone
        m.tried = true;
        continue;
      }
      m.tried = true;
      todo.push_back({def, m.file});
    }
  }
  for (auto& [def, file] : todo) load_copy(ctx, base, def, file);
}

// The ROM node to draw in place of game node `node` (0 = the game's own).
uint32_t rom_node(PPCContext& ctx, uint8_t* base, uint32_t node, bool dl, uint32_t* rwdata) {
  if (!pd::n64tex::ModelsActive()) return 0;
  load_pending(ctx, base);
  std::lock_guard<std::mutex> lock(g_mutex);
  auto n = g_nodes.find(node);
  if (n == g_nodes.end()) return 0;
  const uint32_t def = n->second.def, rom = n->second.rom;
  auto it = g_models.find(def);
  if (it == g_models.end() || !it->second.usable) return 0;
  // Something else loaded where that model was (not through the two loaders).
  if (PdFileAt(def) != it->second.file || !same_sig(base, def, it->second.sig)) {
    forget_locked(def);
    return 0;
  }
  if (it->second.stage != ld32(base, kStageNum)) {
    // A model kept in a buffer of its own (the player's body) outlived the stage its
    // ROM copy was loaded in; the copy went with that stage's memory: load it again.
    it->second.tried = false;
    it->second.noted = ld32(base, kStageNum);
    g_pending = true;
    return 0;
  }
  if (dl) {
    *rwdata = rwdata_for(base, it->second, rom);
    if (!*rwdata) return 0;
  }
  return rom;
}

}  // namespace

// The rwdata block for the ROM node being drawn (0 = none): the DL node renderer's
// rwdata hooks (perfect_dark_hooks.cpp) hand it over in place of the instance's.
uint32_t PdN64RomRwData() { return t_rom_rwdata; }

// File size (r3 = the pack, r4 = the file id): the ROM file's while it is loaded
// from the ROM.
extern "C" REX_FUNC(sub_822549A0) {
  if (t_rom_file && int(ctx.r4.u32) == t_rom_file) {
    if (const std::vector<uint8_t>* file = pd::n64tex::RomFile(t_rom_file)) {
      ctx.r3.u64 = file->size();
      return;
    }
  }
  __imp__sub_822549A0(ctx, base);
}

// Textures the N64 keeps inside a model file (texconfig number 0x05000000 + offset):
// the remaster converted each one to a Textures.raw texture of the same size and wrote
// (id << 17) | offset instead, and its texture set-up no longer reads in-file textures
// (a ROM copy's showed unrelated pictures: the Attack Ship door's planet). The ROM copy
// gets the XBLA's numbers, which hold the same N64-size pictures.
struct EmbeddedTexture {
  int file;
  int index;
  uint32_t number;
};
constexpr EmbeddedTexture kEmbedded[] = {
#include "pd_n64_embedded.inc"
};

// The display lists draw an in-file texture with the N64's own RDP load: G_SETTIMG
// (0xFD) with its address 0x05000000 + offset, then the tile and load commands. The
// game's interpreter binds a Textures.raw texture only through PD's own texture
// command (0xC0: the N64's for its bank textures; the game converts it during the
// model's set-up, which is why this happens before it): w1 = the id, w0 = the s / t
// clamp-mirror modes at bits 22 / 20, TMEM offset 2 at bit 18, sub-command 2 (one
// texture), as the N64's model files have it. Each such SETTIMG becomes that command,
// its modes from the render tile's G_SETTILE (0xF5, tile 0) that follows; the RDP
// commands after it stay. A SETTIMG is recognised by its exact address, so nothing
// else in the file (vertices, colours) is touched.
// Returns how many of the commands written carry one of the `placeholder` ids.
int patch_embedded_display_lists(uint8_t* base, uint32_t dst, size_t size,
                                 const std::unordered_map<uint32_t, uint32_t>& ids,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& placeholder) {
  int placeholders = 0;
  for (uint32_t a = 0; a + 16 <= size; a += 8) {
    const uint32_t w0 = ld32(base, dst + a), w1 = ld32(base, dst + a + 4);
    if ((w0 >> 24) != 0xFD) continue;
    const auto it = ids.find(w1);
    if (it == ids.end()) continue;
    uint32_t cms = 0, cmt = 0;
    for (uint32_t b = a + 8, k = 0; k < 16 && b + 8 <= size; b += 8, ++k) {
      const uint32_t c0 = ld32(base, dst + b), c1 = ld32(base, dst + b + 4);
      if ((c0 >> 24) == 0xFD || (c0 >> 24) == 0xB8) break;
      if ((c0 >> 24) == 0xF5 && ((c1 >> 24) & 7) == 0) {
        cmt = (c1 >> 18) & 3;
        cms = (c1 >> 8) & 3;
      }
    }
    st32(base, dst + a, 0xC0000000u | (cms << 22) | (cmt << 20) | (2u << 18) | 2u);
    st32(base, dst + a + 4, it->second);
    for (const auto& [ph, real] : placeholder)
      if (it->second == ph) {
        ++placeholders;
        break;
      }
  }
  return placeholders;
}

// Placeholder ids written into a file's display lists at its last ROM read, checked
// against those put back after the set-up (PdN64FixEmbeddedIds): a placeholder left
// in place would draw an unrelated remaster texture.
std::mutex g_placeholder_mutex;
std::unordered_map<int, int> g_placeholders_written;

// PD's texture command takes a 12-bit id (the N64 had fewer than 4096 textures);
// the game's set-up turns it into its own command 0xA5, id in the low 16 bits of
// w0. An id from 4096 up goes in as a placeholder (from 4095 down, among the
// remaster's ids 3503-4095, which no N64 display list names; none this file's own)
// and is put back in the 0xA5 after the set-up (PdN64FixEmbeddedIds).
std::vector<std::pair<uint32_t, uint32_t>> embedded_placeholders(int file) {  // {placeholder, id}
  std::vector<uint32_t> high, low;
  for (const EmbeddedTexture& e : kEmbedded) {
    if (e.file != file) continue;
    const uint32_t id = e.number >> 17;
    (id >= 4096 ? high : low).push_back(id);
  }
  std::sort(high.begin(), high.end());
  high.erase(std::unique(high.begin(), high.end()), high.end());
  std::vector<std::pair<uint32_t, uint32_t>> out;
  uint32_t next = 4095;
  for (const uint32_t id : high) {
    while (std::find(low.begin(), low.end(), next) != low.end()) --next;
    out.push_back({next--, id});
  }
  return out;
}

void patch_embedded_textures(uint8_t* base, uint32_t dst, int file, size_t size) {
  if (size < 28) return;
  const int16_t count = int16_t(ld16(base, dst + 22));
  const uint32_t configs = ld32(base, dst + 24);
  if ((configs >> 24) != 5 || count <= 0) return;
  std::unordered_map<uint32_t, uint32_t> ids;  // the texture's 0x05 address -> its id
  for (const EmbeddedTexture& e : kEmbedded) {
    if (e.file != file || e.index >= count) continue;
    const uint32_t at = configs - 0x05000000u + uint32_t(e.index) * 12;
    if (at + 4 > size) continue;
    const uint32_t number = ld32(base, dst + at);
    if ((number >> 24) != 5) continue;
    uint32_t id = e.number >> 17;
    if (id >= 4096)
      for (const auto& [placeholder, real] : embedded_placeholders(file))
        if (real == id) id = placeholder;
    ids.emplace(number, id);
    st32(base, dst + at, e.number);
  }
  const int written = ids.empty() ? 0 : patch_embedded_display_lists(base, dst, size, ids, embedded_placeholders(file));
  std::lock_guard<std::mutex> lock(g_placeholder_mutex);
  g_placeholders_written[file] = written;
}


void PdN64FixEmbeddedIds(uint8_t* base, int file, const std::vector<uint32_t>& gdls) {
  const auto placeholders = embedded_placeholders(file);
  if (placeholders.empty()) return;
  int fixed = 0;
  for (const uint32_t gdl : gdls) {
    for (uint32_t a = gdl, n = 0; n < 4096; a += 8, ++n) {
      const uint32_t w0 = ld32(base, a);
      if ((w0 >> 24) == 0xB8) break;
      if ((w0 >> 24) != 0xA5) continue;
      for (const auto& [placeholder, id] : placeholders)
        if ((w0 & 0xFFFF) == placeholder) {
          st32(base, a, (w0 & 0xFFFF0000u) | id);
          ++fixed;
          break;
        }
    }
  }
  int written = 0;
  {
    std::lock_guard<std::mutex> lock(g_placeholder_mutex);
    written = g_placeholders_written[file];
  }
  if (fixed < written)
    REXLOG_WARN("[n64] model file {}: {} of {} in-file texture ids not put back (they would draw other textures)",
                file, written - fixed, written);
  else if (REXCVAR_GET(pd_n64_trace))
    REXLOG_INFO("[n64] model file {}: {} in-file texture ids put back", file, fixed);
}


// File read (r4 = the file id, r5 = the destination; returns nonzero on success):
// the ROM file's bytes while it is loaded from the ROM.
extern "C" REX_FUNC(sub_822549C0) {
  if (t_rom_file && int(ctx.r4.u32) == t_rom_file) {
    if (const std::vector<uint8_t>* file = pd::n64tex::RomFile(t_rom_file)) {
      std::memcpy(base + ctx.r5.u32, file->data(), file->size());
      patch_embedded_textures(base, ctx.r5.u32, t_rom_file, file->size());
      ctx.r3.u64 = 1;
      return;
    }
  }
  __imp__sub_822549C0(ctx, base);
}

// After the game loaded model file `file` at `def` (r3 holds it): an edited model is
// noted; its ROM copy is loaded now when the ROM is ready, else on first use.
static void note_model(PPCContext& ctx, uint8_t* base, int file, uint32_t def) {
  if (REXCVAR_GET(pd_n64_trace))
    REXLOG_INFO("[n64] modeldef load: file {} at 0x{:08X}{}", file, def, kEdited.count(file) ? " (edited)" : "");
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    forget_locked(def);  // a new model where an old one was
    if (!def || !kEdited.count(file) || pd::n64tex::RemasterOnlyModel(file)) return;
    g_models[def].file = file;
    g_models[def].noted = ld32(base, kStageNum);
    read_sig(base, def, g_models[def].sig);
    g_pending = true;
  }
  if (pd::n64tex::GetStatus() == pd::n64tex::Status::kReady && REXCVAR_GET(pd_n64_models)) {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_models[def].tried = true;
    }
    load_copy(ctx, base, def, file);
    ctx.r3.u64 = def;  // the caller gets the game's copy
  }
}

// modeldef_load_to_new (r3 = the file id; returns the modeldef).
extern "C" REX_FUNC(sub_82196A58) {
  const int file = int(ctx.r3.u64 & 0xFFFF);
  __imp__sub_82196A58(ctx, base);
  note_model(ctx, base, file, ctx.r3.u32);
}

// First-person guns load through their own state machine, bgun_tick_gun_load
// (sub_82170488: file read to the gun memory, relocation, textures over a few ticks,
// then model_allocate_rw_data). An edited gun (the IR scanner, cartridges) is noted when
// that last step runs: the file was read to the modeldef's address, so PdFileAt names it.
thread_local bool t_gun_load = false;
extern "C" REX_FUNC(sub_82170488) {
  t_gun_load = true;
  __imp__sub_82170488(ctx, base);
  t_gun_load = false;
}

// model_allocate_rw_data (r3 = the modeldef).
extern "C" REX_FUNC(sub_821957D0) {
  const uint32_t def = ctx.r3.u32;
  __imp__sub_821957D0(ctx, base);
  if (!t_gun_load || !plausible(def)) return;
  const int file = PdFileAt(def);
  if (REXCVAR_GET(pd_n64_trace))
    REXLOG_INFO("[n64] first-person gun model at 0x{:08X}: file {}", def, file);
  if (file <= 0) return;
  const uint64_t r3 = ctx.r3.u64;
  t_gun_load = false;  // the ROM copy's own load isn't a gun load
  note_model(ctx, base, file, def);
  t_gun_load = true;
  ctx.r3.u64 = r3;
}

// modeldef_load_to_addr (r3 = the file id, r4 = the buffer or 0, r5 = its size, r6 = the
// texture list; returns the modeldef): the player's own body and head.
extern "C" REX_FUNC(sub_82196988) {
  const int file = int(ctx.r3.u64 & 0xFFFF);
  __imp__sub_82196988(ctx, base);
  note_model(ctx, base, file, ctx.r3.u32);
}

// The model renderer (r3 = render data, r4 = the model). It hangs a body's head below
// the head spot on every draw, from the head spot's rwdata (the head's modeldef;
// 0x821948DC). A head the remaster rebuilt is drawn from the ROM's copy by pointing
// that rwdata at the ROM copy for this draw only; afterwards the rwdata and the head
// spot's child are put back, so hit tests and updates see the game's head.
extern "C" REX_FUNC(sub_82194778) {
  if (!pd::n64tex::ModelsActive() || !ctx.r4.u32) {
    __imp__sub_82194778(ctx, base);
    return;
  }
  load_pending(ctx, base);
  const uint32_t model = ctx.r4.u32;
  struct Swap {
    uint32_t spot, rwdata, head, child;
  };
  Swap swaps[4];
  int count = 0;
  std::vector<std::pair<uint32_t, uint32_t>> found;  // head spot, ROM head modeldef
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t def = ld32(base, model + 8);
    if (!g_heads.empty() && plausible(def)) {
      auto hs = g_headspots.find(def);
      if (hs == g_headspots.end()) {
        std::vector<uint32_t> spots, stack{ld32(base, def)};
        int n = 0;
        while (!stack.empty() && ++n < 4096) {
          const uint32_t node = stack.back();
          stack.pop_back();
          if (!plausible(node)) continue;
          const uint32_t t = ld16(base, node) & 0xFF;
          stack.push_back(ld32(base, node + 12));
          if (t == kTypeHeadSpot) spots.push_back(node);
          else stack.push_back(ld32(base, node + 20));
        }
        hs = g_headspots.emplace(def, std::move(spots)).first;
      }
      for (const uint32_t spot : hs->second) {
        auto h = g_heads.find(ld32(base, spot + 20));
        if (h == g_heads.end()) continue;
        // The ROM head's modeldef: the model whose ROM copy has that root.
        for (auto& [d, m] : g_models)
          if (m.whole && m.rom_root == h->second) found.push_back({spot, m.rom});
      }
    }
  }
  if (!found.empty()) {
    PPCRegister* args[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    uint64_t saved[8];
    for (int i = 0; i < 8; ++i) saved[i] = args[i]->u64;
    for (auto& [spot, romdef] : found) {
      if (count == 4) break;
      ctx.r3.u64 = model;
      ctx.r4.u64 = spot;
      __imp__sub_8218EF30(ctx, base);  // the head spot's rwdata
      const uint32_t rw = ctx.r3.u32;
      if (!plausible(rw)) continue;
      swaps[count++] = {spot, rw, ld32(base, rw), ld32(base, spot + 20)};
      st32(base, rw, romdef);
    }
    for (int i = 0; i < 8; ++i) args[i]->u64 = saved[i];
  }
  __imp__sub_82194778(ctx, base);
  for (int i = 0; i < count; ++i) {
    st32(base, swaps[i].rwdata, swaps[i].head);
    st32(base, swaps[i].spot + 20, swaps[i].child);
  }
}

// The DL node renderer (r3 = render data, r4 = the model, r5 = the node).
extern "C" REX_FUNC(sub_82193B68) {
  uint32_t rwdata = 0;
  if (const uint32_t node = rom_node(ctx, base, ctx.r5.u32, true, &rwdata)) {
    ctx.r5.u64 = node;
    const uint32_t outer = t_rom_rwdata;
    t_rom_rwdata = rwdata;
    __imp__sub_82193B68(ctx, base);
    t_rom_rwdata = outer;
    return;
  }
  __imp__sub_82193B68(ctx, base);
}

// The gun display-list node renderer (r3 = render data, r4 = the model, r5 = the node).
extern "C" REX_FUNC(sub_82193998) {
  uint32_t unused = 0;
  if (const uint32_t node = rom_node(ctx, base, ctx.r5.u32, false, &unused)) ctx.r5.u64 = node;
  __imp__sub_82193998(ctx, base);
}
