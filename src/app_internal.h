#ifndef APP_INTERNAL_H
#define APP_INTERNAL_H

#include <cstddef>
#include <cstdint>

#include <RmlUi/Core.h>

// Constants and helpers shared between the App translation units
// (app.cpp, app_connect.cpp, app_guide.cpp, app_recordings.cpp,
// app_watch.cpp). Not part of the public interface of App.
namespace appdetail {

// Row geometry; must match the stylesheets (.channel-row / .epg-row /
// .rec-row / .server-row height + margin-bottom) so keyboard scrolling can be
// computed without layout queries on the generated rows.
inline constexpr float kChannelRowPitch = 96.0f + 12.0f;
inline constexpr float kEpgRowPitch = 76.0f + 10.0f;
inline constexpr float kRecRowPitch = 104.0f + 12.0f;
inline constexpr float kServerRowPitch = 96.0f + 12.0f;
inline constexpr float kMenuRowPitch = 72.0f + 6.0f;

inline constexpr size_t kMaxEpgRows = 80;
inline constexpr double kRefreshIntervalSec = 20.0; // progress bars, clock drift
inline constexpr double kToastSec = 4.0;
inline constexpr double kConfirmWindowSec = 5.0;    // destructive double-press
inline constexpr double kWatchInfoSec = 5.0;        // watch info bar auto-hide

inline constexpr int64_t kSeekLargeUs = 300 * 1000000LL;

// Resume: less than this into a recording is not worth asking about; this
// far through it counts as watched (the end is usually padding).
inline constexpr int64_t kMinResumeSec = 30;
inline constexpr double kWatchedFraction = 0.92;
// How often the position of a recording that plays is sent to the server,
// so a crash or the PS button loses no more than this.
inline constexpr double kPositionSaveSec = 60.0;

// The row 'delta' steps from 'current' in a list of 'count' rows (count > 0).
// A single step past either end wraps round to the other; bigger jumps (the
// shoulder buttons, home and end) stop at the end instead.
inline int StepIndex(int current, int delta, int count)
{
  if (delta == 1 || delta == -1)
    return ((current + delta) % count + count) % count;
  return current + delta < 0 ? 0 : current + delta > count - 1 ? count - 1 : current + delta;
}

// Seconds since startup, from RmlUi's system interface.
inline double Now()
{
  Rml::SystemInterface* sys = Rml::GetSystemInterface();
  return sys ? sys->GetElapsedTime() : 0.0;
}

} // namespace appdetail

#endif
