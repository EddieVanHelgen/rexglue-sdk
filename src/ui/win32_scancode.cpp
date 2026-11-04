/**
 * @file        ui/win32_scancode.cpp
 * @brief       Win32 key message -> SDL scancode, the way SDL's Windows backend
 *              computes it (WindowsScanCodeToSDLScanCode in SDL 3.4.14), for
 *              windows outside SDL that must name keys as the game will see them.
 */
#include <rex/ui/keybinds.h>

#include <SDL3/SDL_scancode.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rex::ui {

#ifdef _WIN32
namespace {
#include "win32_scancodes.inc"
}  // namespace

int Win32KeyMessageToScancode(uintptr_t wparam, intptr_t lparam) {
  const WORD key_flags = HIWORD(lparam);
  WORD scan = LOBYTE(key_flags);
  // The on-screen keyboard can set the break bit.
  scan &= ~0x80;
  if (scan != 0) {
    if ((key_flags & KF_EXTENDED) == KF_EXTENDED) {
      scan = MAKEWORD(scan, 0xE0);
    } else if (scan == 0x45) {
      scan = 0xE046;  // Pause
    }
  } else {
    // No scan code (multimedia keys, software that sends only a virtual key).
    scan = LOWORD(MapVirtualKeyW(UINT(LOWORD(wparam)), MAPVK_VK_TO_VSC_EX));
    if (scan == 0xE11D) {
      scan = 0xE046;
    }
  }
  const BYTE index = BYTE(LOBYTE(scan) | (HIBYTE(scan) ? 0x80 : 0x00));
  return int(kWin32ScancodeTable[index]);
}
#else
int Win32KeyMessageToScancode(uintptr_t, intptr_t) {
  return 0;
}
#endif

}  // namespace rex::ui
