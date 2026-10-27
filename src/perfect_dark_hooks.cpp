// Perfect Dark XBLA PC Recomp - the codegen hooks named in perfect_dark_config.toml.
//
// The recompiled code is generated from the player's own default.xex, so the port's
// changes to the game cannot be edits to it: each is a hook that ReXGlue's codegen
// calls at a guest address. The registers a hook lists in the config are passed by
// reference; a hook that returns bool decides the config's return or jump. The
// behaviour itself lives in main.cpp and perfect_dark_app.h; these are the glue.

#include "generated/default/perfect_dark_init.h"

#include <cstdint>
#include <cstring>

#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

// main.cpp
bool PdSkipArcadeLogo();
int32_t PdLegalScreenTicks();
bool PdSmoothGameClock();
uint32_t PdOsCountStep(uint32_t real_step);
void PdNoteGameFrame(uint8_t* base);
uint64_t PdTimerMicroseconds(uint64_t ticks);
void PdGpuWaitBackoff();
bool PdIdleSleep();
bool PdMenuSkipScanWanted(uint32_t dialog);
void PdMenuSkipNote(uint32_t item, uint32_t skipped);
bool PdExtraAllCheats();
bool PdExtraAllMultiplayer();
uint32_t PdRoomBrightnessFloor(uint32_t brightness);
float PdDarkRoomObjectVisibility();
int64_t PdMpUpdateRate();
int64_t PdMpUpdateRateMin();
int64_t PdMpFacingDelayMs();
uint32_t PdNetLocalInput(uint32_t input);
void PdNetLookReceive(uint32_t record, uint32_t input);
void PdNetLookSend(uint32_t input, uint32_t frame);
void PdNetLookApply(uint32_t pad);
void PdMpChooseTrack(PPCContext& ctx, uint8_t* base);

namespace {
// The calling guest thread's full context and guest memory, for a hook that needs
// more than its registers.
PPCContext& HookContext() {
  return *rex::runtime::ThreadState::Get()->context();
}
uint8_t* HookBase() {
  return rex::system::kernel_state()->memory()->virtual_membase();
}
uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return __builtin_bswap32(value);
}
void StoreU8(uint8_t* base, uint32_t address, uint8_t value) {
  base[address] = value;
}
}  // namespace

// ---- Start-up ---------------------------------------------------------------------

bool PdHookSkipArcadeLogo() {
  return PdSkipArcadeLogo();
}

// After titleTickLegal's `cmpwi cr6,r11,180`.
void PdHookLegalScreenCompare(PPCRegister& r11, PPCCRRegister& cr6, PPCXERRegister& xer) {
  cr6.compare<int32_t>(r11.s32, PdLegalScreenTicks(), xer);
}

// ---- Game clock and CPU use -------------------------------------------------------

// osGetCount after `lwz r11,92(r1)` (r11 = the counter units since the last call).
// True jumps to 0x8224FC44 with r11 = the new total, as the game's own path does.
bool PdHookOsGetCount(PPCRegister& r9, PPCRegister& r10, PPCRegister& r11) {
  uint8_t* base = HookBase();
  PdNoteGameFrame(base);
  if (!PdSmoothGameClock()) return false;
  r9.s64 = -2107310080;  // 0x82650000
  r11.u64 = PdOsCountStep(r11.u32);
  r10.u64 = LoadU32(base, r9.u32 + 8340);
  r11.u64 = r10.u64 + r11.u64;
  return true;
}

void PdHookTimerR3FromR9(PPCRegister& r3, PPCRegister& r9) {
  r3.u64 = PdTimerMicroseconds(r9.u64);
}
void PdHookTimerR7FromR9(PPCRegister& r7, PPCRegister& r9) {
  r7.u64 = PdTimerMicroseconds(r9.u64);
}
void PdHookTimerR3FromR10(PPCRegister& r3, PPCRegister& r10) {
  r3.u64 = PdTimerMicroseconds(r10.u64);
}

void PdHookGpuWaitBackoff() {
  PdGpuWaitBackoff();
}

// Before `bl Sleep(0)`: true skips the call, with Sleep's result 0.
bool PdHookIdleSleep(PPCRegister& r3) {
  if (!PdIdleSleep()) return false;
  r3.u64 = 0;
  return true;
}

// ---- Controls ---------------------------------------------------------------------

// After `ori r9,r11,2`: the Auto-Aim bit stays off.
void PdHookAutoAimOffR9(PPCRegister& r9, PPCRegister& r11) {
  r9.u64 = r11.u64 & ~uint64_t(2);
}
// After `li r4,1` (default control settings): Auto-Aim off.
void PdHookAutoAimOffR4(PPCRegister& r4) {
  r4.s64 = 0;
}
// After `ori r11,r11,9130` (a new profile's controls; 9130 includes Auto-Aim, 2): the
// bit is kept only if the stored flags at r29 already had it, as `| 9128` would.
void PdHookAutoAimOffProfile(PPCRegister& r11, PPCRegister& r29) {
  const uint32_t stored = LoadU32(HookBase(), r29.u32);
  r11.u64 = (r11.u64 & ~uint64_t(2)) | (stored & 2);
}

// After the storage-device call: X does not open the selector.
void PdHookNoStorageSelector(PPCRegister& r3) {
  r3.u64 = 0;
}
// Replaces `beq cr6,0x82282a70` with an unconditional jump (the config's jump_address).
void PdHookNoStorageLegend() {}

// ---- Menus ------------------------------------------------------------------------

// The dialog tick's start: report which of the dialog's rows the game would skip
// (greyed out or hidden - sub_821C1F38, the test its own focus move uses), so the
// mouse never puts the highlight on one. Rows are the dialog's item array, 20 bytes each.
void PdHookMenuSkipScan(PPCRegister& r3) {
  const uint32_t dialog = r3.u32;
  if (!dialog || !PdMenuSkipScanWanted(dialog)) return;
  uint8_t* base = HookBase();
  const uint32_t def = LoadU32(base, dialog + 0);
  const uint32_t items = def ? LoadU32(base, def + 8) : 0;
  for (uint32_t i = 0; items && i < 40; ++i) {
    const uint32_t item = items + i * 20;
    const uint32_t type = base[item];
    if (type == 0 || type == 29) break;
    PPCContext ctx = HookContext();
    ctx.r3.u64 = item;
    ctx.r4.u64 = dialog;
    sub_821C1F38(ctx, base);
    PdMenuSkipNote(item, ctx.r3.u32 != 0);
  }
}

// mpIsFeatureUnlocked / cheatIsUnlocked: true returns 1 at once while the extra is on.
bool PdHookAllMultiplayer(PPCRegister& r3) {
  if (!PdExtraAllMultiplayer()) return false;
  r3.u64 = 1;
  return true;
}
bool PdHookAllCheats(PPCRegister& r3) {
  if (!PdExtraAllCheats()) return false;
  r3.u64 = 1;
  return true;
}

// ---- Graphics ---------------------------------------------------------------------

void PdHookRoomBrightness(PPCRegister& r3) {
  r3.u64 = PdRoomBrightnessFloor(r3.u32);
}

// After `stfs f9,6092(r9)` (FogCol.a): the stored alpha goes through the dark-room
// remap. f12/f11/f10 still hold r, g, b; f9 itself is left as it was.
void PdHookShadeAlpha(PPCRegister& r9, PPCRegister& f9, PPCRegister& f10, PPCRegister& f11,
                      PPCRegister& f12) {
  constexpr float kShadeStart = 0.75f;
  const float r = float(f12.f64), g = float(f11.f64), b = float(f10.f64);
  float a = float(f9.f64);
  if (r <= 0.004f && g <= 0.004f && b <= 0.004f) {
    const float remapped = (a - kShadeStart) / (1.0f - kShadeStart);
    const float darkest = 1.0f - PdDarkRoomObjectVisibility();
    a = remapped < 0.0f ? 0.0f : (remapped > darkest ? darkest : remapped);
  }
  uint32_t bits;
  std::memcpy(&bits, &a, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(HookBase() + r9.u32 + 6092, &bits, 4);
}

// ---- Multiplayer ------------------------------------------------------------------

// The lobby's LIVE name check: no check wanted, none in flight, every name fine; the
// function returns without sending the request (config: return).
void PdHookNameVerification(PPCRegister& r3) {
  uint8_t* base = HookBase();
  StoreU8(base, r3.u32 + 172, 0);
  StoreU8(base, r3.u32 + 173, 0);
  StoreU8(base, r3.u32 + 174, 1);
}

void PdHookUpdateRateMinR4(PPCRegister& r4) {
  r4.s64 = PdMpUpdateRateMin();
}
void PdHookUpdateRateR5(PPCRegister& r5) {
  r5.s64 = PdMpUpdateRate();
}
void PdHookUpdateRateMinR8(PPCRegister& r8) {
  r8.s64 = PdMpUpdateRateMin();
}
void PdHookUpdateRateR7(PPCRegister& r7) {
  r7.s64 = PdMpUpdateRate();
}

// After `addi r6,r10,-200`.
void PdHookFacingDelay(PPCRegister& r6, PPCRegister& r10) {
  r6.s64 = r10.s64 - PdMpFacingDelayMs();
}

void PdHookNetLocalInput(PPCRegister& r4) {
  r4.u64 = PdNetLocalInput(r4.u32);
}
void PdHookNetLookReceive(PPCRegister& r3, PPCRegister& r4) {
  PdNetLookReceive(r3.u32, r4.u32);
}
void PdHookNetLookSend(PPCRegister& r4, PPCRegister& r5) {
  PdNetLookSend(r4.u32, r5.u32);
}
void PdHookNetLookApply(PPCRegister& r4) {
  PdNetLookApply(r4.u32 & 0xFF);
}

// In place of the tail call to mpChooseTrack (config: return).
void PdHookChooseTrack() {
  PdMpChooseTrack(HookContext(), HookBase());
}
