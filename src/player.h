// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback through libmpv, which draws the video into the window's
// backbuffer under the UI. Everything comes over the one HTSP connection,
// which mpv reads through two stream protocols of ours:
//
//   tvhp-live://<n>   live TV: the subscription's packets put back into an
//                     MPEG transport stream (TsMux)
//   tvhp-dvr://<id>   a recording, with fileRead; at the end of one still
//                     running, the next data is waited for
//
// Tracks, settings and transport follow jtplay's player.
#ifndef PLAYER_H
#define PLAYER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "htsp.h"

struct mpv_handle;
struct mpv_render_context;
struct mpv_node;
class TsMux;

// A track of what plays.
struct TrackInfo {
  int id = -1;       // mpv track id, stable while the stream is open
  std::string label; // "eng · Dolby Digital · 5.1", "1080p · H.264"
  std::string lang;  // ISO 639 as the stream says it, "" when it doesn't
};

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

  // With the window's GL context current. 'fonts_dir' has the subtitles'
  // font (the PS4 has no fontconfig).
  bool Initialize(std::string& error, const std::string& fonts_dir = std::string());
  // Main thread, GL context still current.
  void Shutdown();

  // Returns once subscribed; the picture follows.
  bool PlayChannel(HtspClient& client, uint32_t channel_id, std::string& error);

  bool PlayRecording(HtspClient& client, uint32_t dvr_id, int64_t start_seconds,
                     std::string& error);

  void Stop(HtspClient& client);

  bool IsPlaying() const { return active_; }
  bool IsRecordingPlayback() const { return source_ == Source::Recording; }
  bool IsLoaded() const { return loaded_; }
  // A recording played to its end (a running one never gets there).
  bool AtEnd() const { return active_ && source_ == Source::Recording && eof_; }

  // Every frame, before the UI: the video into framebuffer 0.
  void RenderVideo(int width, int height);

  // "Tuning...", the server's last word on the stream, why playback failed;
  // "" while all is well.
  std::string StatusText() const;

  // --- Transport (recordings) ---------------------------------------------
  bool CanSeek() const { return source_ == Source::Recording && seekable_; }
  // Position and duration in microseconds; -1 when unknown.
  int64_t PositionUs() const { return position_us_.load(); }
  int64_t DurationUs() const { return duration_us_.load(); }
  // A running recording's duration grows: no basis for "ends at".
  bool DurationReliable() const { return !duration_unreliable_.load(); }
  // Stops a second short of the end.
  void SeekRelative(int64_t delta_us);
  void SetPaused(bool paused);
  bool TogglePause(); // the new state
  bool IsPaused() const { return paused_; }
  // 1.0 = normal; back to that for every stream.
  void SetSpeed(double speed);
  double Speed() const { return speed_.load(); }

  // --- Adjustments ---------------------------------------------------------
  // For the room, so they stay from stream to stream; except the subtitle
  // delay, which is one recording's.
  enum class Fit { Fit, Fill, Stretch, Count };
  void SetFit(Fit fit);
  Fit GetFit() const { return fit_; }
  // Seconds; positive = later.
  void SetAudioDelay(double seconds);
  double AudioDelay() const { return audio_delay_; }
  void SetSubtitleDelay(double seconds);
  double SubtitleDelay() const { return sub_delay_; }
  // 1.0 = normal.
  void SetSubtitleScale(double scale);
  double SubtitleScale() const { return sub_scale_; }
  // mpv's alang/slang ("swe,eng"; "" = the stream's choice), from the next
  // stream on. Subtitles in a preferred language come on by themselves.
  void SetPreferredLanguages(const std::string& audio, const std::string& subs);

  // --- Tracks ---------------------------------------------------------------
  // "" when there is none.
  std::string CurrentVideoLabel() const;

  std::vector<TrackInfo> AudioTracks() const;
  int CurrentAudioStream() const { return audio_id_.load(); }
  std::string CurrentAudioLabel() const;
  bool SelectAudioTrack(int id);

  std::vector<TrackInfo> SubtitleTracks() const;
  // -1: off.
  int CurrentSubtitleStream() const { return sub_id_.load(); }
  bool SelectSubtitleTrack(int id);

private:
  enum class Source { None, Live, Recording };

  // On mpv's demuxer thread.
  static int OpenLive(void* user_data, char* uri, struct mpv_stream_cb_info* info);
  static int OpenRecording(void* user_data, char* uri, struct mpv_stream_cb_info* info);

  void EventThread();
  void UpdateTracks(const mpv_node* list);
  void LoadFile(const std::string& url, const char* options);
  void SetStatus(const std::string& text);
  void ResetFileState();
  // Before a stream: undoes what the last one left set.
  void ResetPerStream(int64_t start_seconds);
  void SetDouble(const char* name, double value);

  mpv_handle* mpv_ = nullptr;
  mpv_render_context* render_ = nullptr;
  std::thread event_thread_;
  std::atomic<bool> quitting_{false};

  std::atomic<bool> active_{false};
  std::atomic<Source> source_{Source::None};

  // --- live ---------------------------------------------------------------
  // The transport stream fed now, and its <n>; a new one for every
  // subscriptionStart.
  std::mutex live_mutex_;
  std::shared_ptr<TsMux> live_;
  uint64_t live_serial_ = 0;
  // Bumped by PlayChannel()/Stop(): an older subscription's callbacks
  // still in flight are ignored.
  std::atomic<uint64_t> session_{0};

  // --- recordings ---------------------------------------------------------
  // For OpenRecording(), set before the loadfile.
  std::atomic<HtspClient*> dvr_client_{nullptr};
  std::atomic<uint32_t> dvr_id_{0};

  // --- the open stream (event thread) -------------------------------------
  std::atomic<bool> loaded_{false};
  std::atomic<bool> eof_{false};
  std::atomic<bool> seekable_{false};
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> position_us_{-1};
  std::atomic<int64_t> duration_us_{-1};
  std::atomic<bool> duration_unreliable_{false};
  std::atomic<double> speed_{1.0};
  std::atomic<int> video_id_{-1};
  std::atomic<int> audio_id_{-1};
  std::atomic<int> sub_id_{-1};

  mutable std::mutex tracks_mutex_;
  std::vector<TrackInfo> video_tracks_;
  std::vector<TrackInfo> audio_tracks_;
  std::vector<TrackInfo> sub_tracks_;

  // The last mpv message, not printed again while it repeats.
  std::string log_repeat_text_;
  int log_repeats_ = 0;

  // Adjustments (main thread).
  Fit fit_ = Fit::Fit;
  double audio_delay_ = 0.0;
  double sub_delay_ = 0.0;
  double sub_scale_ = 1.0;

  mutable std::mutex status_mutex_;
  std::string status_;
};

#endif
