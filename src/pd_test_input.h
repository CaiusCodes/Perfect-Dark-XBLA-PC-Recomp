// Native renderer research only (branch native-renderer, never shipped): an
// input driver for the test pipe (perfect_dark_app.h TickTestPipe). The SDK's
// keyboard-and-mouse driver reports nothing while the window lacks focus, and
// test runs must not take focus from whoever is using the PC; the input system
// merges every driver's state, so injected buttons and sticks come from here
// instead, whatever window has focus. Nothing reaches the desktop.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>

namespace pd {

using rex::X_RESULT;
using rex::X_STATUS;

struct TestPad {
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint8_t lt = 0, rt = 0;
  int64_t buttons_until = 0, stick_until = 0, rstick_until = 0, triggers_until = 0;
  std::deque<uint16_t> keys;
  bool release_next = false, connected = false;
  uint32_t packet = 0;
};

class TestInputDriver final : public rex::input::InputDriver {
 public:
  TestInputDriver() : InputDriver(nullptr, 0) {}

  // Installs the driver once (the first test command).
  static void Install(rex::system::IInputSystem* input) {
    static std::atomic<bool> installed{false};
    auto* system = dynamic_cast<rex::input::InputSystem*>(input);
    if (!system || installed.exchange(true)) return;
    system->AddDriver(std::make_unique<TestInputDriver>());
  }

  // Pads 0-3 (user indices); pad 0 is the one plain test commands use.
  static void Buttons(uint16_t buttons, int ms, uint32_t pad = 0) {
    std::lock_guard lock(mutex_);
    pads_[pad & 3].buttons = buttons;
    pads_[pad & 3].buttons_until = Now() + ms;
  }
  static void Stick(int16_t lx, int16_t ly, int ms, uint32_t pad = 0) {
    std::lock_guard lock(mutex_);
    pads_[pad & 3].lx = lx;
    pads_[pad & 3].ly = ly;
    pads_[pad & 3].stick_until = Now() + ms;
  }
  static void RightStick(int16_t rx, int16_t ry, int ms, uint32_t pad = 0) {
    std::lock_guard lock(mutex_);
    pads_[pad & 3].rx = rx;
    pads_[pad & 3].ry = ry;
    pads_[pad & 3].rstick_until = Now() + ms;
  }
  static void Triggers(uint8_t lt, uint8_t rt, int ms, uint32_t pad = 0) {
    std::lock_guard lock(mutex_);
    pads_[pad & 3].lt = lt;
    pads_[pad & 3].rt = rt;
    pads_[pad & 3].triggers_until = Now() + ms;
  }
  static void Keystroke(uint16_t vk_pad, uint32_t pad = 0) {
    std::lock_guard lock(mutex_);
    pads_[pad & 3].keys.push_back(vk_pad);
  }
  // Pads other than 0 report connected only once a command used them.
  static void Connect(uint32_t pad) {
    std::lock_guard lock(mutex_);
    pads_[pad & 3].connected = true;
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  X_RESULT GetCapabilities(uint32_t user_index, uint32_t, rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (!Connected(user_index)) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      *caps = {};
      caps->type = 1;      // XINPUT_DEVTYPE_GAMEPAD
      caps->sub_type = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t user_index, rex::input::X_INPUT_STATE* out) override {
    if (!Connected(user_index)) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(mutex_);
    TestPad& p = pads_[user_index];
    const int64_t now = Now();
    *out = {};
    out->packet_number = ++p.packet;
    if (now < p.buttons_until) out->gamepad.buttons = p.buttons;
    if (now < p.stick_until) {
      out->gamepad.thumb_lx = p.lx;
      out->gamepad.thumb_ly = p.ly;
    }
    if (now < p.rstick_until) {
      out->gamepad.thumb_rx = p.rx;
      out->gamepad.thumb_ry = p.ry;
    }
    if (now < p.triggers_until) {
      out->gamepad.left_trigger = p.lt;
      out->gamepad.right_trigger = p.rt;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t user_index, rex::input::X_INPUT_VIBRATION*) override {
    return Connected(user_index) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t, rex::input::X_INPUT_KEYSTROKE* out) override {
    std::lock_guard lock(mutex_);
    for (uint32_t u = 0; u < 4; ++u) {
      if (user_index != 0xFF && user_index != u) continue;
      TestPad& p = pads_[u];
      if (!(u == 0 || p.connected) || p.keys.empty()) continue;
      // A press, then its release on the next poll.
      *out = {};
      out->virtual_key = p.keys.front();
      out->flags = p.release_next ? 0x0002 /* KEYUP */ : 0x0001 /* KEYDOWN */;
      out->user_index = uint8_t(u);
      if (p.release_next) p.keys.pop_front();
      p.release_next = !p.release_next;
      return X_ERROR_SUCCESS;
    }
    // (The lock is held: no Connected() here.)
    const bool connected = user_index == 0 || (user_index < 4 && pads_[user_index].connected);
    return user_index == 0xFF || connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  static bool Connected(uint32_t user_index) {
    if (user_index >= 4) return false;
    std::lock_guard lock(mutex_);
    return user_index == 0 || pads_[user_index].connected;
  }
  inline static std::mutex mutex_;
  inline static TestPad pads_[4];
};

}  // namespace pd
