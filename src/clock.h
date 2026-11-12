// SPDX-License-Identifier: GPL-3.0-or-later
//
// The time the app goes by: the server's, not the console's. A console kept
// offline (as a jailbroken PS4 often is) can have its clock hours or days
// off, while the guide is the server's: it sends programmes up to a time
// judged by its own clock, and what is on now is what is on by it.
#ifndef CLOCK_H
#define CLOCK_H

#include <atomic>
#include <cstdint>
#include <ctime>

namespace tvclock {

// Seconds the server's clock is ahead of the console's; set on connecting.
inline std::atomic<int64_t> skew{0};

inline int64_t Now() { return (int64_t)std::time(nullptr) + skew.load(); }

} // namespace tvclock

#endif
