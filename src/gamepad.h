// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game controllers, as the keys a keyboard or remote sends (keymap.h gives
// them their meaning):
//
//   d-pad, left stick     arrows       (held: repeats)
//   cross / A             Return       select, record, play/pause
//   circle / B            Escape       back, cancel
//   square / X            Tab          Guide <-> Recordings; the info bar
//   triangle / Y          Delete       delete
//   L1 / R1               tab keys     Guide / Recordings; watching: back
//                                      and on (channel, 5 minutes)
//   L2 / R2               PageUp/Down  page (repeats)
//   options / start       Apps         the Options menu
//   back / select         F9           the guide (not on a PS4, whose
//                                      system keeps SHARE)
//
// The PS4's SDL makes the DualShock 4 an SDL game controller, so this serves
// the console and a PC alike.
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <functional>
#include <string>

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

// OK held is Options, for remotes without an options key (or whose TV does
// not pass it on). An OK press (Return, keypad Enter) is held back until it
// is clear which it is: let go within kOkHoldMs, or followed by another
// press, it is OK; held, Update() sends Options and the press and its
// release go nowhere.
//
// Feed every event before handling it. What to do with it:
enum class Hold {
  kPass,    // handle it as usual
  kSwallow, // drop it
  kReplay,  // handle *press (the held-back OK) first, then it as usual
};
Hold HoldOk(const SDL_Event& event, SDL_Event* press);

// Input tracing: with a callback set, every input event SDL delivers --
// keys, controller and joystick buttons, hats, axes, also the ones nothing
// reacts to -- is described to it in a line, with 'press' set for a
// button or key going down (or an axis or hat leaving rest). nullptr ends
// it. For finding out what a remote (over HDMI-CEC, say) sends.
void SetTrace(std::function<void(const std::string& line, bool press)> trace);
// Feeds one SDL event to the trace, if there is one. Call for every event,
// before handling it.
void Trace(const SDL_Event& event);

} // namespace gamepad

#endif
