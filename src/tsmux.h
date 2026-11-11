// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live TV for mpv: HTSP sends a channel's packets, mpv plays bytes. TsMux
// puts the packets back into an MPEG transport stream with libavformat, for
// mpv to read (see Player).
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

  // For a subscriptionStart's streams: video, audio and DVB subtitles (not
  // teletext). Says what they are, "H264 1280x720 + AC3 + DVB subtitles
  // (swe)", or "" when there is nothing to play.
  std::string Start(const std::vector<HtspStreamInfo>& streams);

  void Write(const HtspMuxPacket& pkt);

  // The subscription is over: readers get what is buffered, then the end.
  void Finish();

  // Blocks until there are bytes (their count), or the end (0).
  int64_t Read(char* buf, uint64_t size);
  void Cancel();
  // Finish() and Cancel() at once: nothing is read any more.
  void Close();

private:
  // libavformat's write callback (const since FFmpeg 7).
  static int WritePacket(void* opaque, const uint8_t* buf, int size);
  static int WritePacketMutable(void* opaque, uint8_t* buf, int size);
  void Push(const uint8_t* buf, int size);
  void FreeMuxer();

  // --- muxer (HTSP reader thread) -----------------------------------------
  AVFormatContext* fmt_ = nullptr;
  AVIOContext* avio_ = nullptr;
  std::map<uint32_t, int> streams_; // HTSP stream index -> muxer stream
  // Timestamps start near zero, so the 33-bit clock does not wrap.
  int64_t base_us_ = INT64_MIN;
  std::vector<int64_t> last_dts_; // per stream, to drop going back

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
