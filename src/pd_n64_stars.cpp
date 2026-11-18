// N64 star field in N64 mode (pd_n64_stars; HANDOVER 4dg, N64-NOTES "N64 star field").
//
// The N64 draws a field of small pulsing stars over the sky in Defection,
// Extraction, Infiltration, Escape and Attack Ship (stars_render, game/stars.c in
// the decompilation). The remaster kept the set-up (sub_82164AA0 builds the star
// directions at stage load, in both modes) and the call sequence in the room
// renderer (0x820DA258: text_begin, the orthographic matrix, player_load_matrix,
// env_stop_fog, then stars_render, text_end), but compiled stars_render itself out:
// the call at 0x820DA284 goes to the shared empty stub sub_82303F50. Its skybox has
// stars painted in.
//
// PdN64DrawStars runs right after that stub (midasm hook at 0x820DA288, r3 = the
// display list) and writes what stars_render wrote, in our own code from the
// decompilation's description: the box mode (the game's own text_begin_boxmode /
// text_end_boxmode, sub_82235240 / sub_822352F8), the render mode G_RM_CLD_SURF, and
// per star a 1x1 fill rectangle in one of four pulsing colours. The XBLA's display
// list is the N64's command format (0xFA set-prim-colour, 0xF6 fill rectangle, 0xB9
// set-other-mode-L, as its own encoders sub_8224FD20 / sub_8224FEB8 write them).
//
// Projection, as stars_render: the camera's world-to-screen matrix (player
// +5992; the XBLA player block is the N64's + 0x28 here), rotation only (the
// stars are directions), y and x scaled by the player's reciprocal scales, around
// the screen middle (left + halfwidth, top + halfheight). The N64 skips whole grid
// cells facing away using cam_look; with at most 1200 stars this tests each star
// (in front of the camera, inside the view) instead, which gives the same picture.
// Stars below the horizon exist only where the N64 builds them (Attack Ship): the
// set-up only makes positive-y directions elsewhere.
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "generated/default/perfect_dark_init.h"  // PPCContext, generated function declarations
#include "pd_n64_textures.h"
#include <rex/logging/api.h>

namespace {

constexpr uint32_t kStarGlobals = 0x82630000;
constexpr uint32_t kStarPositions = kStarGlobals - 7044;  // s8 x,y,z per star
constexpr uint32_t kStarGridSize = kStarGlobals - 7048;
constexpr uint32_t kStarCount = kStarGlobals - 7056;
constexpr uint32_t kCurrentPlayer = 0x82648BF8 + 1188;  // g_Vars.currentplayer

// XBLA player offsets (N64 + 0x28 in this block).
constexpr uint32_t kScreenWidth = 5932, kScreenHeight = 5936, kScreenLeft = 5940, kScreenTop = 5944;
constexpr uint32_t kHalfWidth = 5960, kHalfHeight = 5964, kRecipScaleX = 5976, kRecipScaleY = 5980;
constexpr uint32_t kWorldToScreen = 5992;
constexpr uint32_t kStarWidth = 2;  // 2D units: one N64 pixel (of 320) in the XBLA's 640-wide space

uint32_t rd32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
float rdf(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(rd32(base, a)); }
void wr32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
bool plausible(uint32_t p) { return p >= 0x40000000u && p < 0xA0000000u; }

// colour_blend (game_006900.c): a weighted by `aweight` / 255 against b.
uint32_t blend(uint32_t a, uint32_t b, uint32_t aweight) {
  const uint32_t bweight = 0xFF - aweight;
  uint32_t out = 0;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
    out |= (((aweight * ca + bweight * cb) >> 8) & 0xFF) << shift;
  }
  return out;
}

// menu_get_sin_osc_frac / cos: a wave over the menu's 20-second timer.
float osc(float freq, bool cosine) {
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  const double t = std::fmod(std::chrono::duration<double>(clock::now() - start).count(), 20.0) / 20.0;
  const double angle = 2.0 * freq * t * 3.14159265358979;
  return float((cosine ? std::cos(angle) : std::sin(angle)) / 2.0 + 0.5);
}

}  // namespace

// The display list after stars_render's empty stub; returns where it now ends.
uint32_t PdN64DrawStars(uint8_t* base, PPCContext& live, uint32_t gdl) {
  if (!pd::n64tex::StarsActive() || !plausible(gdl)) return gdl;
  const uint32_t positions = rd32(base, kStarPositions);
  const int32_t grid = int32_t(rd32(base, kStarGridSize));
  const int32_t count = int32_t(rd32(base, kStarCount));
  const uint32_t player = rd32(base, kCurrentPlayer);
  if (!plausible(positions) || grid < 1 || grid > 4 || count < 1 || count > 2000 || !plausible(player)) return gdl;
  const uint32_t mtx = rd32(base, player + kWorldToScreen);
  if (!plausible(mtx)) return gdl;
  const uint32_t indexes = positions + uint32_t(count) * 3;  // s32 per grid cell (+1)

  float m[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m[i][j] = rdf(base, mtx + uint32_t(i * 4 + j) * 4);
  const float left = rdf(base, player + kScreenLeft), top = rdf(base, player + kScreenTop);
  const float right = left + rdf(base, player + kScreenWidth), bottom = top + rdf(base, player + kScreenHeight);
  const float midx = left + rdf(base, player + kHalfWidth), midy = top + rdf(base, player + kHalfHeight);
  const float rx = rdf(base, player + kRecipScaleX), ry = rdf(base, player + kRecipScaleY);
  if (!(right > left) || !(bottom > top) || !std::isfinite(rx) || !std::isfinite(ry)) return gdl;

  const uint32_t colours[4] = {
      blend(0xffffff7f, 0x7777777f, uint32_t(osc(2, false) * 255)),
      blend(0x0000aa7f, 0x2222ff7f, uint32_t(osc(4, false) * 255)),
      blend(0x0000ff7f, 0x5555ff7f, uint32_t(osc(2, true) * 255)),
      blend(0xaaaaff7f, 0x7777ff7f, uint32_t(osc(4, true) * 255)),
  };

  // text_begin_boxmode(gdl, 0xffffffff), the game's own.
  PPCContext ctx = live;
  ctx.r3.u64 = gdl;
  ctx.r4.u64 = 0xffffffffu;
  sub_82235240(ctx, base);
  gdl = ctx.r3.u32;
  // gDPSetRenderMode(G_RM_CLD_SURF, G_RM_CLD_SURF2).
  wr32(base, gdl, 0xB900031Du);
  wr32(base, gdl + 4, 0x404340u | 0x104340u);
  gdl += 8;

  const int32_t cells = 6 * grid * grid;
  for (int32_t cell = 0; cell < cells; ++cell) {
    const int32_t first = int32_t(rd32(base, indexes + uint32_t(cell) * 4));
    const int32_t last = int32_t(rd32(base, indexes + uint32_t(cell + 1) * 4));
    if (first < 0 || last > count || last <= first) continue;
    const int32_t group = (last - first) / 4 + 1;
    int32_t next = first;
    int colour = 0;
    for (int32_t l = first; l < last; ++l) {
      if (l == next) {
        wr32(base, gdl, 0xFA000000u);
        wr32(base, gdl + 4, colours[std::min(colour, 3)]);
        gdl += 8;
        ++colour;
        next += group;
      }
      const int8_t* p = reinterpret_cast<const int8_t*>(base + positions + uint32_t(l) * 3);
      const float x = p[0], y = p[1], z = p[2];
      const float depth = m[0][2] * x + m[1][2] * y + m[2][2] * z;
      if (!(depth < 0.0f)) continue;  // behind the camera (it looks down -z)
      const float inv = 1.0f / depth;
      const float sy = midy + (m[0][1] * x + m[1][1] * y + m[2][1] * z) * ry * inv;
      const float sx = midx - (m[0][0] * x + m[1][0] * y + m[2][0] * z) * rx * inv;
      if (!(sy > top && sy < bottom) || !(sx > left && sx < right)) continue;  // off screen
      const uint32_t ix = uint32_t(int32_t(sx)) & 0x3FF, iy = uint32_t(int32_t(sy)) & 0x3FF;
      // gDPFillRectangle(ix, iy, ix + w, iy + 1). The N64 drew 1x1 on its 320x240
      // screen; the XBLA's 2D space is 640 x 220, so one N64 pixel is about 2 x 1 here.
      wr32(base, gdl, 0xF6000000u | (((ix + kStarWidth) & 0x3FF) << 14) | (((iy + 1) & 0x3FF) << 2));
      wr32(base, gdl + 4, (ix << 14) | (iy << 2));
      gdl += 8;
    }
  }

  // text_end_boxmode(gdl), the game's own.
  ctx.r3.u64 = gdl;
  sub_822352F8(ctx, base);
  gdl = ctx.r3.u32;
  return gdl;
}
