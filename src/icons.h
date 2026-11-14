// SPDX-License-Identifier: GPL-3.0-or-later
//
// The icons the code uses (main.rml writes its own as &#xE...;). They are
// characters of Lucide's font, in the Private Use Area, which is a fallback
// face: an icon takes the colour and size of the text around it. Each is
// centred in its box, so a tile centres one with line-height = its height.
//
// To add one: find its code in lucide-static's font/info.json, use it, and
// cut fonts/lucide.ttf again with tools/cut-lucide.py.
#ifndef ICONS_H
#define ICONS_H

namespace icon {

inline constexpr const char* kResume = "";        // circle-play
inline constexpr const char* kStop = "";          // circle-stop
inline constexpr const char* kForward = "";       // fast-forward
inline constexpr const char* kFilm = "";          // film
inline constexpr const char* kSleep = "";         // moon
inline constexpr const char* kPause = "";         // pause
inline constexpr const char* kPlay = "";          // play
inline constexpr const char* kRewind = "";        // rewind
inline constexpr const char* kWarning = "";       // triangle-alert
inline constexpr const char* kAudio = "";         // volume-2
inline constexpr const char* kScheduled = "";     // calendar-clock
inline constexpr const char* kDelete = "";        // trash-2
inline constexpr const char* kUnschedule = "";    // calendar-x
inline constexpr const char* kDone = "";          // circle-check
inline constexpr const char* kGamepad = "";       // gamepad-2

} // namespace icon

#endif
