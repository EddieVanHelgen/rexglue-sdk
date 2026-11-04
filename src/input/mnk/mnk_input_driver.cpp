/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/sdl_virtual_key.h>
#include <rex/ui/window.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string_view>

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(mnk_mouse, false, "Input",
                    "Use the mouse for the right stick. Off means the right stick comes "
                    "from the keybind_rstick_* keys only");
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);

REXCVAR_DEFINE_STRING(keybind_a, "Semicolon,Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "Quote,Backspace", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "L", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "P", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "Q,I", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "E,O", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "1", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "3", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "F", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_up, "Up", "Input/Keybinds/Controller", "Right stick up");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "Down", "Input/Keybinds/Controller", "Right stick down");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "Left", "Input/Keybinds/Controller", "Right stick left");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "Right", "Input/Keybinds/Controller",
                      "Right stick right");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "K", "Input/Keybinds/Controller", "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Shift+Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Shift+Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Shift+Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Shift+Right", "Input/Keybinds/Controller",
                      "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Z,Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "X,Return", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

REXCVAR_DEFINE_STRING(mnk_test_script, "", "Testing",
                      "TESTING TOOL: inputs pushed as real SDL events, ';'-separated '<seconds> <bind token> "
                      "[hold seconds]' (time from the first pad poll; e.g. '20 Mouse4; 22 WheelLeft; 24 RControl "
                      "0.3'). Each input and every pad button it changes is logged ('mnk: ...')");
REXCVAR_DEFINE_BOOL(mnk_log_inputs, false, "Input",
                    "Log every key / mouse button / wheel input the keyboard and mouse driver sees, with its bind "
                    "token, and every pad button the binds change (on by itself with mnk_test_script)");

REXCVAR_DEFINE_BOOL(mnk_passthrough, false, "Input",
                    "Surface raw keystrokes to the title instead of emulating a pad, for "
                    "titles that read a USB keyboard. Enables the device on its own");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {

// A single device, so its handle is a constant.
constexpr rex::input::DeviceId kMnkDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

// Bounds the queue for titles that never call XamInputGetKeystroke.
constexpr size_t kMaxQueuedKeystrokes = 256;

// Bind values are a comma-separated list of alternatives, each optionally
// carrying modifier prefixes: "Q,I" or "Shift+W". Modifier matching is exact,
// so "Up" stays silent while Shift is held and "Shift+Up" stays silent without
// it. That is what lets the D-pad share the arrow keys. A consequence is
// that binding a bare modifier name ("Shift") can never fire, since holding it
// makes the live mask non-zero while the bind wants zero.
constexpr uint8_t kModShift = 1u << 0;
constexpr uint8_t kModCtrl = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;

using KeyState = bool[rex::ui::kInputCodeCount];

uint8_t LiveModifiers(const KeyState& key_down) {
  uint8_t mods = 0;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kShift)])
    mods |= kModShift;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kControl)])
    mods |= kModCtrl;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

// Strips leading modifier prefixes off 'token', advancing it to the bare key
// name and returning the mask they require.
uint8_t TakeModifiers(std::string_view& token) {
  uint8_t mods = 0;
  for (;;) {
    size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0) {
      break;
    }
    std::string_view head = token.substr(0, plus);
    uint8_t bit = 0;
    if (head == "Shift") {
      bit = kModShift;
    } else if (head == "Ctrl" || head == "Control") {
      bit = kModCtrl;
    } else if (head == "Alt") {
      bit = kModAlt;
    } else {
      break;
    }
    mods |= bit;
    token.remove_prefix(plus + 1);
  }
  return mods;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && s.front() == ' ') {
    s.remove_prefix(1);
  }
  while (!s.empty() && s.back() == ' ') {
    s.remove_suffix(1);
  }
  return s;
}

bool TokenPressed(const KeyState& key_down, std::string_view token, uint8_t live_mods) {
  uint8_t want = TakeModifiers(token);
  // A bind with modifiers needs them held; one without ignores them, so a key
  // bound to Shift / Control (dodge, crouch) doesn't silence movement.
  if ((live_mods & want) != want) {
    return false;
  }
  // Any key (by VirtualKey or scancode), mouse button 1-32 or wheel direction.
  const uint16_t idx = rex::ui::ParseInputCode(token);
  return idx != 0 && idx < rex::ui::kInputCodeCount && key_down[idx];
}

std::atomic<bool> mouse_look_active{true};
std::atomic<bool> menu_mode{false};
std::atomic<int64_t> last_activity_ns{0};
std::atomic<bool> zoom_wheel{false};
std::atomic<int> zoom_wheel_steps{0};

// Pseudo keys (rex::ui input codes): the wheel's four directions (WheelUp,
// WheelDown, WheelLeft, WheelRight) and mouse buttons 4-32 (Mouse4 ..).
constexpr uint16_t kVkWheelUp = rex::ui::kInputCodeWheelUp;
constexpr uint16_t kVkWheelDown = rex::ui::kInputCodeWheelDown;
constexpr uint16_t kWheelCodes[4] = {rex::ui::kInputCodeWheelUp, rex::ui::kInputCodeWheelDown,
                                     rex::ui::kInputCodeWheelLeft, rex::ui::kInputCodeWheelRight};

bool LogInputs() {
  return REXCVAR_GET(mnk_log_inputs) || !REXCVAR_GET(mnk_test_script).empty();
}

const char* PadBindName(size_t i) {
  static const char* const kNames[] = {
      "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "Back", "Start",
      "D-pad up", "D-pad down", "D-pad left", "D-pad right", "left stick up", "left stick down",
      "left stick left", "left stick right", "right stick up", "right stick down", "right stick left",
      "right stick right"};
  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}
// A wheel notch holds its key this long (at least one poll sees it).
constexpr int64_t kWheelPulseNs = 80'000'000;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void NoteActivity() {
  last_activity_ns.store(NowNs(), std::memory_order_relaxed);
}

// Menu mode's own keys: kept away from the other binds while it is on.
constexpr uint16_t kMenuKeys[] = {static_cast<uint16_t>(VirtualKey::kLButton),
                                  static_cast<uint16_t>(VirtualKey::kRButton),
                                  static_cast<uint16_t>(VirtualKey::kReturn),
                                  static_cast<uint16_t>(VirtualKey::kEscape), kVkWheelUp,
                                  kVkWheelDown};

// ui::VirtualKey follows Windows VK_ numbering; the guest wants USB HID usage
// page 0x07.
uint8_t VirtualKeyToHIDUsage(rex::ui::VirtualKey vk_enum) {
  const uint32_t vk = static_cast<uint32_t>(vk_enum);

  // The runs below are contiguous in both numbering schemes.
  if (vk >= 'A' && vk <= 'Z') {
    return static_cast<uint8_t>(vk - 'A' + 0x04);
  }
  // 0 is irregular in both digit runs; handled in the switch.
  if (vk >= '1' && vk <= '9') {
    return static_cast<uint8_t>(vk - '1' + 0x1E);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kF1) &&
      vk <= static_cast<uint32_t>(VirtualKey::kF12)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kF1) + 0x3A);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kF13) &&
      vk <= static_cast<uint32_t>(VirtualKey::kF24)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kF13) + 0x68);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kNumpad1) &&
      vk <= static_cast<uint32_t>(VirtualKey::kNumpad9)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kNumpad1) + 0x59);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kLShift) &&
      vk <= static_cast<uint32_t>(VirtualKey::kLMenu)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kLShift) + 0xE1);
  }

  switch (vk_enum) {
    case VirtualKey::k0:
      return 0x27;
    case VirtualKey::kReturn:
      return 0x28;
    case VirtualKey::kEscape:
      return 0x29;
    case VirtualKey::kBack:
      return 0x2A;
    case VirtualKey::kTab:
      return 0x2B;
    case VirtualKey::kSpace:
      return 0x2C;
    case VirtualKey::kOemMinus:
      return 0x2D;
    case VirtualKey::kOemPlus:
      return 0x2E;
    case VirtualKey::kOem4:
      return 0x2F;
    case VirtualKey::kOem6:
      return 0x30;
    case VirtualKey::kOem5:
      return 0x31;
    case VirtualKey::kOem1:
      return 0x33;
    case VirtualKey::kOem7:
      return 0x34;
    case VirtualKey::kOem3:
      return 0x35;
    case VirtualKey::kOemComma:
      return 0x36;
    case VirtualKey::kOemPeriod:
      return 0x37;
    case VirtualKey::kOem2:
      return 0x38;
    case VirtualKey::kCapital:
      return 0x39;
    case VirtualKey::kSnapshot:
      return 0x46;
    case VirtualKey::kScroll:
      return 0x47;
    case VirtualKey::kPause:
      return 0x48;
    case VirtualKey::kInsert:
      return 0x49;
    case VirtualKey::kHome:
      return 0x4A;
    case VirtualKey::kPrior:
      return 0x4B;
    case VirtualKey::kDelete:
      return 0x4C;
    case VirtualKey::kEnd:
      return 0x4D;
    case VirtualKey::kNext:
      return 0x4E;
    case VirtualKey::kRight:
      return 0x4F;
    case VirtualKey::kLeft:
      return 0x50;
    case VirtualKey::kDown:
      return 0x51;
    case VirtualKey::kUp:
      return 0x52;
    case VirtualKey::kNumLock:
      return 0x53;
    case VirtualKey::kDivide:
      return 0x54;
    case VirtualKey::kMultiply:
      return 0x55;
    case VirtualKey::kSubtract:
      return 0x56;
    case VirtualKey::kAdd:
      return 0x57;
    case VirtualKey::kNumpad0:
      return 0x62;
    case VirtualKey::kDecimal:
      return 0x63;
    case VirtualKey::kApps:
      return 0x65;
    default:
      return 0x00;
  }
}

// Sticks and triggers are included: XInput reports those as directional pad
// keys, which is what menu navigation reads.
struct PadBind {
  const std::string& keys;
  VirtualKey pad_key;
};

// Rebuilt per call because the cvars are live-editable.
std::array<PadBind, 24> PadBinds() {
  return {{
      {REXCVAR_GET(keybind_a), VirtualKey::kXInputPadA},
      {REXCVAR_GET(keybind_b), VirtualKey::kXInputPadB},
      {REXCVAR_GET(keybind_x), VirtualKey::kXInputPadX},
      {REXCVAR_GET(keybind_y), VirtualKey::kXInputPadY},
      {REXCVAR_GET(keybind_left_shoulder), VirtualKey::kXInputPadLShoulder},
      {REXCVAR_GET(keybind_right_shoulder), VirtualKey::kXInputPadRShoulder},
      {REXCVAR_GET(keybind_left_trigger), VirtualKey::kXInputPadLTrigger},
      {REXCVAR_GET(keybind_right_trigger), VirtualKey::kXInputPadRTrigger},
      {REXCVAR_GET(keybind_lstick_press), VirtualKey::kXInputPadLThumbPress},
      {REXCVAR_GET(keybind_rstick_press), VirtualKey::kXInputPadRThumbPress},
      {REXCVAR_GET(keybind_back), VirtualKey::kXInputPadBack},
      {REXCVAR_GET(keybind_start), VirtualKey::kXInputPadStart},
      {REXCVAR_GET(keybind_dpad_up), VirtualKey::kXInputPadDpadUp},
      {REXCVAR_GET(keybind_dpad_down), VirtualKey::kXInputPadDpadDown},
      {REXCVAR_GET(keybind_dpad_left), VirtualKey::kXInputPadDpadLeft},
      {REXCVAR_GET(keybind_dpad_right), VirtualKey::kXInputPadDpadRight},
      {REXCVAR_GET(keybind_lstick_up), VirtualKey::kXInputPadLThumbUp},
      {REXCVAR_GET(keybind_lstick_down), VirtualKey::kXInputPadLThumbDown},
      {REXCVAR_GET(keybind_lstick_left), VirtualKey::kXInputPadLThumbLeft},
      {REXCVAR_GET(keybind_lstick_right), VirtualKey::kXInputPadLThumbRight},
      {REXCVAR_GET(keybind_rstick_up), VirtualKey::kXInputPadRThumbUp},
      {REXCVAR_GET(keybind_rstick_down), VirtualKey::kXInputPadRThumbDown},
      {REXCVAR_GET(keybind_rstick_left), VirtualKey::kXInputPadRThumbLeft},
      {REXCVAR_GET(keybind_rstick_right), VirtualKey::kXInputPadRThumbRight},
  }};
}

}  // namespace

void SetMenuMode(bool menu) {
  menu_mode.store(menu, std::memory_order_relaxed);
}

bool IsMenuMode() {
  return menu_mode.load(std::memory_order_relaxed);
}

int64_t LastActivityNs() {
  return last_activity_ns.load(std::memory_order_relaxed);
}

void SetZoomWheel(bool on) {
  zoom_wheel.store(on, std::memory_order_relaxed);
}

int TakeZoomWheelSteps() {
  return zoom_wheel_steps.exchange(0, std::memory_order_relaxed);
}

void SetMouseLookActive(bool active) {
  mouse_look_active.store(active, std::memory_order_relaxed);
}

bool IsMouseLookActive() {
  return mouse_look_active.load(std::memory_order_relaxed);
}

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  DetachFromWindow();
}

X_STATUS MnkInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = window;
    }
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  DetachFromWindow();
}

void MnkInputDriver::DetachFromWindow() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  window->app_context().CallInUIThreadSynchronous([this, window] {
    // Detach first so nothing new is queued, then run out what already was.
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = nullptr;
    }
    if (mouse_capture_update_queued_.load(std::memory_order_relaxed)) {
      window->app_context().ExecutePendingFunctionsFromUIThread();
    }
    ReleaseMouseCaptureFromUIThread(window);
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  });
}

bool MnkInputDriver::IsEnabled() const {
  // Passthrough enables the device on its own.
  return REXCVAR_GET(mnk_mode) || REXCVAR_GET(mnk_passthrough);
}

static bool IsPassthroughEnabled() {
  return REXCVAR_GET(mnk_passthrough);
}

static bool IsBindPressed(const KeyState& key_down, const std::string& cvar_val) {
  const uint8_t live_mods = LiveModifiers(key_down);
  std::string_view rest(cvar_val);
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    if (comma == std::string_view::npos) {
      rest = std::string_view();
    } else {
      rest.remove_prefix(comma + 1);
    }
    token = TrimSpaces(token);
    if (!token.empty() && TokenPressed(key_down, token, live_mods)) {
      return true;
    }
  }
  return false;
}

void MnkInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Disabled means no device at all, so it never occupies a guest user slot.
  if (!IsEnabled()) {
    return;
  }
  DeviceInfo info;
  info.id = kMnkDevice;
  info.name = IsPassthroughEnabled() ? "Keyboard" : "Keyboard and Mouse";
  info.subtype =
      IsPassthroughEnabled() ? XINPUT_DEVSUBTYPE_USB_KEYBOARD : XINPUT_DEVSUBTYPE_GAMEPAD;
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT MnkInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    if (IsPassthroughEnabled()) {
      out_caps->type = XINPUT_DEVTYPE_KEYBOARD;
      out_caps->sub_type = XINPUT_DEVSUBTYPE_USB_KEYBOARD;
      return X_ERROR_SUCCESS;
    }
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Keys reach the guest through GetDeviceKeystroke only, leaving the guest
  // user free for a real controller.
  if (IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (!REXCVAR_GET(mnk_test_script).empty()) {
    RunTestScript();
  }

  // Mouse look is opt in. Without this gate, keyboard input alone would hide
  // and lock the cursor, breaking the ImGui overlays.
  QueueMouseCaptureUpdate(IsEnabled() && REXCVAR_GET(mnk_mouse) && IsMouseLookActive() &&
                          has_focus_ && is_active());

  if (!is_active() || !Focused()) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  // PadBinds() order: a b x y lb rb lt rt l3 r3 back start, dpad up down left
  // right, lstick up down left right, rstick up down left right.
  const uint32_t mask = BoundMaskLocked();
  const auto on = [mask](int i) { return (mask >> i) & 1u; };
  uint16_t buttons = 0;
  if (on(0)) buttons |= X_INPUT_GAMEPAD_A;
  if (on(1)) buttons |= X_INPUT_GAMEPAD_B;
  if (on(2)) buttons |= X_INPUT_GAMEPAD_X;
  if (on(3)) buttons |= X_INPUT_GAMEPAD_Y;
  if (on(4)) buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (on(5)) buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (on(8)) buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (on(9)) buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (on(10)) buttons |= X_INPUT_GAMEPAD_BACK;
  if (on(11)) buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (on(12)) buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (on(13)) buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (on(14)) buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (on(15)) buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = on(6) ? 0xFF : 0;
  uint8_t rt = on(7) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (on(18)) lx -= INT16_MAX;
  if (on(19)) lx += INT16_MAX;
  if (on(16)) ly += INT16_MAX;
  if (on(17)) ly -= INT16_MAX;

  int32_t rx = 0;
  int32_t ry = 0;
  if (on(22)) rx -= INT16_MAX;
  if (on(23)) rx += INT16_MAX;
  if (on(20)) ry += INT16_MAX;
  if (on(21)) ry -= INT16_MAX;
  // The wheel's pulse ends after a poll has seen it.
  ReleaseWheelLocked();

  if (REXCVAR_GET(mnk_mouse) && IsMouseLookActive()) {
    double sensitivity = REXCVAR_GET(mnk_sensitivity);
    constexpr double kBaseScale = 200.0;
    rx += static_cast<int32_t>(double(mouse_dx_) * sensitivity * kBaseScale);
    ry += static_cast<int32_t>(double(-mouse_dy_) * sensitivity * kBaseScale);
  }
  // Drained unconditionally: deltas keep accumulating in OnMouseMove while the
  // mouse is off, and toggling it on would otherwise dump the whole backlog
  // into one frame as a camera snap.
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (!IsPassthroughEnabled()) {
    ReleaseWheelLocked();
  }
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // InputSystem stamps the guest user this device is assigned to.
  ks.user_index = 0;
  ks.hid_code = 0;
  PushKeystroke(ks);
}

void MnkInputDriver::PushKeystroke(const X_INPUT_KEYSTROKE& ks) {
  if (keystroke_queue_.size() >= kMaxQueuedKeystrokes) {
    keystroke_queue_.pop();
  }
  keystroke_queue_.push(ks);
}

void MnkInputDriver::EnqueueRawKeystroke(const rex::ui::KeyEvent& e, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = static_cast<uint16_t>(e.virtual_key());
  ks.unicode = 0;
  ks.user_index = 0;
  ks.hid_code = VirtualKeyToHIDUsage(e.virtual_key());

  uint16_t flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // Already down, so this is the OS auto-repeat.
  if (down && e.prev_state()) {
    flags |= X_INPUT_KEYSTROKE_REPEAT;
  }
  if (e.is_shift_pressed()) {
    flags |= X_INPUT_KEYSTROKE_SHIFT;
  }
  if (e.is_ctrl_pressed()) {
    flags |= X_INPUT_KEYSTROKE_CTRL;
  }
  if (e.is_alt_pressed()) {
    flags |= X_INPUT_KEYSTROKE_ALT;
  }
  ks.flags = flags;
  PushKeystroke(ks);
}

uint32_t MnkInputDriver::BoundMaskLocked() const {
  const auto binds = PadBinds();
  const bool menu = IsMenuMode();
  static thread_local KeyState masked;
  std::memcpy(masked, key_down_, sizeof(masked));
  if (menu) {
    for (uint16_t k : kMenuKeys) {
      masked[k] = false;
    }
  }
  uint32_t pressed = 0;
  for (size_t i = 0; i < binds.size(); i++) {
    bool down = IsBindPressed(menu ? masked : key_down_, binds[i].keys);
    if (menu) {
      const auto k = [this](uint16_t vk) { return key_down_[vk]; };
      switch (binds[i].pad_key) {
        case VirtualKey::kXInputPadA:
          down = down || k(static_cast<uint16_t>(VirtualKey::kLButton)) ||
                 k(static_cast<uint16_t>(VirtualKey::kReturn));
          break;
        case VirtualKey::kXInputPadB:
          down = down || k(static_cast<uint16_t>(VirtualKey::kRButton)) ||
                 k(static_cast<uint16_t>(VirtualKey::kEscape));
          break;
        case VirtualKey::kXInputPadDpadUp:
          down = down || k(kVkWheelUp);
          break;
        case VirtualKey::kXInputPadDpadDown:
          down = down || k(kVkWheelDown);
          break;
        default:
          break;
      }
    }
    if (down) {
      pressed |= uint32_t(1) << i;
    }
  }
  return pressed;
}

void MnkInputDriver::ReleaseWheelLocked() {
  const int64_t now = NowNs();
  bool changed = false;
  for (int i = 0; i < 4; ++i) {
    if (wheel_release_ns_[i] && now >= wheel_release_ns_[i]) {
      wheel_release_ns_[i] = 0;
      SetInputLocked(kWheelCodes[i], false, "");
      changed = true;
    }
  }
  if (changed) {
    RefreshBoundKeystrokesLocked();
  }
}

void MnkInputDriver::RefreshBoundKeystrokesLocked() {
  const auto binds = PadBinds();
  const uint32_t pressed = BoundMaskLocked();

  // The whole table is diffed rather than the key that moved: binds carry
  // modifiers, so Shift alone can flip several of them at once.
  const uint32_t changed = pressed ^ bound_pressed_;
  bound_pressed_ = pressed;
  if (!changed) {
    return;
  }
  const bool log = LogInputs();
  for (size_t i = 0; i < binds.size(); i++) {
    const uint32_t bit = uint32_t(1) << i;
    if (changed & bit) {
      EnqueueKeystroke(static_cast<uint16_t>(binds[i].pad_key), (pressed & bit) != 0);
      if (log) {
        REXLOG_INFO("mnk: pad {} {} (bind \"{}\"{})", PadBindName(i), (pressed & bit) ? "pressed" : "released",
                    binds[i].keys, IsMenuMode() ? ", menu mode" : "");
      }
    }
  }
}

void MnkInputDriver::QueueMouseCaptureUpdate(bool should_capture) {
  mouse_capture_requested_.store(should_capture, std::memory_order_relaxed);
  bool already_queued = false;
  if (!mouse_capture_update_queued_.compare_exchange_strong(already_queued, true)) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  if (!attached_window_) {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    return;
  }
  // Deferred, not CallInUIThread: running inline would re-enter state_mutex_.
  attached_window_->app_context().CallInUIThreadDeferred([this] {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    ApplyMouseCaptureFromUIThread();
  });
}

void MnkInputDriver::ApplyMouseCaptureFromUIThread() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  bool should_capture = mouse_capture_requested_.load(std::memory_order_relaxed);
  if (should_capture == mouse_captured_) {
    return;
  }
  if (!should_capture) {
    ReleaseMouseCaptureFromUIThread(window);
    return;
  }
  mouse_captured_ = true;
  precapture_cursor_visibility_ = window->GetCursorVisibility();
  window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  window->CaptureMouse();
  relative_mouse_mode_ = window->SetRelativeMouseMode(true);
  if (!relative_mouse_mode_) {
    REXLOG_WARN("Pointer lock unavailable, mouse look falls back to recentering the cursor");
  }
  // Reset deltas to avoid a spike on capture start
  std::lock_guard lock(state_mutex_);
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

void MnkInputDriver::ReleaseMouseCaptureFromUIThread(rex::ui::Window* window) {
  if (!mouse_captured_) {
    return;
  }
  mouse_captured_ = false;
  {
    // Buttons 6+ come from Raw Input, which stops with the pointer lock: let
    // go of any still held, or its release would never arrive.
    std::lock_guard lock(state_mutex_);
    bool any = false;
    for (int b = 6; b <= 32; ++b) {
      const uint16_t code = rex::ui::InputCodeForMouseButton(b);
      any = any || key_down_[code];
      key_down_[code] = false;
    }
    if (any) {
      RefreshBoundKeystrokesLocked();
    }
  }
  window->SetCursorVisibility(precapture_cursor_visibility_);
  window->SetRelativeMouseMode(false);
  relative_mouse_mode_ = false;
  window->ReleaseMouse();
}

void MnkInputDriver::RecenterCursorFromUIThread(int32_t x, int32_t y) {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  int32_t width = int32_t(window->GetActualPhysicalWidth());
  int32_t height = int32_t(window->GetActualPhysicalHeight());
  if (width <= 0 || height <= 0) {
    return;
  }
  // Only once the pointer has drifted well off the middle, to keep warps rare.
  if (std::abs(x - width / 2) < width / 4 && std::abs(y - height / 2) < height / 4) {
    return;
  }
  int32_t center_x = 0;
  int32_t center_y = 0;
  if (window->WarpMouseToCenter(center_x, center_y)) {
    prev_mouse_x_ = center_x;
    prev_mouse_y_ = center_y;
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < rex::ui::kInputCodeCount) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::SetInputLocked(uint16_t code, bool down, const char* what) {
  if (code == 0 || code >= rex::ui::kInputCodeCount) {
    return;
  }
  if (LogInputs() && key_down_[code] != down) {
    REXLOG_INFO("mnk: input {} [{}] {}{}", rex::ui::InputCodeDisplayName(code), rex::ui::InputCodeToken(code),
                down ? "down" : "up", what);
  }
  key_down_[code] = down;
}

void MnkInputDriver::SetKeyEventLocked(const rex::ui::KeyEvent& e, bool down) {
  // Both names a key has: its VirtualKey (shared by left / right Shift, Ctrl,
  // Alt) and its scancode's own code (each physical key; keys with no
  // VirtualKey have only this). The input is logged once, under the name a
  // bind is written with.
  const int sc = e.scancode();
  const bool has_sc = sc > 0 && sc < int(rex::ui::kInputCodeCount - rex::ui::kInputCodeScancodeBase);
  const uint16_t sc_code = has_sc ? uint16_t(rex::ui::kInputCodeScancodeBase + sc) : 0;
  const bool sc_is_name = has_sc && rex::ui::InputCodeForScancode(sc) == sc_code;
  const uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (vk != 0 && vk < 256) {
    if (sc_is_name) {
      key_down_[vk] = down;
    } else {
      SetInputLocked(vk, down, "");
    }
  }
  if (sc_is_name) {
    SetInputLocked(sc_code, down, "");
  } else if (has_sc) {
    key_down_[sc_code] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !Focused())
    return;
  std::lock_guard lock(state_mutex_);
  if (IsPassthroughEnabled()) {
    EnqueueRawKeystroke(e, true);
    return;
  }
  SetKeyEventLocked(e, true);
  RefreshBoundKeystrokesLocked();
  NoteActivity();
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  if (IsPassthroughEnabled()) {
    EnqueueRawKeystroke(e, false);
    return;
  }
  SetKeyEventLocked(e, false);
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnKeyChar(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !IsPassthroughEnabled())
    return;
  // WM_CHAR convention: the codepoint arrives after its key-down, so it is
  // attached to the keystroke already queued for that key.
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return;
  }
  X_INPUT_KEYSTROKE& last = keystroke_queue_.back();
  const uint16_t flags = last.flags;
  if (!(flags & X_INPUT_KEYSTROKE_KEYDOWN) || last.unicode != 0) {
    return;
  }
  last.unicode = static_cast<uint16_t>(e.virtual_key());
  last.flags = static_cast<uint16_t>(flags | X_INPUT_KEYSTROKE_VALIDUNICODE);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !Focused())
    return;
  std::lock_guard lock(state_mutex_);
  // Buttons 1-5 and, through Raw Input, 6-32 (the enum keeps the button number).
  SetInputLocked(rex::ui::InputCodeForMouseButton(static_cast<int>(e.button())), true, "");
  RefreshBoundKeystrokesLocked();
  NoteActivity();
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  SetInputLocked(rex::ui::InputCodeForMouseButton(static_cast<int>(e.button())), false, "");
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !Focused() || IsPassthroughEnabled() || (e.scroll_y() == 0 && e.scroll_x() == 0))
    return;
  std::lock_guard lock(state_mutex_);
  // Zoom wheel: while the left trigger's bind is held (aiming), up / down go
  // to the title's zoom, not to their binds. Notches of 120; smaller
  // (high-resolution) amounts add up.
  if (e.scroll_y() != 0 && zoom_wheel.load(std::memory_order_relaxed) && !IsMenuMode() &&
      (bound_pressed_ & (uint32_t(1) << 6))) {
    zoom_wheel_acc_ += e.scroll_y();
    const int steps = zoom_wheel_acc_ / int(rex::ui::MouseEvent::kScrollPerDetent);
    if (steps != 0) {
      zoom_wheel_acc_ -= steps * int(rex::ui::MouseEvent::kScrollPerDetent);
      zoom_wheel_steps.fetch_add(steps, std::memory_order_relaxed);
      if (LogInputs()) {
        REXLOG_INFO("mnk: input {} [{}]: zoom wheel ({} step{}, aim held)", steps > 0 ? "Wheel up" : "Wheel down",
                    steps > 0 ? "WheelUp" : "WheelDown", steps, steps == 1 || steps == -1 ? "" : "s");
      }
    }
    NoteActivity();
    if (e.scroll_x() == 0) {
      return;
    }
  }
  // y + = away from the user (up); x + = tilted right. One pulse per event;
  // notches arriving during a pulse extend it.
  const int64_t release = NowNs() + kWheelPulseNs;
  auto pulse = [&](int i) {
    SetInputLocked(kWheelCodes[i], true, "");
    wheel_release_ns_[i] = release;
  };
  if (e.scroll_y() != 0 && !(zoom_wheel.load(std::memory_order_relaxed) && !IsMenuMode() &&
                              (bound_pressed_ & (uint32_t(1) << 6)))) {
    pulse(e.scroll_y() > 0 ? 0 : 1);
  }
  if (e.scroll_x() != 0) {
    pulse(e.scroll_x() > 0 ? 3 : 2);
  }
  RefreshBoundKeystrokesLocked();
  NoteActivity();
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !Focused())
    return;
  int32_t x = e.x();
  int32_t y = e.y();
  if (e.dx() != 0.0f || e.dy() != 0.0f || x != prev_mouse_x_ || y != prev_mouse_y_) {
    NoteActivity();
  }
  {
    std::lock_guard lock(state_mutex_);
    if (relative_mouse_mode_) {
      // The pointer is locked, so the absolute position no longer moves.
      mouse_dx_ += e.dx();
      mouse_dy_ += e.dy();
    } else {
      mouse_dx_ += float(x - prev_mouse_x_);
      mouse_dy_ += float(y - prev_mouse_y_);
    }
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
  // Without a pointer lock the cursor still has to be kept off the edges.
  if (mouse_captured_ && !relative_mouse_mode_) {
    RecenterCursorFromUIThread(x, y);
  }
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  // Withdraw the request too, or an update queued before the focus loss grabs
  // the cursor straight back.
  mouse_capture_requested_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard lock(state_mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    // Releases landing on another window never arrive here as key-ups.
    RefreshBoundKeystrokesLocked();
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
  }
  if (attached_window_) {
    ReleaseMouseCaptureFromUIThread(attached_window_);
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_ = true;
}

bool MnkInputDriver::Focused() const {
  return has_focus_ || !REXCVAR_GET(mnk_test_script).empty();
}

namespace {

// One input as the SDL event the platform would send for it.
bool PushTestSdlInput(const std::string& token, bool down) {
  const uint16_t code = rex::ui::ParseInputCode(token);
  if (code == 0) {
    return false;
  }
  SDL_WindowID window_id = 0;
  int count = 0;
  if (SDL_Window** windows = SDL_GetWindows(&count)) {
    if (count > 0) {
      window_id = SDL_GetWindowID(windows[0]);
    }
    SDL_free(windows);
  }
  SDL_Event ev{};
  int button = 0;
  for (int b = 1; b <= 32 && !button; ++b) {
    if (rex::ui::InputCodeForMouseButton(b) == code) {
      button = b;
    }
  }
  if (button) {
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.windowID = window_id;
    // Input codes count 1 left, 2 right, 3 middle (as MouseEvent::Button); SDL has 2 middle, 3 right.
    ev.button.button = Uint8(button == 2 ? SDL_BUTTON_RIGHT : button == 3 ? SDL_BUTTON_MIDDLE : button);
    ev.button.down = down;
    ev.button.clicks = 1;
  } else if (code == rex::ui::kInputCodeWheelUp || code == rex::ui::kInputCodeWheelDown ||
             code == rex::ui::kInputCodeWheelLeft || code == rex::ui::kInputCodeWheelRight) {
    if (!down) {
      return true;  // a wheel notch has no release
    }
    ev.type = SDL_EVENT_MOUSE_WHEEL;
    ev.wheel.windowID = window_id;
    ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    ev.wheel.x = code == rex::ui::kInputCodeWheelLeft    ? -1.0f
                 : code == rex::ui::kInputCodeWheelRight ? 1.0f
                                                         : 0.0f;
    ev.wheel.y = code == rex::ui::kInputCodeWheelUp     ? 1.0f
                 : code == rex::ui::kInputCodeWheelDown ? -1.0f
                                                        : 0.0f;
  } else {
    int scancode = 0;
    if (code >= rex::ui::kInputCodeScancodeBase) {
      scancode = code - rex::ui::kInputCodeScancodeBase;
    } else {
      for (int sc = 1; sc < SDL_SCANCODE_COUNT && !scancode; ++sc) {
        if (static_cast<uint16_t>(rex::ui::TranslateSDLScancode(static_cast<SDL_Scancode>(sc))) == code) {
          scancode = sc;
        }
      }
    }
    if (!scancode) {
      return false;
    }
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.windowID = window_id;
    ev.key.scancode = static_cast<SDL_Scancode>(scancode);
    ev.key.down = down;
    ev.key.repeat = false;
  }
  return SDL_PushEvent(&ev);
}

}  // namespace

void MnkInputDriver::RunTestScript() {
  std::lock_guard lock(test_mutex_);
  const int64_t now = NowNs();
  if (!test_parsed_) {
    test_parsed_ = true;
    test_start_ns_ = now;
    std::string_view rest(REXCVAR_GET(mnk_test_script));
    while (!rest.empty()) {
      const size_t semi = rest.find(';');
      std::string_view item = TrimSpaces(rest.substr(0, semi));
      rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
      TestInput in;
      size_t sp = item.find(' ');
      const std::string at(item.substr(0, sp));
      item = sp == std::string_view::npos ? std::string_view() : TrimSpaces(item.substr(sp + 1));
      sp = item.find(' ');
      in.token = std::string(item.substr(0, sp));
      const std::string hold =
          sp == std::string_view::npos ? std::string() : std::string(TrimSpaces(item.substr(sp + 1)));
      in.at = std::atof(at.c_str());
      if (!hold.empty()) {
        in.hold = std::atof(hold.c_str());
      }
      if (!in.token.empty()) {
        test_inputs_.push_back(in);
      }
    }
    REXLOG_INFO("mnk: test script: {} inputs", test_inputs_.size());
  }
  const double t = double(now - test_start_ns_) / 1e9;
  for (TestInput& in : test_inputs_) {
    if (in.sent == 0 && t >= in.at) {
      in.sent = 1;
      const bool ok = PushTestSdlInput(in.token, true);
      REXLOG_INFO("mnk: test t={:.2f} s: {} [{}] pressed{}", t,
                  rex::ui::InputCodeDisplayName(rex::ui::ParseInputCode(in.token)), in.token,
                  ok ? "" : " (NOT pushed: unknown token or SDL refused)");
    }
    if (in.sent == 1 && t >= in.at + in.hold) {
      in.sent = 2;
      PushTestSdlInput(in.token, false);
    }
  }
}

}  // namespace rex::input::mnk
