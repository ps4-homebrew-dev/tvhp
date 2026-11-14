// Watching a channel or a recording: the info bar, the transport, the
// playback menu and the sleep timer, as in jtplay. Tvheadend keeps the
// resume positions, so every client of the server shares them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "app.h"
#include "app_internal.h"
#include "clock.h"
#include "icons.h"
#include "keymap.h"
#include "format.h"

using namespace appdetail;
using namespace textfmt;

namespace {

constexpr double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
constexpr int kSpeedCount = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));

// Sleep timer choices, minutes; -1 = at the end of the programme on air.
constexpr int kSleepChoices[] = {0, 15, 30, 45, 60, 90, 120, -1};
constexpr int kSleepChoiceCount = (int)(sizeof(kSleepChoices) / sizeof(kSleepChoices[0]));

// Seek steps, seconds, as presses in one direction follow each other.
constexpr int64_t kSeekSteps[] = {10, 10, 10, 30, 30, 30, 60, 60, 120, 300};
constexpr int kSeekStepCount = (int)(sizeof(kSeekSteps) / sizeof(kSeekSteps[0]));
// How long after a press the next one still belongs to the run.
constexpr double kSeekRunSec = 1.5;

const char* FitName(Player::Fit fit) {
  switch (fit) {
  case Player::Fit::Fill:    return "Fill (crop)";
  case Player::Fit::Stretch: return "Stretch";
  default:                   return "Fit";
  }
}

// "+0.15 s", "0 s".
std::string DelayLabel(double seconds) {
  if (std::fabs(seconds) < 0.0005)
    return "0 s";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%+.2f s", seconds);
  return buf;
}

std::string SpeedLabel(double speed) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%g×", speed);
  return buf;
}

// "Ends at 21:40", at this speed; only for something longer than a few
// minutes.
std::string EndsAt(int64_t pos, int64_t dur, double speed) {
  if (pos < 0 || dur < 5 * 60 * 1000000LL || pos >= dur || speed <= 0.0)
    return {};
  const std::time_t end =
    (std::time_t)tvclock::Now() + (std::time_t)((double)(dur - pos) / 1e6 / speed);
  std::tm tm = {};
  localtime_r(&end, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "Ends at %H:%M", &tm);
  return buf;
}

// 'part' of 'whole' as a CSS width; two decimals keep a long film's bar
// moving smoothly.
std::string Percent(int64_t part, int64_t whole) {
  if (whole <= 0 || part <= 0)
    return "0%";
  const double pct = std::clamp((double)part * 100.0 / (double)whole, 0.0, 100.0);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f%%", pct);
  return buf;
}

// Index of the next track after the one with id 'current' in 'direction',
// wrapping around; 'current' not found counts as before the first.
size_t NextTrack(const std::vector<TrackInfo>& tracks, int current, int direction) {
  size_t i = 0;
  while (i < tracks.size() && tracks[i].id != current)
    i++;
  if (i == tracks.size())
    return direction > 0 ? 0 : tracks.size() - 1;
  return (i + tracks.size() + (direction > 0 ? 1 : -1)) % tracks.size();
}

} // namespace

// ---------------------------------------------------------------------------
// Starting and stopping
// ---------------------------------------------------------------------------

void App::PlayChannel(uint32_t channel_id)
{
  if (playing_dvr_id_)
    SaveRecordingPosition();
  std::string error;
  if (!player_->PlayChannel(client_, channel_id, error))
    {
      ShowToast(error.empty() ? "Unable to start playback" : error, icon::kWarning);
      return;
    }
  playing_channel_id_ = channel_id;
  playing_dvr_id_ = 0;
  // "End of programme" follows the zap.
  if (sleep_end_of_programme_)
    {
      const HtspClient::NowNext nn = client_.GetNowNext(channel_id);
      const time_t now = (time_t)tvclock::Now();
      if (nn.has_now && nn.now.stop > now)
	sleep_at_ = Now() + (double)(nn.now.stop - now);
      else
	{
	  sleep_at_ = 0.0;
	  sleep_end_of_programme_ = false;
	}
    }
  EnterWatch();
}

void App::StartRecording(uint32_t dvr_id, int64_t start_seconds)
{
  std::string error;
  if (!player_->PlayRecording(client_, dvr_id, start_seconds, error))
    {
      ShowToast(error.empty() ? "Unable to play this recording" : error, icon::kWarning);
      return;
    }
  playing_dvr_id_ = dvr_id;
  playing_channel_id_ = 0;
  position_saved_s_ = start_seconds;
  position_saved_at_ = Now();
  if (sleep_end_of_programme_)
    {
      sleep_at_ = 0.0;
      sleep_end_of_programme_ = false;
    }
  EnterWatch();
  if (start_seconds > 0)
    ShowToast("Resumed at " + FmtPosition(start_seconds * 1000000), icon::kResume);
}

void App::StopPlayback()
{
  if (playing_dvr_id_)
    SaveRecordingPosition();
  if (player_)
    player_->Stop(client_);
  const bool was_playing = playing_channel_id_ || playing_dvr_id_;
  playing_channel_id_ = 0;
  playing_dvr_id_ = 0;
  // A sleep timer is for this viewing, not the next.
  if (was_playing)
    {
      sleep_at_ = 0.0;
      sleep_end_of_programme_ = false;
    }
  ExitWatch();
  Set(bind_player_status_, player_ ? Rml::String(player_->StatusText()) : Rml::String(),
      "player_status");
  // The recordings list shows how far it got.
  if (section_ == Section::Recordings)
    RefreshFromClient(true);
}

// jtplay's rules: under kMinResumeSec is no position; past kWatchedFraction
// (the rest is mostly padding) is watched, and starts over next time.
void App::SaveRecordingPosition(bool ended)
{
  if (!playing_dvr_id_ || !player_ || !player_->IsRecordingPlayback())
    return;
  const int64_t pos = player_->PositionUs();
  const int64_t dur = player_->DurationUs();
  // Stopped before the stream opened: what the server has stays.
  if (!ended && pos < 0)
    return;

  // A recording still being written has no end to measure against.
  const bool watched = ended ||
    (player_->DurationReliable() && dur > 0 && pos >= (int64_t)(kWatchedFraction * (double)dur));
  int64_t secs = watched ? 0 : pos / 1000000;
  if (secs < kMinResumeSec)
    secs = 0;

  if (watched && position_saved_s_ == -2)
    return;
  if (!watched && secs == position_saved_s_)
    return;
  client_.SavePlayPosition(playing_dvr_id_, (uint32_t)secs, watched);
  position_saved_s_ = watched ? -2 : secs;
  position_saved_at_ = Now();
}

void App::EnterWatch()
{
  Set(bind_watching_, true, "watching");
  Set(bind_fullscreen_, true, "fullscreen");
  MarkPlayingRows();
  Set(bind_watch_recorded_, player_ && player_->IsRecordingPlayback(), "watch_recorded");
  seek_accum_us_ = 0;
  seek_run_ = 0;
  seek_bubble_until_ = 0.0;
  Set(bind_watch_seek_, Rml::String(), "watch_seek");
  UpdateWatchOverlay();
  ShowWatchInfo();
}

void App::ExitWatch()
{
  // The playback menu goes with the player; a question over the menus
  // stays.
  if (bind_menu_open_ && !bind_menu_dialog_)
    CloseMenu();
  Set(bind_watching_, false, "watching");
  Set(bind_fullscreen_, false, "fullscreen");
  HideWatchInfo();
  MarkPlayingRows();
}

void App::SetFullscreen(bool on)
{
  if (!bind_watching_ || bind_fullscreen_ == on)
    return;
  if (bind_menu_open_)
    CloseMenu(); // the playback menu, over either
  Set(bind_fullscreen_, on, "fullscreen");
  if (on)
    {
      ShowWatchInfo();
      return;
    }

  // Back to the menus, playing on, with the list at what plays.
  HideWatchInfo();
  seek_bubble_until_ = 0.0;
  if (player_->IsRecordingPlayback())
    {
      SetSection(Section::Recordings);
      for (size_t i = 0; i < recordings_.size(); i++)
	if (recordings_[i].id == playing_dvr_id_)
	  {
	    sel_rec_ = (int)i;
	    selected_dvr_id_ = playing_dvr_id_;
	    model_.DirtyVariable("sel_rec");
	    RebuildRecordingDetail();
	  }
      scroll_rec_pending_ = true;
    }
  else
    {
      SetSection(Section::Channels);
      SetZone(Zone::Channels);
      for (size_t i = 0; i < channels_.size(); i++)
	if (channels_[i].id == playing_channel_id_ && sel_channel_ != (int)i)
	  {
	    sel_channel_ = (int)i;
	    sel_epg_ = 0;
	    model_.DirtyVariable("sel_channel");
	    RebuildEpgRows();
	    RebuildDetail();
	  }
      scroll_channels_pending_ = true;
    }
  MarkPlayingRows();
  UpdateWatchOverlay();
}

void App::MarkPlayingRows()
{
  bool changed = false;
  for (size_t i = 0; i < channel_rows_.size() && i < channels_.size(); i++)
    {
      const bool playing = bind_watching_ && channels_[i].id == playing_channel_id_;
      changed |= channel_rows_[i].playing != playing;
      channel_rows_[i].playing = playing;
    }
  if (changed)
    model_.DirtyVariable("channels");
  changed = false;
  for (size_t i = 0; i < rec_rows_.size() && i < recordings_.size(); i++)
    {
      const bool playing = bind_watching_ && recordings_[i].id == playing_dvr_id_;
      changed |= rec_rows_[i].playing != playing;
      rec_rows_[i].playing = playing;
    }
  if (changed)
    model_.DirtyVariable("recordings");
}

void App::ShowWatchInfo()
{
  info_deadline_ = Now() + kWatchInfoSec;
  Set(bind_info_visible_, true, "info_visible");
}

void App::HideWatchInfo()
{
  Set(bind_info_visible_, false, "info_visible");
}

// ---------------------------------------------------------------------------
// Info bar
// ---------------------------------------------------------------------------

void App::UpdateWatchOverlay()
{
  Rml::String num, name, now_title, now_time, progress = "0%", ends;
  const bool recorded = player_ && player_->IsRecordingPlayback();

  if (recorded)
    {
      HtspDvrEntry entry;
      if (client_.GetDvrEntry(playing_dvr_id_, &entry))
	{
	  name = entry.title.empty() ? "Recording" : entry.title;
	  now_title = entry.channel_name;
	  if (!entry.subtitle.empty() && entry.subtitle != entry.title)
	    now_title += (now_title.empty() ? "" : "  ·  ") + entry.subtitle;
	}
      else
	{
	  name = "Recording";
	}

      const int64_t pos = player_->PositionUs();
      const int64_t dur = player_->DurationUs();
      now_time = FmtPosition(pos) + (dur > 0 ? " / " + FmtPosition(dur) : "");
      progress = Percent(pos, dur);
      if (player_->DurationReliable())
	ends = EndsAt(pos, dur, player_->Speed());

      // The seek bubble (240 px, watch.rcss) over the playhead, kept on the
      // 1920 px bar.
      if (Now() <= seek_bubble_until_ && dur > 0)
	{
	  const double half = 120.0 * 100.0 / 1920.0;
	  const double at = std::clamp((double)std::max<int64_t>(pos, 0) * 100.0 / (double)dur,
				       half, 100.0 - half);
	  char buf[16];
	  std::snprintf(buf, sizeof(buf), "%.2f%%", at);
	  Set(bind_watch_seek_at_, Rml::String(buf), "watch_seek_at");
	}
    }
  else
    {
      const HtspChannel* ch = nullptr;
      for (const HtspChannel& c : channels_)
	if (c.id == playing_channel_id_)
	  {
	    ch = &c;
	    break;
	  }

      if (ch)
	{
	  if (ch->number)
	    num = std::to_string(ch->number);
	  name = ch->name;
	  const HtspClient::NowNext nn = client_.GetNowNext(ch->id);
	  if (nn.has_now)
	    {
	      now_title = nn.now.title;
	      now_time = FmtRange(nn.now.start, nn.now.stop);
	      const time_t now = (time_t)tvclock::Now();
	      if (nn.now.stop > nn.now.start)
		progress = Percent(now - nn.now.start, nn.now.stop - nn.now.start);
	    }
	}
    }

  if (Now() > seek_bubble_until_)
    Set(bind_watch_seek_, Rml::String(), "watch_seek");
  Set(bind_watch_num_, num, "watch_num");
  Set(bind_watch_icon_, recorded || !playing_channel_id_ ? Rml::String("none")
					     : IconDecorator(playing_channel_id_),
      "watch_icon");
  Set(bind_watch_name_, name, "watch_name");
  Set(bind_watch_now_, now_title, "watch_now");
  Set(bind_watch_time_, now_time, "watch_time");
  Set(bind_watch_progress_, progress, "watch_progress");
  Set(bind_watch_ends_, ends, "watch_ends");
  Set(bind_player_status_, player_ ? Rml::String(player_->StatusText()) : Rml::String(), "player_status");

  Rml::String vtrack, atrack, strack, speed;
  if (player_)
    {
      vtrack = player_->CurrentVideoLabel();
      atrack = player_->CurrentAudioLabel();
      const int sid = player_->CurrentSubtitleStream();
      for (const TrackInfo& t : player_->SubtitleTracks())
	if (t.id == sid)
	  strack = "Subtitles: " + t.label;
      if (recorded && std::fabs(player_->Speed() - 1.0) >= 0.01)
	speed = SpeedLabel(player_->Speed());
    }
  Set(bind_watch_vtrack_, vtrack, "watch_vtrack");
  Set(bind_watch_atrack_, atrack, "watch_atrack");
  Set(bind_watch_strack_, strack, "watch_strack");
  Set(bind_watch_speed_, speed, "watch_speed");
  Set(bind_watch_sleep_, sleep_at_ > 0 ? Rml::String("Sleep: " + SleepLabel()) : Rml::String(),
      "watch_sleep");
  Set(bind_watch_paused_, player_ && player_->IsPaused(), "watch_paused");
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void App::SeekStep(int sign, bool large)
{
  if (!player_->CanSeek())
    {
      ShowToast("This recording can't be seeked", icon::kWarning);
      return;
    }
  if (Now() > seek_bubble_until_ || (seek_accum_us_ < 0) != (sign < 0))
    {
      seek_accum_us_ = 0;
      seek_run_ = 0;
    }
  const int64_t step_s = large ? kSeekLargeUs / 1000000
			       : kSeekSteps[std::min(seek_run_, kSeekStepCount - 1)];
  seek_run_++;
  const int64_t delta = sign * step_s * 1000000;
  player_->SeekRelative(delta);

  // "» +2:30", Kodi style.
  seek_accum_us_ += delta;
  seek_bubble_until_ = Now() + kSeekRunSec;
  const int64_t secs = std::llabs(seek_accum_us_) / 1000000;
  const char* arrow = seek_accum_us_ < 0 ? "«" : "»";
  const char sign_char = seek_accum_us_ < 0 ? '-' : '+';
  char label[48];
  if (secs >= 60 && secs % 60 == 0)
    std::snprintf(label, sizeof(label), "%s %c%lld min", arrow, sign_char, (long long)(secs / 60));
  else if (secs >= 60)
    std::snprintf(label, sizeof(label), "%s %c%s", arrow, sign_char,
		  FmtPosition(secs * 1000000).c_str());
  else
    std::snprintf(label, sizeof(label), "%s %c%lld s", arrow, sign_char, (long long)secs);
  Set(bind_watch_seek_, Rml::String(label), "watch_seek");
  UpdateWatchOverlay();
  ShowWatchInfo();
}

// ---------------------------------------------------------------------------
// Tracks and preferences
// ---------------------------------------------------------------------------

void App::PickAudioTrack(const TrackInfo& track)
{
  if (!player_->SelectAudioTrack(track.id))
    return;
  // First in mpv's list; the choice before stays second.
  PlayerPrefs& prefs = config_.Player();
  if (!track.lang.empty() && prefs.audio_lang.rfind(track.lang, 0) != 0)
    {
      const std::string previous = prefs.audio_lang.substr(0, prefs.audio_lang.find(','));
      prefs.audio_lang = track.lang + (previous.empty() || previous == track.lang ? "" : "," + previous);
      SavePlayerPrefs();
    }
}

void App::PickSubtitleTrack(const TrackInfo* track)
{
  if (!player_->SelectSubtitleTrack(track ? track->id : -1))
    return;
  PlayerPrefs& prefs = config_.Player();
  const std::string lang = track ? track->lang : std::string();
  if (prefs.sub_lang != lang)
    {
      prefs.sub_lang = lang;
      SavePlayerPrefs();
    }
}

void App::CycleAudioTrack(int direction)
{
  const std::vector<TrackInfo> tracks = player_->AudioTracks();
  if (tracks.size() < 2)
    {
      if (!tracks.empty())
	ShowToast("Only one audio track", icon::kAudio);
      return;
    }
  const size_t next = NextTrack(tracks, player_->CurrentAudioStream(), direction);
  PickAudioTrack(tracks[next]);
  ShowToast("Audio " + std::to_string(next + 1) + "/" + std::to_string(tracks.size()) +
	    ": " + tracks[next].label, icon::kAudio);
}

// "Off", then every track.
void App::CycleSubtitleTrack(int direction)
{
  std::vector<TrackInfo> choices = player_->SubtitleTracks();
  if (choices.empty())
    {
      ShowToast("No subtitles");
      return;
    }
  TrackInfo off;
  off.id = -1;
  off.label = "Off";
  choices.insert(choices.begin(), off);
  const size_t next = NextTrack(choices, player_->CurrentSubtitleStream(), direction);
  PickSubtitleTrack(next == 0 ? nullptr : &choices[next]);
  ShowToast("Subtitles: " + choices[next].label);
}

void App::ApplyPlayerPrefs()
{
  const PlayerPrefs& prefs = config_.Player();
  player_->SetFit((Player::Fit)std::clamp(prefs.fit, 0, (int)Player::Fit::Count - 1));
  player_->SetAudioDelay(prefs.audio_delay);
  player_->SetSubtitleScale(prefs.sub_scale);
  player_->SetPreferredLanguages(prefs.audio_lang, prefs.sub_lang);
}

void App::SavePlayerPrefs()
{
  PlayerPrefs& prefs = config_.Player();
  prefs.fit = (int)player_->GetFit();
  prefs.audio_delay = player_->AudioDelay();
  prefs.sub_scale = player_->SubtitleScale();
  // The languages apply from the next stream.
  player_->SetPreferredLanguages(prefs.audio_lang, prefs.sub_lang);
  if (!config_.Save(config_path_))
    std::fprintf(stderr, "Unable to save %s\n", config_path_.c_str());
}

// ---------------------------------------------------------------------------
// Sleep timer
// ---------------------------------------------------------------------------

std::string App::SleepLabel() const
{
  if (sleep_at_ <= 0)
    return "Off";
  const int minutes = std::max(1, (int)std::ceil((sleep_at_ - Now()) / 60.0));
  if (sleep_end_of_programme_)
    return "End of programme (" + std::to_string(minutes) + " min)";
  return std::to_string(minutes) + " min left";
}

void App::UpdateSleepTimer()
{
  if (sleep_at_ <= 0 || Now() < sleep_at_)
    return;
  sleep_at_ = 0.0;
  sleep_end_of_programme_ = false;
  if (bind_watching_)
    {
      StopPlayback();
      ShowToast("Sleep timer: playback stopped", icon::kSleep);
    }
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

void App::OpenWatchMenu()
{
  const bool recorded = player_->IsRecordingPlayback();
  std::vector<MenuItem> items;

  if (VideoBehind())
    items.push_back({"Full screen", nullptr, [this](int) { SetFullscreen(true); }});

  if (player_->AudioTracks().size() > 1)
    {
      items.push_back({"Audio",
	[this] { return player_->CurrentAudioLabel(); },
	[this](int d) { CycleAudioTrack(d < 0 ? -1 : +1); }});
    }
  if (!player_->SubtitleTracks().empty())
    {
      items.push_back({"Subtitles",
	[this] {
	  const int id = player_->CurrentSubtitleStream();
	  for (const TrackInfo& t : player_->SubtitleTracks())
	    if (t.id == id)
	      return t.label;
	  return std::string("Off");
	},
	[this](int d) { CycleSubtitleTrack(d < 0 ? -1 : +1); }});
    }
  if (player_->CanSeek())
    {
      items.push_back({"Speed",
	[this] { return SpeedLabel(player_->Speed()); },
	[this](int d) {
	  int i = 0;
	  while (i < kSpeedCount - 1 && kSpeeds[i] < player_->Speed() - 0.01)
	    i++;
	  i = d == 0 ? (i + 1) % kSpeedCount : std::clamp(i + (d < 0 ? -1 : 1), 0, kSpeedCount - 1);
	  player_->SetSpeed(kSpeeds[i]);
	}});
    }
  items.push_back({"Picture",
    [this] { return std::string(FitName(player_->GetFit())); },
    [this](int d) {
      const int n = (int)Player::Fit::Count;
      player_->SetFit((Player::Fit)(((int)player_->GetFit() + (d < 0 ? n - 1 : 1)) % n));
      SavePlayerPrefs();
    }});
  if (!player_->SubtitleTracks().empty())
    {
      if (recorded)
	{
	  items.push_back({"Subtitle delay",
	    [this] { return DelayLabel(player_->SubtitleDelay()); },
	    [this](int d) {
	      player_->SetSubtitleDelay(d == 0 ? 0.0 : player_->SubtitleDelay() + (d < 0 ? -0.1 : 0.1));
	    }});
	}
      items.push_back({"Subtitle size",
	[this] { return std::to_string((int)std::lround(player_->SubtitleScale() * 100)) + "%"; },
	[this](int d) {
	  const double scale = d == 0 ? 1.0 : player_->SubtitleScale() + (d < 0 ? -0.1 : 0.1);
	  player_->SetSubtitleScale(std::clamp(scale, 0.5, 2.5));
	  SavePlayerPrefs();
	}});
    }
  items.push_back({"Audio delay",
    [this] { return DelayLabel(player_->AudioDelay()); },
    [this](int d) {
      player_->SetAudioDelay(d == 0 ? 0.0 : player_->AudioDelay() + (d < 0 ? -0.05 : 0.05));
      SavePlayerPrefs();
    }});
  items.push_back({"Sleep timer",
    [this] { return SleepLabel(); },
    [this, recorded](int d) {
      // "End of programme" is for live TV; a recording ends anyway.
      const HtspClient::NowNext nn = recorded ? HtspClient::NowNext()
					      : client_.GetNowNext(playing_channel_id_);
      const time_t now = (time_t)tvclock::Now();
      const bool can_end = nn.has_now && nn.now.stop > now;
      const int count = can_end ? kSleepChoiceCount : kSleepChoiceCount - 1;
      int i = 0;
      if (sleep_end_of_programme_)
	i = kSleepChoiceCount - 1;
      else if (sleep_at_ > 0)
	{
	  const double left = (sleep_at_ - Now()) / 60.0;
	  i = 1;
	  while (i < kSleepChoiceCount - 2 && kSleepChoices[i] < left - 0.5)
	    i++;
	}
      i = std::min(i, count - 1);
      i = (i + (d < 0 ? count - 1 : 1)) % count;
      sleep_end_of_programme_ = kSleepChoices[i] < 0;
      if (sleep_end_of_programme_)
	sleep_at_ = Now() + (double)(nn.now.stop - now);
      else
	sleep_at_ = kSleepChoices[i] > 0 ? Now() + kSleepChoices[i] * 60.0 : 0.0;
    }});
  items.push_back({"Stop", nullptr, [this](int) { StopPlayback(); }});
  ShowMenu(recorded ? "Playback" : "Live TV", std::move(items), false);
}

std::vector<App::MenuItem> App::PlayItems(const HtspDvrEntry& entry)
{
  const uint32_t id = entry.id;
  if (client_.SupportsPlayPosition() && entry.play_position >= kMinResumeSec)
    {
      const int64_t at = entry.play_position;
      return {{"Resume from " + FmtPosition(at * 1000000), nullptr,
	       [this, id, at](int) { StartRecording(id, at); }},
	      {"Play from beginning", nullptr, [this, id](int) { StartRecording(id, 0); }}};
    }
  return {{"Play", nullptr, [this, id](int) { StartRecording(id, 0); }}};
}

void App::OpenResumeDialog(const HtspDvrEntry& entry)
{
  // Of the scheduled length; the file runs a little longer with padding.
  const int64_t at = entry.play_position;
  const int64_t length = entry.stop - entry.start;
  std::string meta, progress = "0%";
  if (length > at)
    {
      meta = FmtPosition(at * 1000000) + " of " + FmtPosition(length * 1000000) + "  ·  " +
	     FmtPosition((length - at) * 1000000) + " left";
      progress = Percent(at, length);
    }
  ShowMenu(entry.title.empty() ? "Recording" : entry.title, PlayItems(entry), true, meta,
	   progress);
}

void App::ShowMenu(const std::string& title, std::vector<MenuItem> items, bool dialog,
		   const std::string& meta, const std::string& progress)
{
  menu_items_ = std::move(items);
  Set(bind_menu_title_, Rml::String(title), "menu_title");
  Set(bind_menu_meta_, Rml::String(meta), "menu_meta");
  Set(bind_menu_progress_, Rml::String(progress), "menu_progress");
  Set(bind_menu_dialog_, dialog, "menu_dialog");
  sel_menu_ = 0;
  RebuildMenuRows();
  Set(bind_menu_open_, true, "menu_open");
}

void App::OpenContextMenu(const std::string& title, std::vector<MenuItem> items)
{
  if (!items.empty())
    ShowMenu(title, std::move(items), false);
}

void App::OpenConfirm(const std::string& question, const std::string& detail,
		      const std::string& action, std::function<void()> run)
{
  ShowMenu(question,
	   {{action, nullptr, [run](int) { run(); }}, {"Cancel", nullptr, [](int) {}}},
	   true, detail);
}

void App::OpenOptionsMenu()
{
  if (VideoBehind())
    {
      OpenWatchMenu();
      return;
    }

  std::vector<MenuItem> items;
  std::string title;
  if (section_ == Section::Recordings)
    {
      if (const HtspDvrEntry* e = SelectedRecording())
	{
	  const HtspDvrEntry entry = *e;
	  title = entry.title.empty() ? "Recording" : entry.title;
	  if (entry.HasFile())
	    {
	      items = PlayItems(entry);
	      if (client_.SupportsPlayPosition())
		{
		  const bool watched = entry.play_count > 0 && entry.play_position < kMinResumeSec;
		  items.push_back({watched ? "Mark as unwatched" : "Mark as watched", nullptr,
		    [this, entry, watched](int) {
		      client_.SetWatched(entry.id, !watched);
		      ShowToast(watched ? "Marked as unwatched" : "Marked as watched", icon::kDone);
		      RefreshFromClient(true);
		    }});
		}
	    }
	  if (entry.IsPending())
	    items.push_back({entry.IsRecording() ? "Stop recording" : "Cancel recording", nullptr,
	      [this, entry](int) { CancelOrDeleteRecording(entry, false); }});
	  else
	    items.push_back({"Delete", nullptr,
	      [this, entry](int) { CancelOrDeleteRecording(entry, true); }});
	}
    }
  else
    {
      const HtspChannel* ch = sel_channel_ >= 0 && sel_channel_ < (int)channels_.size()
			      ? &channels_[sel_channel_] : nullptr;
      if (zone_ == Zone::Epg && sel_epg_ >= 0 && sel_epg_ < (int)epg_events_.size())
	{
	  const HtspEvent ev = epg_events_[sel_epg_];
	  title = ev.title.empty() ? "Programme" : ev.title;
	  HtspDvrEntry entry;
	  const bool have = client_.GetDvrEntryForEvent(ev.id, &entry);
	  if (have && entry.IsPending())
	    items.push_back({entry.IsRecording() ? "Stop recording" : "Cancel recording", nullptr,
	      [this, entry](int) { CancelOrDeleteRecording(entry, false); }});
	  else if (!(have && entry.IsFinished()))
	    items.push_back({"Record", nullptr, [this](int) { ToggleRecordSelectedEvent(); }});
	}
      else if (ch)
	{
	  title = ch->name;
	}
      if (ch)
	{
	  const uint32_t id = ch->id;
	  items.push_back({"Watch " + ch->name, nullptr, [this, id](int) { PlayChannel(id); }});
	}
    }
  items.push_back(InputTraceItem());
  items.push_back({"Switch server", nullptr, [this](int) { DisconnectToLogin(); }});
  OpenContextMenu(title.empty() ? std::string("Options") : title, std::move(items));
}

void App::CloseMenu()
{
  Set(bind_menu_open_, false, "menu_open");
  menu_items_.clear();
}

void App::RebuildMenuRows()
{
  scroll_menu_pending_ = true;
  menu_rows_.clear();
  for (const MenuItem& item : menu_items_)
    menu_rows_.push_back({item.label, item.value ? item.value() : ""});
  model_.DirtyVariable("menu");
  model_.DirtyVariable("sel_menu");
}

void App::HandleKeyMenu(Rml::Event& event, int key)
{
  event.StopPropagation(); // modal
  const int count = (int)menu_items_.size();
  if (count == 0)
    {
      CloseMenu();
      return;
    }

  switch (keymap::Command(key))
    {
    case keymap::Cmd::Up:
    case keymap::Cmd::Down:
      sel_menu_ = StepIndex(sel_menu_, keymap::Command(key) == keymap::Cmd::Up ? -1 : +1, count);
      model_.DirtyVariable("sel_menu");
      scroll_menu_pending_ = true;
      break;
    case keymap::Cmd::Left:
    case keymap::Cmd::Right:
      if (menu_items_[sel_menu_].value)
	{
	  menu_items_[sel_menu_].change(keymap::Command(key) == keymap::Cmd::Left ? -1 : +1);
	  RebuildMenuRows();
	}
      break;
    case keymap::Cmd::Ok:
      if (menu_items_[sel_menu_].value)
	{
	  menu_items_[sel_menu_].change(0);
	  RebuildMenuRows();
	}
      else
	{
	  // Closed first: the action may open another menu.
	  const std::function<void(int)> action = menu_items_[sel_menu_].change;
	  CloseMenu();
	  action(0);
	}
      break;
    case keymap::Cmd::Back:
    case keymap::Cmd::Options:
      CloseMenu();
      break;
    case keymap::Cmd::Stop:
      CloseMenu();
      if (bind_watching_)
	StopPlayback();
      break;
    default:
      break;
    }
  if (bind_watching_ && bind_fullscreen_)
    ShowWatchInfo();
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void App::HandleKeyWatch(Rml::Event& event, int key)
{
  const bool recorded = player_ && player_->IsRecordingPlayback();
  // Up and P+ are the next channel, or 5 minutes on; L1 and R1 go by
  // their side: back, and on.
  keymap::Cmd cmd = keymap::Command(key);
  if (cmd == keymap::Cmd::TabPrev)
    cmd = keymap::Cmd::PageDown;
  else if (cmd == keymap::Cmd::TabNext)
    cmd = keymap::Cmd::PageUp;

  switch (cmd)
    {
    case keymap::Cmd::Options:
      ShowWatchInfo();
      OpenWatchMenu();
      event.StopPropagation();
      return;
    case keymap::Cmd::Audio:
      CycleAudioTrack(+1);
      ShowWatchInfo();
      event.StopPropagation();
      return;
    case keymap::Cmd::Subtitles:
      CycleSubtitleTrack(+1);
      ShowWatchInfo();
      event.StopPropagation();
      return;
    default:
      break;
    }

  if (recorded)
    {
      switch (cmd)
	{
	// Left/right and rewind/fast forward: steps that grow; up/down, the
	// shoulder buttons and skip: 5 minutes.
	case keymap::Cmd::Left:
	case keymap::Cmd::SeekBack:
	  SeekStep(-1, false);
	  break;
	case keymap::Cmd::Right:
	case keymap::Cmd::SeekForward:
	  SeekStep(+1, false);
	  break;
	case keymap::Cmd::Up:
	case keymap::Cmd::PageUp:
	case keymap::Cmd::SkipForward:
	  SeekStep(+1, true);
	  break;
	case keymap::Cmd::Down:
	case keymap::Cmd::PageDown:
	case keymap::Cmd::SkipBack:
	  SeekStep(-1, true);
	  break;
	case keymap::Cmd::Ok:
	case keymap::Cmd::PlayPause:
	  player_->TogglePause();
	  UpdateWatchOverlay();
	  ShowWatchInfo();
	  break;
	case keymap::Cmd::Secondary: // the info bar
	  if (bind_info_visible_ && !player_->IsPaused())
	    HideWatchInfo();
	  else
	    ShowWatchInfo();
	  break;
	// The menus, playing on behind them.
	case keymap::Cmd::Back:
	case keymap::Cmd::Guide:
	  SetFullscreen(false);
	  break;
	case keymap::Cmd::Stop:
	  StopPlayback();
	  ShowToast("Playback stopped", icon::kStop);
	  break;
	default:
	  return;
	}
      event.StopPropagation();
      return;
    }

  switch (cmd)
    {
    case keymap::Cmd::Up: // zap
    case keymap::Cmd::Down:
    case keymap::Cmd::PageUp:
    case keymap::Cmd::PageDown:
    case keymap::Cmd::SkipBack:
    case keymap::Cmd::SkipForward:
      {
	const bool up = (cmd == keymap::Cmd::Up || cmd == keymap::Cmd::PageUp ||
			 cmd == keymap::Cmd::SkipForward);
	const int delta = up ? +1 : -1;
	const int prev = sel_channel_;
	SetZone(Zone::Channels);
	MoveSelection(delta);
	if (sel_channel_ != prev && sel_channel_ >= 0 && sel_channel_ < (int)channels_.size())
	  PlayChannel(channels_[sel_channel_].id); // re-enters watch, shows info
	else
	  ShowWatchInfo();
	break;
      }
    // Live TV cannot pause: these show or hide the info bar.
    case keymap::Cmd::Ok:
    case keymap::Cmd::PlayPause:
    case keymap::Cmd::Secondary:
      if (bind_info_visible_)
	HideWatchInfo();
      else
	ShowWatchInfo();
      break;
    // The channel list, the channel playing on behind it.
    case keymap::Cmd::Left:
    case keymap::Cmd::Back:
    case keymap::Cmd::Guide:
      SetFullscreen(false);
      break;
    case keymap::Cmd::Stop:
      StopPlayback();
      ShowToast("Playback stopped", icon::kStop);
      break;
    default:
      return;
    }
  event.StopPropagation();
}
