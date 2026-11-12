#include <algorithm>
#include <cstdio>
#include <ctime>

#include "format.h"
#include "clock.h"

namespace textfmt {

std::string FmtHM(int64_t t) {
  char buf[16] = {};
  time_t tt = (time_t)t;
  struct tm tm;
  localtime_r(&tt, &tm);
  std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

std::string FmtRange(int64_t start, int64_t stop) {
  return FmtHM(start) + " – " + FmtHM(stop);
}

namespace {

// strftime's day of the month is padded ("%e") or zero-filled ("%d").
std::string Date(const struct tm& tm, const char* weekday, const char* month) {
  char day[16] = {}, mon[16] = {};
  std::strftime(day, sizeof(day), weekday, &tm);
  std::strftime(mon, sizeof(mon), month, &tm);
  return std::string(day) + " " + std::to_string(tm.tm_mday) + " " + mon;
}

} // namespace

std::string FmtDay(int64_t t) {
  const time_t now = (time_t)tvclock::Now();
  time_t tt = (time_t)t;
  struct tm tm_now, tm_ev;
  localtime_r(&now, &tm_now);
  localtime_r(&tt, &tm_ev);

  const int day_now = tm_now.tm_yday + tm_now.tm_year * 366;
  const int day_ev = tm_ev.tm_yday + tm_ev.tm_year * 366;
  if (day_ev == day_now)
    return "Today";
  if (day_ev == day_now + 1)
    return "Tomorrow";
  if (day_ev == day_now - 1)
    return "Yesterday";
  return Date(tm_ev, "%a", "%b");
}

std::string FmtLongDate(int64_t t) {
  time_t tt = (time_t)t;
  struct tm tm;
  localtime_r(&tt, &tm);
  return Date(tm, "%A", "%B");
}

std::string FmtDuration(int64_t start, int64_t stop) {
  const int64_t mins = std::max<int64_t>(0, (stop - start) / 60);
  if (mins < 60)
    return std::to_string(mins) + " min";
  std::string out = std::to_string(mins / 60) + " h";
  if (mins % 60)
    out += " " + std::to_string(mins % 60) + " min";
  return out;
}

std::string FmtPosition(int64_t us) {
  if (us < 0)
    return "--:--";
  const int64_t total = us / 1000000;
  const int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
  char buf[24] = {};
  if (h > 0)
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
  else
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)m, (long long)s);
  return buf;
}

std::string FmtEpisode(uint32_t season, uint32_t episode) {
  std::string out;
  if (season)
    out += "S" + std::to_string(season);
  if (episode)
    out += "E" + std::to_string(episode);
  return out;
}

std::string Synopsis(const std::string& title, const std::string& subtitle,
                     const std::string& description, const std::string& summary) {
  std::string out;
  if (!subtitle.empty() && subtitle != title)
    out = subtitle + "\n";
  out += !description.empty() ? description : summary;
  return out;
}

std::string DvrStateLabel(const HtspDvrEntry& e) {
  if (e.IsRecording())
    return "Recording now";
  if (e.IsScheduled())
    return e.enabled ? "Scheduled" : "Disabled";
  if (e.IsFinished())
    return FmtDuration(e.start, e.stop);
  if (e.state == "missed")
    return "Missed";
  if (!e.error.empty())
    return e.error;
  return e.state.empty() ? "Unknown" : e.state;
}

} // namespace textfmt
