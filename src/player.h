// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback through libmpv. mpv demuxes, decodes and plays the audio itself;
// the video is drawn with mpv's OpenGL renderer straight into the window's
// backbuffer, under the RmlUi document (see Backend::BeginFrame()).
//
// Everything still comes over the one HTSP connection; mpv reads it through
// two stream protocols of ours (mpv_stream_cb_add_ro):
//
//   Live TV      subscribe() -> subscriptionStart and muxpkt. The packets
//                are put back into an MPEG transport stream (TsMux), which
//                mpv reads as "tvhp-live://<n>".
//
//   Recordings   fileOpen("/dvrfile/<id>") + fileRead/fileStat, read by mpv
//                as "tvhp-dvr://<id>" with seeking. A recording that is
//                still running is waited for at its end rather than ended.
#ifndef PLAYER_H
#define PLAYER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "htsp.h"

struct mpv_handle;
struct mpv_render_context;
struct mpv_node;
class TsMux;

// Threads:
//   mpv's own    demuxing (and with it our stream callbacks), decoding,
//                audio output
//   event thread mpv_wait_event(): file and property changes into the
//                members below
//   HTSP reader  the live stream callbacks: TsMux
//   main thread  everything public
class Player {
public:
  Player();
  ~Player();

  // Main thread, with the window's GL context current: creates the mpv core
  // and its GL renderer. Without it nothing plays, and Play*() say so.
  bool Initialize(std::string& error);
  // Main thread, GL context still current.
  void Shutdown();

  // Starts live playback of a channel, replacing whatever is playing.
  // Returns once subscribed; the picture follows when the server starts
  // the stream.
  bool PlayChannel(HtspClient& client, uint32_t channel_id, std::string& error);

  // Starts playback of a recording over the HTSP file API, replacing
  // whatever is playing. Works for finished and in-progress recordings.
  bool PlayRecording(HtspClient& client, uint32_t dvr_id, std::string& error);

  // Stops playback, unsubscribing or closing the file as appropriate.
  void Stop(HtspClient& client);

  bool IsPlaying() const { return active_; }
  // True while playing a recording rather than live TV.
  bool IsRecordingPlayback() const { return source_ == Source::Recording; }

  // Called every frame after the backbuffer has been cleared and before the
  // UI is rendered; draws the current video frame into framebuffer 0. The
  // document body is transparent while playing, so the UI composites on top.
  void RenderVideo(int width, int height);

  // Short human-readable status ("h264 1280x720 + ac3", "Tuning...", the
  // last subscriptionStatus, ...) for display in the UI. Empty = nothing.
  std::string StatusText() const;

  // --- Transport, recordings only ----------------------------------------
  bool CanSeek() const { return source_ == Source::Recording && seekable_; }
  // Position and duration in microseconds; -1 when unknown.
  int64_t PositionUs() const { return position_us_.load(); }
  int64_t DurationUs() const { return duration_us_.load(); }
  // Seeks by a relative amount; clamped to the file. No-op for live TV.
  void SeekRelative(int64_t delta_us);
  void SetPaused(bool paused);
  // Returns the new paused state.
  bool TogglePause();
  bool IsPaused() const { return paused_; }

private:
  enum class Source { None, Live, Recording };

  // The stream protocols, called by mpv on its demuxer thread.
  static int OpenLive(void* user_data, char* uri, struct mpv_stream_cb_info* info);
  static int OpenRecording(void* user_data, char* uri, struct mpv_stream_cb_info* info);

  void EventThread();
  void UpdateTracks(const mpv_node* list);
  void LoadFile(const std::string& url, const char* options);
  void SetStatus(const std::string& text);
  void ResetFileState();

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  std::thread event_thread_;
  std::atomic<bool> quitting_{false};

  std::atomic<bool> active_{false};
  std::atomic<Source> source_{Source::None};

  // --- live ---------------------------------------------------------------
  // The transport stream being fed, and its number in "tvhp-live://<n>".
  // A new one is made for every subscriptionStart.
  std::mutex live_mutex_;
  std::shared_ptr<TsMux> live_;
  uint64_t live_serial_ = 0;
  // Bumped on every PlayChannel()/Stop(), so callbacks of an older
  // subscription that are still in flight are ignored.
  std::atomic<uint64_t> session_{0};

  // --- recordings ---------------------------------------------------------
  // Read by OpenRecording(); set before the loadfile that leads to it.
  std::atomic<HtspClient*> dvr_client_{nullptr};
  std::atomic<uint32_t> dvr_id_{0};

  // --- the open file (event thread) ---------------------------------------
  std::atomic<bool> seekable_{false};
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> position_us_{-1};
  std::atomic<int64_t> duration_us_{-1};

  mutable std::mutex status_mutex_;
  std::string status_;
};

#endif
