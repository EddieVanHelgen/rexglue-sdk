/**
 * @file        rex/ui/keybinds.h
 * @brief       Key binding registry with cvar-backed rebindable keys.
 *
 * Provides a global bind registry where components (like overlay dialogs)
 * can register named keybinds with default keys and callbacks. Binds are
 * backed by string CVARs in the "Keybinds" category, so they appear in
 * the settings overlay and can be saved to config files.
 *
 * @section keybinds_usage Usage
 *
 * @code
 * // Register a bind (typically in a constructor):
 * rex::ui::RegisterBind("bind_console", "Backtick",
 *                       "Toggle console overlay", [this]{ ToggleVisible(); });
 *
 * // Dispatch key events (typically in OnKeyDown):
 * rex::ui::ProcessKeyEvent(e);
 *
 * // Unregister (typically in a destructor):
 * rex::ui::UnregisterBind("bind_console");
 * @endcode
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <functional>
#include <string>
#include <string_view>

namespace rex::ui {

/**
 * Parse a human-readable key name to a VirtualKey enum value.
 * @param name  Key name string (e.g. "F3", "Backtick", "A", "Escape").
 * @return      Matching VirtualKey, or VirtualKey::kNone if unrecognized.
 */
VirtualKey ParseVirtualKey(std::string_view name);

/**
 * Convert a VirtualKey to its human-readable string name.
 * @param vk  VirtualKey to convert.
 * @return    Key name string (e.g. "F3", "LMB"), or empty if unrecognized.
 */
std::string VirtualKeyToString(VirtualKey vk);

/**
 * Register a named keybind with a default key and callback.
 *
 * Creates a string CVAR named @p name in the "Keybinds" category so the
 * binding is visible in the settings overlay and persisted to config.
 *
 * @param name         CVAR name for this bind (e.g. "bind_console").
 * @param default_key  Default key name (e.g. "Backtick", "F3").
 * @param description  Human-readable description for the settings UI.
 * @param callback     Function to invoke when the bound key is pressed.
 */
void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> callback);

/**
 * Remove a previously registered keybind.
 * @param name  The CVAR name used when registering the bind.
 */
void UnregisterBind(std::string_view name);

/**
 * Process a key-down event against all registered binds.
 *
 * Looks up each bind's current key from its CVAR, parses it, and compares
 * against the event's virtual key. If a match is found, the bind's callback
 * is invoked and the event is marked as handled.
 *
 * @param e  The key event to process.
 * @return   True if a bind matched and the event was consumed.
 */
bool ProcessKeyEvent(KeyEvent& e);

/* ---- Input codes: every bindable key, mouse button and wheel direction ----
 *
 * One number per physical input, used by the keyboard / mouse pad binds
 * (keybind_*) and by tools that write them (a settings window). Bind tokens
 * name a code; InputCodeToken / ParseInputCode round-trip exactly.
 *
 *   0x001-0x0FF  a Windows-style VirtualKey (keys and the classic mouse buttons:
 *                LMB 0x01, RMB 0x02, MMB 0x04, Mouse4 0x05, Mouse5 0x06) plus
 *                the wheel's pseudo keys WheelUp 0x0E / WheelDown 0x0F;
 *   0x106-0x120  mouse buttons 6-32 ("Mouse6".."Mouse32"), from Raw Input;
 *   0x121/0x122  wheel tilt left / right ("WheelLeft", "WheelRight");
 *   0x200-0x3FF  0x200 + SDL scancode, for keys a VirtualKey can't tell apart
 *                (left / right Shift, Ctrl, Alt; Numpad Enter) or doesn't
 *                have at all ("Scancode<N>"). */
constexpr uint16_t kInputCodeCount = 0x400;
constexpr uint16_t kInputCodeMouseBase = 0x100;  // + button number (6..32)
constexpr uint16_t kInputCodeWheelUp = 0x0E;
constexpr uint16_t kInputCodeWheelDown = 0x0F;
constexpr uint16_t kInputCodeWheelLeft = 0x121;
constexpr uint16_t kInputCodeWheelRight = 0x122;
constexpr uint16_t kInputCodeScancodeBase = 0x200;

/// The code a bind token names (modifier prefixes already removed), or 0.
uint16_t ParseInputCode(std::string_view token);
/// The token a bind is written with ("R", "LControl", "Mouse4", "WheelLeft").
std::string InputCodeToken(uint16_t code);
/// A readable name ("Left Ctrl", "Mouse 4 (back)", "Wheel up").
std::string InputCodeDisplayName(uint16_t code);
/// Mouse button 1-32 (1 left, 2 right, 3 middle, 4 back, 5 forward) -> code.
uint16_t InputCodeForMouseButton(int button);
/// The code to bind a key by, from its SDL scancode: its VirtualKey when that
/// names it uniquely, else 0x200 + scancode.
uint16_t InputCodeForScancode(int sdl_scancode);
/// The SDL scancode SDL's Windows backend gives a WM_KEYDOWN / WM_KEYUP with
/// these parameters (0 = none). Windows only; 0 elsewhere.
int Win32KeyMessageToScancode(uintptr_t wparam, intptr_t lparam);

}  // namespace rex::ui
