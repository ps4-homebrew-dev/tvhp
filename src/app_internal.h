#ifndef APP_INTERNAL_H
#define APP_INTERNAL_H

#include <cstddef>
#include <cstdint>

#include <RmlUi/Core.h>

// What the app_*.cpp files share.
namespace appdetail {

// Row height + margin, as in the stylesheets: scrolling goes by them.
inline constexpr float kChannelRowPitch = 96.0f + 12.0f;
inline constexpr float kEpgRowPitch = 76.0f + 10.0f;
inline constexpr float kRecRowPitch = 104.0f + 12.0f;
inline constexpr float kServerRowPitch = 96.0f + 12.0f;
inline constexpr float kMenuRowPitch = 72.0f + 6.0f;

inline constexpr size_t kMaxEpgRows = 80;
inline constexpr double kRefreshIntervalSec = 20.0; // for the progress bars
inline constexpr double kToastSec = 4.0;
inline constexpr double kWatchInfoSec = 5.0;        // the info bar

inline constexpr int64_t kSeekLargeUs = 300 * 1000000LL;

// Less than this in is no position to resume; this far is watched (the
// rest is mostly padding).
inline constexpr int64_t kMinResumeSec = 30;
inline constexpr double kWatchedFraction = 0.92;
// How often a recording's position is sent while it plays.
inline constexpr double kPositionSaveSec = 60.0;

// Connecting by itself: how long before each try; the last repeats.
inline constexpr double kReconnectDelaysSec[] = {2.0, 5.0, 10.0, 30.0};

// 'delta' rows on from 'current' of 'count' (> 0): a single step wraps
// round, bigger ones stop at the end.
inline int StepIndex(int current, int delta, int count)
{
  if (delta == 1 || delta == -1)
    return ((current + delta) % count + count) % count;
  return current + delta < 0 ? 0 : current + delta > count - 1 ? count - 1 : current + delta;
}

// Seconds since startup.
inline double Now()
{
  Rml::SystemInterface* sys = Rml::GetSystemInterface();
  return sys ? sys->GetElapsedTime() : 0.0;
}

} // namespace appdetail

#endif
