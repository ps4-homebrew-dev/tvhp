// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game controller navigation. Controller input is turned into the same
// RmlUi key presses the keyboard and remote controls produce, so every view
// handles a pad exactly as it handles the keys it already knows (the keys
// are given their meaning in keymap.h):
//
//   d-pad, left stick     arrows (held: repeats)
//   cross / A             Return       select, record, play/pause
//   circle / B            Escape       back, cancel, stop
//   square / X            Tab          guide <-> recordings, info bar
//   triangle / Y          Delete       delete a recording or server
//   L1 / R1               PageUp/Down  page, zap (repeats)
//   L2 / R2               Home / End   first / last row
//   options / start       Apps         add a server; playback menu
//   back / select         F9           jump to the guide (not on a PS4:
//                                      the system keeps SHARE)
//
// The PS4's SDL port turns the DualShock 4 into an SDL game controller, so
// the same code serves the console and any pad SDL knows on a PC.
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <SDL.h>

namespace Rml {
class Context;
}

namespace gamepad {

// Opens the game controller subsystem and the pads already connected.
// Call after SDL_Init(). Failure only means there will be no pads.
void Initialize();
void Shutdown();

// Feeds one SDL event. Returns true if it was a controller event, which
// then needs no further handling.
bool HandleEvent(Rml::Context* context, const SDL_Event& event);

// Once per frame, after the events: repeats the held direction, and turns
// an OK held long enough into Options.
void Update(Rml::Context* context);

// The long press of OK (Return or keypad Enter: a remote's OK arrives as
// Return), for the remotes that have no options key or whose TV does not
// pass it on over HDMI-CEC. Its press is held back until it is clear which
// it is: let go within kOkHoldMs, it was OK after all; held that long,
// Update() sends Options (Apps, see keymap.h) and the press and its release
// go nowhere. Any other press while OK is held back also makes it OK,
// delivered first. (From jtplay.)
//
// Feed every event before handling it. What to do with it:
enum class Hold {
  kPass,    // handle it as usual
  kSwallow, // drop it
  kReplay,  // handle *press (the held-back OK) first, then it as usual
};
Hold HoldOk(const SDL_Event& event, SDL_Event* press);

} // namespace gamepad

#endif
