// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live TV for mpv. HTSP delivers a live channel as elementary-stream
// packets (muxpkt), not as a container, while mpv plays byte streams. A
// TsMux puts the packets back into an MPEG transport stream with
// libavformat's muxer - the container they were broadcast in - and hands
// the bytes to mpv through a stream callback (see Player).
//
// Threads:
//   HTSP reader  Start(), Write(), Finish()
//   mpv demuxer  Read(), Cancel()
//   any          Close()
#ifndef TSMUX_H
#define TSMUX_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "htsp.h"

struct AVFormatContext;
struct AVIOContext;

class TsMux {
public:
  ~TsMux();

  // Sets the muxer up for the streams of a subscriptionStart and writes the
  // stream header. Video and audio streams that MPEG-TS can carry are
  // kept; subtitles, teletext and anything else are left out. Returns a
  // description like "H264 1280x720 + AC3", empty when nothing is playable.
  std::string Start(const std::vector<HtspStreamInfo>& streams);

  // Muxes one packet. Packets of streams that were left out are dropped.
  void Write(const HtspMuxPacket& pkt);

  // The subscription is over: readers get what is buffered, then the end.
  void Finish();

  // Blocks until there are bytes, the stream is over, or the read is
  // cancelled. Returns the number of bytes copied, 0 at the end.
  int64_t Read(char* buf, uint64_t size);
  // Wakes a blocked Read(), which then returns 0.
  void Cancel();
  // Finish() and Cancel() at once: nothing is read any more.
  void Close();

private:
  // libavformat's write callback (its buffer became const in FFmpeg 7).
  static int WritePacket(void* opaque, const uint8_t* buf, int size);
  static int WritePacketMutable(void* opaque, uint8_t* buf, int size);
  void Push(const uint8_t* buf, int size);
  void FreeMuxer();

  // --- muxer (HTSP reader thread) -----------------------------------------
  AVFormatContext* fmt_ = nullptr;
  AVIOContext* avio_ = nullptr;
  std::map<uint32_t, int> streams_; // HTSP stream index -> muxer stream
  // HTSP timestamps are rebased so the transport stream starts near zero
  // and doesn't wrap its 33-bit clock in the middle of a programme.
  int64_t base_us_ = INT64_MIN;
  std::vector<int64_t> last_dts_; // per muxer stream, to drop regressions

  // --- bytes for mpv ------------------------------------------------------
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> chunks_;
  size_t chunk_offset_ = 0; // into chunks_.front()
  size_t buffered_ = 0;
  bool finished_ = false;
  bool cancelled_ = false;
  bool dropping_ = false;
};

#endif
