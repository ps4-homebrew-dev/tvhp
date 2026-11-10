// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI's icons, as in jtplay. They are glyphs of the Lucide icon font
// (fonts/lucide.ttf: the icons below, cut from lucide-static 0.460.0, ISC
// licence), which is loaded as a fallback face: an icon is a character
// like any other, drawn in the colour and size of the text around it, and
// can sit inside a string. Lucide puts its glyphs in the Private Use Area,
// which no text font has, so they never clash with what the text says.
//
// To add one: look its code up in lucide-static's font/info.json and cut
// the font again with it (fontTools' subsetter, all the codes below plus
// the new one).
#ifndef ICONS_H
#define ICONS_H

namespace icon {

inline constexpr const char* kCheck = "";         // check
inline constexpr const char* kChevron = "";       // chevron-right
inline constexpr const char* kResume = "";        // circle-play
inline constexpr const char* kStop = "";          // circle-stop
inline constexpr const char* kForward = "";       // fast-forward
inline constexpr const char* kFilm = "";          // film
inline constexpr const char* kSleep = "";         // moon
inline constexpr const char* kPause = "";         // pause
inline constexpr const char* kPlay = "";          // play
inline constexpr const char* kRewind = "";        // rewind
inline constexpr const char* kServer = "";        // server
inline constexpr const char* kSettings = "";      // settings
inline constexpr const char* kPrevious = "";      // skip-back
inline constexpr const char* kNext = "";          // skip-forward
inline constexpr const char* kWarning = "";       // triangle-alert
inline constexpr const char* kTv = "";            // tv
inline constexpr const char* kAudio = "";         // volume-2
inline constexpr const char* kRecord = "";        // circle-dot
inline constexpr const char* kScheduled = "";     // calendar-clock
inline constexpr const char* kDelete = "";        // trash-2
inline constexpr const char* kClock = "";         // clock
inline constexpr const char* kSubtitles = "";     // captions
inline constexpr const char* kUnschedule = "";    // calendar-x
inline constexpr const char* kClose = "";         // x
inline constexpr const char* kAdd = "";           // plus
inline constexpr const char* kEdit = "";          // pencil
inline constexpr const char* kOffline = "";       // wifi-off
inline constexpr const char* kDone = "";          // circle-check
inline constexpr const char* kNowPlaying = "";    // monitor-play
inline constexpr const char* kGuide = "";         // list-video

} // namespace icon

#endif
