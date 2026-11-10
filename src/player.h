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
//
// Tracks, the picture and sound adjustments and the transport follow
// jtplay's player: the same labels, the same settings, the same rules.
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

// One selectable elementary stream of what plays.
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

  // Main thread, with the window's GL context current: creates the mpv core
  // and its GL renderer. Without it nothing plays, and Play*() say so.
  // 'fonts_dir' is where text subtitles find a font: the console has no
  // fontconfig, so without one they would not show.
  bool Initialize(std::string& error, const std::string& fonts_dir = std::string());
  // Main thread, GL context still current.
  void Shutdown();

  // Starts live playback of a channel, replacing whatever is playing.
  // Returns once subscribed; the picture follows when the server starts
  // the stream.
  bool PlayChannel(HtspClient& client, uint32_t channel_id, std::string& error);

  // Starts playback of a recording over the HTSP file API at
  // 'start_seconds', replacing whatever is playing. Works for finished and
  // in-progress recordings.
  bool PlayRecording(HtspClient& client, uint32_t dvr_id, int64_t start_seconds,
                     std::string& error);

  // Stops playback, unsubscribing or closing the file as appropriate.
  void Stop(HtspClient& client);

  bool IsPlaying() const { return active_; }
  // True while playing a recording rather than live TV.
  bool IsRecordingPlayback() const { return source_ == Source::Recording; }
  // True once mpv has the stream open and knows its tracks.
  bool IsLoaded() const { return loaded_; }
  // True once a recording has played to its end (a running one never
  // does: its end is waited for). The app then leaves the player.
  bool AtEnd() const { return active_ && source_ == Source::Recording && eof_; }

  // Called every frame after the backbuffer has been cleared and before the
  // UI is rendered; draws the current video frame into framebuffer 0. The
  // document body is transparent while playing, so the UI composites on top.
  void RenderVideo(int width, int height);

  // What is wrong, or still to come: "Tuning...", the last
  // subscriptionStatus, why playback failed. Empty while all is well.
  std::string StatusText() const;

  // --- Transport, recordings only ----------------------------------------
  bool CanSeek() const { return source_ == Source::Recording && seekable_; }
  // Position and duration in microseconds; -1 when unknown.
  int64_t PositionUs() const { return position_us_.load(); }
  int64_t DurationUs() const { return duration_us_.load(); }
  // False when the duration moves while playing: a recording still being
  // written grows, so it is no basis for "ends at".
  bool DurationReliable() const { return !duration_unreliable_.load(); }
  // Seeks by a relative amount; stops a second short of the end. No-op for
  // live TV.
  void SeekRelative(int64_t delta_us);
  // Live TV doesn't wait for anyone: recordings only.
  void SetPaused(bool paused);
  // Returns the new paused state.
  bool TogglePause();
  bool IsPaused() const { return paused_; }
  // Playback speed, 1.0 = normal; recordings only. Reset for every stream.
  void SetSpeed(double speed);
  double Speed() const { return speed_.load(); }

  // --- Picture and sound adjustments ----------------------------------------
  // Sticky across streams (they are about the room, not the programme),
  // except the subtitle delay, which belongs to one recording's timing.
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
  // Dynamic range compression, for watching late without waking the house:
  // quiet dialogue up, explosions down.
  void SetNightMode(bool on);
  bool NightMode() const { return night_mode_; }
  // Languages tried first when a stream opens, as mpv's alang/slang lists
  // ("swe,eng"); "" = the stream's default. A subtitle track in a preferred
  // language is switched on by itself; with none, subtitles start off.
  // Applies from the next stream (the next channel).
  void SetPreferredLanguages(const std::string& audio, const std::string& subs);

  // --- Tracks ---------------------------------------------------------------
  // Label of the video track shown; "" when there is none.
  std::string CurrentVideoLabel() const;

  std::vector<TrackInfo> AudioTracks() const;
  int CurrentAudioStream() const { return audio_id_.load(); }
  std::string CurrentAudioLabel() const;
  bool SelectAudioTrack(int id);

  std::vector<TrackInfo> SubtitleTracks() const;
  // mpv id of the subtitle shown, or -1 when they are off.
  int CurrentSubtitleStream() const { return sub_id_.load(); }
  // Shows track 'id', or turns subtitles off for -1.
  bool SelectSubtitleTrack(int id);

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
  // Undoes what the last stream left set, before the next one starts.
  void ResetPerStream(int64_t start_seconds);
  void SetDouble(const char* name, double value);

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

  // Event thread: the last mpv message, not printed again while it repeats.
  std::string log_repeat_text_;
  int log_repeats_ = 0;

  // Adjustments (main thread).
  Fit fit_ = Fit::Fit;
  double audio_delay_ = 0.0;
  double sub_delay_ = 0.0;
  double sub_scale_ = 1.0;
  bool night_mode_ = false;

  mutable std::mutex status_mutex_;
  std::string status_;
};

#endif
