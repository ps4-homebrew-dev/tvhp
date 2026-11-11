#ifndef FORMAT_H
#define FORMAT_H

#include <cstdint>
#include <string>

#include "htsp.h"

// Text for the guide, the recordings and the info bar, in local time.
namespace textfmt {

// Unix seconds -> "20:00" in local time.
std::string FmtHM(int64_t t);

// "20:00 – 21:30".
std::string FmtRange(int64_t start, int64_t stop);

// "Today", "Tomorrow", "Yesterday", or "Wed 5 Jul" further out.
std::string FmtDay(int64_t t);

// "Wednesday 5 July".
std::string FmtLongDate(int64_t t);

// "45 min", "1 h", "1 h 30 min".
std::string FmtDuration(int64_t start, int64_t stop);

// Microseconds -> "1:23:45" or "23:45". Negative input gives "--:--".
std::string FmtPosition(int64_t us);

// "S2E5", "S2", "E5", or "".
std::string FmtEpisode(uint32_t season, uint32_t episode);

// The subtitle (unless it is the title), then the description, or else the
// summary.
std::string Synopsis(const std::string& title, const std::string& subtitle,
                     const std::string& description, const std::string& summary);

// "Recording now", "Scheduled", or the length once recorded.
std::string DvrStateLabel(const HtspDvrEntry& e);

} // namespace textfmt

#endif
