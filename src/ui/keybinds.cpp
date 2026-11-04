/**
 * @file        ui/keybinds.cpp
 * @brief       Key binding implementation. See keybinds.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/keybinds.h>
#include <rex/cvar.h>
#include <rex/ui/sdl_virtual_key.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <charconv>
#include <mutex>
#include <string>
#include <deque>
#include <unordered_map>

namespace rex::ui {

using rex::ui::VirtualKey;

// In priority order: where two names share a key, the first is the one written back.
static const std::pair<const char*, VirtualKey> kKeyTable[] = {
    // Function keys
    {"F1", VirtualKey::kF1},
    {"F2", VirtualKey::kF2},
    {"F3", VirtualKey::kF3},
    {"F4", VirtualKey::kF4},
    {"F5", VirtualKey::kF5},
    {"F6", VirtualKey::kF6},
    {"F7", VirtualKey::kF7},
    {"F8", VirtualKey::kF8},
    {"F9", VirtualKey::kF9},
    {"F10", VirtualKey::kF10},
    {"F11", VirtualKey::kF11},
    {"F12", VirtualKey::kF12},
    {"F13", VirtualKey::kF13},
    {"F14", VirtualKey::kF14},
    {"F15", VirtualKey::kF15},
    {"F16", VirtualKey::kF16},
    {"F17", VirtualKey::kF17},
    {"F18", VirtualKey::kF18},
    {"F19", VirtualKey::kF19},
    {"F20", VirtualKey::kF20},
    {"F21", VirtualKey::kF21},
    {"F22", VirtualKey::kF22},
    {"F23", VirtualKey::kF23},
    {"F24", VirtualKey::kF24},
    // Letters
    {"A", VirtualKey::kA},
    {"B", VirtualKey::kB},
    {"C", VirtualKey::kC},
    {"D", VirtualKey::kD},
    {"E", VirtualKey::kE},
    {"F", VirtualKey::kF},
    {"G", VirtualKey::kG},
    {"H", VirtualKey::kH},
    {"I", VirtualKey::kI},
    {"J", VirtualKey::kJ},
    {"K", VirtualKey::kK},
    {"L", VirtualKey::kL},
    {"M", VirtualKey::kM},
    {"N", VirtualKey::kN},
    {"O", VirtualKey::kO},
    {"P", VirtualKey::kP},
    {"Q", VirtualKey::kQ},
    {"R", VirtualKey::kR},
    {"S", VirtualKey::kS},
    {"T", VirtualKey::kT},
    {"U", VirtualKey::kU},
    {"V", VirtualKey::kV},
    {"W", VirtualKey::kW},
    {"X", VirtualKey::kX},
    {"Y", VirtualKey::kY},
    {"Z", VirtualKey::kZ},
    // Digits
    {"0", VirtualKey::k0},
    {"1", VirtualKey::k1},
    {"2", VirtualKey::k2},
    {"3", VirtualKey::k3},
    {"4", VirtualKey::k4},
    {"5", VirtualKey::k5},
    {"6", VirtualKey::k6},
    {"7", VirtualKey::k7},
    {"8", VirtualKey::k8},
    {"9", VirtualKey::k9},
    // OEM / special
    {"Backtick", VirtualKey::kOem3},
    {"Minus", VirtualKey::kOemMinus},
    {"Plus", VirtualKey::kOemPlus},
    {"Comma", VirtualKey::kOemComma},
    {"Period", VirtualKey::kOemPeriod},
    {"Semicolon", VirtualKey::kOem1},
    {"Slash", VirtualKey::kOem2},
    {"Backslash", VirtualKey::kOem5},
    {"LBracket", VirtualKey::kOem4},
    {"RBracket", VirtualKey::kOem6},
    {"Quote", VirtualKey::kOem7},
    // Control
    {"Escape", VirtualKey::kEscape},
    {"Return", VirtualKey::kReturn},
    {"Space", VirtualKey::kSpace},
    {"Tab", VirtualKey::kTab},
    {"Backspace", VirtualKey::kBack},
    {"Delete", VirtualKey::kDelete},
    {"Insert", VirtualKey::kInsert},
    {"Home", VirtualKey::kHome},
    {"End", VirtualKey::kEnd},
    {"PageUp", VirtualKey::kPrior},
    {"PageDown", VirtualKey::kNext},
    // Navigation
    {"Left", VirtualKey::kLeft},
    {"Right", VirtualKey::kRight},
    {"Up", VirtualKey::kUp},
    {"Down", VirtualKey::kDown},
    // Modifier
    {"Shift", VirtualKey::kShift},
    {"Control", VirtualKey::kControl},
    {"Alt", VirtualKey::kMenu},
    // Numpad
    {"Numpad0", VirtualKey::kNumpad0},
    {"Numpad1", VirtualKey::kNumpad1},
    {"Numpad2", VirtualKey::kNumpad2},
    {"Numpad3", VirtualKey::kNumpad3},
    {"Numpad4", VirtualKey::kNumpad4},
    {"Numpad5", VirtualKey::kNumpad5},
    {"Numpad6", VirtualKey::kNumpad6},
    {"Numpad7", VirtualKey::kNumpad7},
    {"Numpad8", VirtualKey::kNumpad8},
    {"Numpad9", VirtualKey::kNumpad9},
    {"NumpadEnter", VirtualKey::kReturn},
    {"NumpadPlus", VirtualKey::kAdd},
    {"NumpadMinus", VirtualKey::kSubtract},
    {"NumpadStar", VirtualKey::kMultiply},
    {"NumpadSlash", VirtualKey::kDivide},
    {"PrintScreen", VirtualKey::kSnapshot},
    {"Pause", VirtualKey::kPause},
    {"CapsLock", VirtualKey::kCapital},
    {"NumLock", VirtualKey::kNumLock},
    {"ScrollLock", VirtualKey::kScroll},
    {"NumpadPeriod", VirtualKey::kDecimal},
    // Mouse buttons
    {"Mouse4", static_cast<VirtualKey>(0x05)},
    {"Mouse5", static_cast<VirtualKey>(0x06)},
    {"WheelUp", static_cast<VirtualKey>(0x0E)},
    {"WheelDown", static_cast<VirtualKey>(0x0F)},
    {"LMB", VirtualKey::kLButton},
    {"RMB", VirtualKey::kRButton},
    {"MMB", VirtualKey::kMButton},
};

static const std::unordered_map<std::string_view, VirtualKey>& KeyNames() {
  static const auto* names = [] {
    auto* m = new std::unordered_map<std::string_view, VirtualKey>();
    for (const auto& [name, key] : kKeyTable) {
      m->emplace(name, key);
    }
    return m;
  }();
  return *names;
}

static const std::unordered_map<uint16_t, std::string_view>& KeyTokens() {
  static const auto* tokens = [] {
    auto* m = new std::unordered_map<uint16_t, std::string_view>();
    for (const auto& [name, key] : kKeyTable) {
      m->emplace(static_cast<uint16_t>(key), name);  // the first name wins
    }
    return m;
  }();
  return *tokens;
}

VirtualKey ParseVirtualKey(std::string_view name) {
  auto it = KeyNames().find(name);
  return (it != KeyNames().end()) ? it->second : VirtualKey::kNone;
}

std::string VirtualKeyToString(VirtualKey vk) {
  auto it = KeyTokens().find(static_cast<uint16_t>(vk));
  return it != KeyTokens().end() ? std::string(it->second) : std::string();
}

/* ---- Bind registry ---- */

struct BindEntry {
  std::string name;
  std::string current_key;
  std::function<void()> callback;
};

static std::mutex g_binds_mutex;
static std::deque<BindEntry> g_binds;

void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> callback) {
  std::lock_guard lock(g_binds_mutex);

  /* Store the bind entry (owns the key string that the CVAR references). */
  auto& entry = g_binds.emplace_back();
  entry.name = std::string(name);
  entry.current_key = std::string(default_key);
  entry.callback = std::move(callback);

  /* Capture a pointer to the entry's key string for the CVAR getter/setter.
     The entry is stable because g_binds is never compacted while binds are
     alive (UnregisterBind sets the callback to null rather than erasing). */
  std::string* key_ptr = &entry.current_key;

  rex::cvar::RegisterFlag({
      .name = std::string(name),
      .type = rex::cvar::FlagType::String,
      .category = "Input/Keybinds/System",
      .description = std::string(description),
      .setter = [key_ptr](std::string_view v) -> bool {
        *key_ptr = std::string(v);
        return true;
      },
      .getter = [key_ptr]() -> std::string { return *key_ptr; },
      .lifecycle = rex::cvar::Lifecycle::kHotReload,
      .default_value = std::string(default_key),
  });
}

void UnregisterBind(std::string_view name) {
  std::lock_guard lock(g_binds_mutex);
  for (auto& entry : g_binds) {
    if (entry.name == name) {
      entry.callback = nullptr;
      return;
    }
  }
}

bool ProcessKeyEvent(KeyEvent& e) {
  std::lock_guard lock(g_binds_mutex);
  for (auto& entry : g_binds) {
    if (!entry.callback)
      continue;
    VirtualKey vk = ParseVirtualKey(entry.current_key);
    if (vk != VirtualKey::kNone && e.virtual_key() == vk) {
      entry.callback();
      e.set_handled(true);
      return true;
    }
  }
  return false;
}

/* ---- Input codes (see keybinds.h) ---- */

namespace {

// Keys a VirtualKey can't tell apart, bound by their scancode under these names.
struct ScancodeName {
  SDL_Scancode scancode;
  const char* token;
  const char* display;
};
constexpr ScancodeName kScancodeNames[] = {
    {SDL_SCANCODE_LSHIFT, "LShift", "Left Shift"},
    {SDL_SCANCODE_RSHIFT, "RShift", "Right Shift"},
    {SDL_SCANCODE_LCTRL, "LControl", "Left Ctrl"},
    {SDL_SCANCODE_RCTRL, "RControl", "Right Ctrl"},
    {SDL_SCANCODE_LALT, "LAlt", "Left Alt"},
    {SDL_SCANCODE_RALT, "RAlt", "Right Alt (AltGr)"},
    {SDL_SCANCODE_KP_ENTER, "NumpadEnter", "Numpad Enter"},
    {SDL_SCANCODE_LGUI, "LWin", "Left Windows"},
    {SDL_SCANCODE_RGUI, "RWin", "Right Windows"},
    {SDL_SCANCODE_APPLICATION, "Apps", "Menu key"},
    {SDL_SCANCODE_NONUSBACKSLASH, "Oem102", "\\ (next to Left Shift)"},
};

// Readable names for VirtualKey tokens whose token isn't readable already.
constexpr std::pair<VirtualKey, const char*> kVkDisplay[] = {
    {VirtualKey::kShift, "Shift"},
    {VirtualKey::kControl, "Ctrl"},
    {VirtualKey::kMenu, "Alt"},
    {VirtualKey::kReturn, "Enter"},
    {VirtualKey::kEscape, "Esc"},
    {VirtualKey::kBack, "Backspace"},
    {VirtualKey::kPrior, "Page Up"},
    {VirtualKey::kNext, "Page Down"},
    {VirtualKey::kLeft, "Left arrow"},
    {VirtualKey::kRight, "Right arrow"},
    {VirtualKey::kUp, "Up arrow"},
    {VirtualKey::kDown, "Down arrow"},
    {VirtualKey::kOem3, "` (backtick)"},
    {VirtualKey::kOemMinus, "-"},
    {VirtualKey::kOemPlus, "="},
    {VirtualKey::kOemComma, ","},
    {VirtualKey::kOemPeriod, "."},
    {VirtualKey::kOem1, ";"},
    {VirtualKey::kOem2, "/"},
    {VirtualKey::kOem5, "\\"},
    {VirtualKey::kOem4, "["},
    {VirtualKey::kOem6, "]"},
    {VirtualKey::kOem7, "' (quote)"},
    {VirtualKey::kAdd, "Numpad +"},
    {VirtualKey::kSubtract, "Numpad -"},
    {VirtualKey::kMultiply, "Numpad *"},
    {VirtualKey::kDivide, "Numpad /"},
    {VirtualKey::kDecimal, "Numpad ."},
    {VirtualKey::kSnapshot, "Print Screen"},
    {VirtualKey::kCapital, "Caps Lock"},
    {VirtualKey::kNumLock, "Num Lock"},
    {VirtualKey::kScroll, "Scroll Lock"},
    {VirtualKey::kLButton, "Left mouse button"},
    {VirtualKey::kRButton, "Right mouse button"},
    {VirtualKey::kMButton, "Middle mouse button"},
    {static_cast<VirtualKey>(0x05), "Mouse 4 (back)"},
    {static_cast<VirtualKey>(0x06), "Mouse 5 (forward)"},
    {static_cast<VirtualKey>(kInputCodeWheelUp), "Wheel up"},
    {static_cast<VirtualKey>(kInputCodeWheelDown), "Wheel down"},
};

bool ParseNumber(std::string_view s, int& out) {
  if (s.empty()) {
    return false;
  }
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

}  // namespace

uint16_t InputCodeForMouseButton(int button) {
  switch (button) {
    case 1:
      return static_cast<uint16_t>(VirtualKey::kLButton);
    case 2:
      return static_cast<uint16_t>(VirtualKey::kRButton);
    case 3:
      return static_cast<uint16_t>(VirtualKey::kMButton);
    case 4:
      return 0x05;
    case 5:
      return 0x06;
    default:
      return (button >= 6 && button <= 32) ? uint16_t(kInputCodeMouseBase + button) : 0;
  }
}

uint16_t InputCodeForScancode(int sdl_scancode) {
  if (sdl_scancode <= 0 || sdl_scancode >= int(kInputCodeCount - kInputCodeScancodeBase)) {
    return 0;
  }
  for (const auto& n : kScancodeNames) {
    if (n.scancode == sdl_scancode) {
      return uint16_t(kInputCodeScancodeBase + sdl_scancode);
    }
  }
  const VirtualKey vk = TranslateSDLScancode(static_cast<SDL_Scancode>(sdl_scancode));
  if (vk != VirtualKey::kNone && !VirtualKeyToString(vk).empty()) {
    return static_cast<uint16_t>(vk);
  }
  return uint16_t(kInputCodeScancodeBase + sdl_scancode);
}

uint16_t ParseInputCode(std::string_view token) {
  if (token.empty()) {
    return 0;
  }
  for (const auto& n : kScancodeNames) {
    if (token == n.token) {
      return uint16_t(kInputCodeScancodeBase + n.scancode);
    }
  }
  if (token == "WheelLeft") {
    return kInputCodeWheelLeft;
  }
  if (token == "WheelRight") {
    return kInputCodeWheelRight;
  }
  int n = 0;
  if (token.size() > 5 && token.substr(0, 5) == "Mouse" && ParseNumber(token.substr(5), n)) {
    return InputCodeForMouseButton(n);
  }
  if (token.size() > 8 && token.substr(0, 8) == "Scancode" && ParseNumber(token.substr(8), n)) {
    return (n > 0 && n < int(kInputCodeCount - kInputCodeScancodeBase)) ? uint16_t(kInputCodeScancodeBase + n)
                                                                        : uint16_t(0);
  }
  const VirtualKey vk = ParseVirtualKey(token);
  return vk == VirtualKey::kNone ? uint16_t(0) : static_cast<uint16_t>(vk);
}

std::string InputCodeToken(uint16_t code) {
  if (code == 0 || code >= kInputCodeCount) {
    return {};
  }
  if (code >= kInputCodeScancodeBase) {
    const int sc = code - kInputCodeScancodeBase;
    for (const auto& n : kScancodeNames) {
      if (n.scancode == sc) {
        return n.token;
      }
    }
    return "Scancode" + std::to_string(sc);
  }
  if (code == kInputCodeWheelLeft) {
    return "WheelLeft";
  }
  if (code == kInputCodeWheelRight) {
    return "WheelRight";
  }
  if (code > kInputCodeMouseBase && code <= kInputCodeMouseBase + 32) {
    return "Mouse" + std::to_string(code - kInputCodeMouseBase);
  }
  return VirtualKeyToString(static_cast<VirtualKey>(code));
}

std::string InputCodeDisplayName(uint16_t code) {
  if (code == 0 || code >= kInputCodeCount) {
    return {};
  }
  if (code >= kInputCodeScancodeBase) {
    const int sc = code - kInputCodeScancodeBase;
    for (const auto& n : kScancodeNames) {
      if (n.scancode == sc) {
        return n.display;
      }
    }
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc));
    return (name && *name) ? std::string(name) : "Key " + std::to_string(sc);
  }
  if (code == kInputCodeWheelLeft) {
    return "Wheel left (tilt)";
  }
  if (code == kInputCodeWheelRight) {
    return "Wheel right (tilt)";
  }
  if (code > kInputCodeMouseBase && code <= kInputCodeMouseBase + 32) {
    return "Mouse " + std::to_string(code - kInputCodeMouseBase);
  }
  for (const auto& [vk, name] : kVkDisplay) {
    if (static_cast<uint16_t>(vk) == code) {
      return name;
    }
  }
  const VirtualKey vk = static_cast<VirtualKey>(code);
  if (vk >= VirtualKey::kNumpad0 && vk <= VirtualKey::kNumpad9) {
    return "Numpad " + std::to_string(code - static_cast<uint16_t>(VirtualKey::kNumpad0));
  }
  std::string token = VirtualKeyToString(vk);
  return token.empty() ? "Key " + std::to_string(code) : token;
}

}  // namespace rex::ui
