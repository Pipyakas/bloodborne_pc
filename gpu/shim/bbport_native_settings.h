// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's settings shown in the game's own options screen (bbgpu.h
// bbgpu_native_settings; runtime_menu.c builds the rows).
#pragma once

#include "../bbgpu.h"

namespace BbNative {

/// Fills the rows' bytes from the settings (the screen is opening) and returns the rows.
int Rows(int screen, const BbNativeSetting** rows);
/// Applies bytes the game's widgets changed, and saves. Called by the window thread.
void Poll();
/// Applies changed bytes; choice rows only when `choices` (the screen closed or reopened).
void Apply(bool choices);
/// The game widget's "dropdown open" byte of a choice row of the open screen.
void WatchDropdown(const BbNativeSetting* row, const volatile uint8_t* open);
/// The screen closed: its widgets' bytes are no longer read.
void ForgetDropdowns();
/// The button the port presses on this pad read (bbgpu_native_menu_press).
int MenuPress();

} // namespace BbNative
