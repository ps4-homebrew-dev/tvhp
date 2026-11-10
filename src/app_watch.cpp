// Watch view: full-screen playback of a channel or a recording, the
// auto-hiding info bar, the transport controls, and the playback menu.
//
// Much of it follows jtplay's player screen: the menu (tracks, subtitles,
// speed, night mode, picture, delays, sleep timer), seek steps that grow
// while pressed, the track labels and "ends at" on the bar, and the resume
// question. Tvheadend keeps the resume positions, so they are shared with
// every other client of the server.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "app.h"
#include "app_internal.h"
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

// Seek steps grow while the same direction is pressed (or held) in quick
// succession: three presses of each, then the largest step from there on.
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

// When what is playing will be over, at this speed, by the wall clock:
// "Ends at 21:40". Only for something long enough to plan an evening by.
std::string EndsAt(int64_t pos, int64_t dur, double speed) {
  if (pos < 0 || dur < 5 * 60 * 1000000LL || pos >= dur || speed <= 0.0)
    return {};
  const std::time_t end =
    std::time(nullptr) + (std::time_t)((double)(dur - pos) / 1e6 / speed);
  std::tm tm = {};
  localtime_r(&end, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "Ends at %H:%M", &tm);
  return buf;
}

// Percentage of 'part' in 'whole' as a CSS width, two decimals so the bar
// moves smoothly even through a three-hour film.
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
  // Zapping from a recording: how far it got goes to the server first.
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
  // "End of programme" follows to the channel zapped to.
  if (sleep_end_of_programme_)
    {
      const HtspClient::NowNext nn = client_.GetNowNext(channel_id);
      const time_t now = time(nullptr);
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
  start_position_s_ = start_seconds;
  position_saved_s_ = start_seconds;
  position_saved_at_ = Now();
  // The end of the programme on air means nothing to a recording.
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
  // A sleep timer is for this evening's viewing, not the next one.
  if (was_playing)
    {
      sleep_at_ = 0.0;
      sleep_end_of_programme_ = false;
    }
  ExitWatch();
  // Reflect the cleared status right away instead of waiting for the next
  // periodic refresh.
  const Rml::String status = player_ ? Rml::String(player_->StatusText()) : Rml::String();
  if (bind_player_status_ != status)
    {
      bind_player_status_ = status;
      model_.DirtyVariable("player_status");
    }
  // The recordings list shows how far it got.
  if (section_ == Section::Recordings)
    RefreshFromClient(true);
}

// Where the recording that plays has got, for the server. The rules are
// jtplay's: less than kMinResumeSec in is no position at all, past
// kWatchedFraction (the rest is mostly padding) counts as watched and
// starts over next time.
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

  // -2: counted as watched already, in this playback.
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
  if (!bind_watching_)
    {
      bind_watching_ = true;
      model_.DirtyVariable("watching");
    }
  // Whatever is chosen starts full screen, from the menus too.
  if (!bind_fullscreen_)
    {
      bind_fullscreen_ = true;
      model_.DirtyVariable("fullscreen");
    }
  MarkPlayingRows();
  const bool recorded = player_ && player_->IsRecordingPlayback();
  if (bind_watch_recorded_ != recorded)
    {
      bind_watch_recorded_ = recorded;
      model_.DirtyVariable("watch_recorded");
    }
  // A seek run belongs to what it was made in.
  seek_accum_us_ = 0;
  seek_run_ = 0;
  seek_bubble_until_ = 0.0;
  if (!bind_watch_seek_.empty())
    {
      bind_watch_seek_.clear();
      model_.DirtyVariable("watch_seek");
    }
  UpdateWatchOverlay();
  ShowWatchInfo(kWatchInfoSec);
}

void App::ExitWatch()
{
  // The playback menu goes with the player; the resume question over the
  // menus stays.
  if (bind_menu_open_ && !bind_menu_dialog_)
    CloseMenu();
  if (bind_watching_)
    {
      bind_watching_ = false;
      model_.DirtyVariable("watching");
    }
  if (bind_fullscreen_)
    {
      bind_fullscreen_ = false;
      model_.DirtyVariable("fullscreen");
    }
  if (bind_info_visible_)
    {
      bind_info_visible_ = false;
      model_.DirtyVariable("info_visible");
    }
  MarkPlayingRows();
}

void App::SetFullscreen(bool on)
{
  if (!bind_watching_ || bind_fullscreen_ == on)
    return;
  if (bind_menu_open_)
    CloseMenu(); // the playback menu, over either
  bind_fullscreen_ = on;
  model_.DirtyVariable("fullscreen");
  if (on)
    {
      ShowWatchInfo(kWatchInfoSec);
      return;
    }

  // Back to the menus, playing on: the info bar and a seek run go, and
  // the list comes back at what plays.
  if (bind_info_visible_)
    {
      bind_info_visible_ = false;
      model_.DirtyVariable("info_visible");
    }
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

void App::ShowWatchInfo(double seconds)
{
  info_deadline_ = Now() + seconds;
  if (!bind_info_visible_)
    {
      bind_info_visible_ = true;
      model_.DirtyVariable("info_visible");
    }
}

// ---------------------------------------------------------------------------
// The info bar
// ---------------------------------------------------------------------------

void App::UpdateWatchOverlay()
{
  Rml::String num, name, now_title, now_time, progress = "0%", ends;
  const bool recorded = player_ && player_->IsRecordingPlayback();

  if (recorded)
    {
      // Recording: the "channel number" slot becomes a play/pause marker and
      // the progress bar tracks the position in the file.
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
      // The OSD shows playing or paused in place of a channel number.
      if (player_->DurationReliable())
	ends = EndsAt(pos, dur, player_->Speed());

      // The seek bubble rides on the bar where the playhead is.
      if (Now() <= seek_bubble_until_ && dur > 0)
	{
	  // The bubble is centred on the playhead (240px wide, see watch.rcss),
	  // kept half its width from either end of the 1920px timeline.
	  const double half = 120.0 * 100.0 / 1920.0;
	  const double at = std::clamp((double)std::max<int64_t>(pos, 0) * 100.0 / (double)dur,
				       half, 100.0 - half);
	  char buf[16];
	  std::snprintf(buf, sizeof(buf), "%.2f%%", at);
	  if (bind_watch_seek_at_ != buf)
	    {
	      bind_watch_seek_at_ = buf;
	      model_.DirtyVariable("watch_seek_at");
	    }
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
	  num = std::to_string(ch->number);
	  name = ch->name;
	  const HtspClient::NowNext nn = client_.GetNowNext(ch->id);
	  if (nn.has_now)
	    {
	      now_title = nn.now.title;
	      now_time = FmtRange(nn.now.start, nn.now.stop);
	      const time_t now = time(nullptr);
	      if (nn.now.stop > nn.now.start)
		progress = Percent(now - nn.now.start, nn.now.stop - nn.now.start);
	    }
	}
    }

  auto set = [&](Rml::String& bind, const Rml::String& value, const char* var) {
    if (bind != value)
      {
	bind = value;
	model_.DirtyVariable(var);
      }
  };
  // The run of seeks is over: away with the bubble.
  if (Now() > seek_bubble_until_)
    set(bind_watch_seek_, Rml::String(), "watch_seek");
  set(bind_watch_num_, num, "watch_num");
  set(bind_watch_name_, name, "watch_name");
  set(bind_watch_now_, now_title, "watch_now");
  set(bind_watch_time_, now_time, "watch_time");
  set(bind_watch_progress_, progress, "watch_progress");
  set(bind_watch_ends_, ends, "watch_ends");
  set(bind_player_status_, player_ ? Rml::String(player_->StatusText()) : Rml::String(), "player_status");

  // The streams actually being decoded, so the labels follow a switch.
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
  set(bind_watch_vtrack_, vtrack, "watch_vtrack");
  set(bind_watch_atrack_, atrack, "watch_atrack");
  set(bind_watch_strack_, strack, "watch_strack");
  set(bind_watch_speed_, speed, "watch_speed");
  set(bind_watch_sleep_, sleep_at_ > 0 ? Rml::String("Sleep: " + SleepLabel()) : Rml::String(),
      "watch_sleep");
  const bool paused = player_ && player_->IsPaused();
  if (bind_watch_paused_ != paused)
    {
      bind_watch_paused_ = paused;
      model_.DirtyVariable("watch_paused");
    }
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
  // A run of presses (or a held button) in one direction speeds up: 10 s
  // steps at first, then 30 s, a minute, two, five. The large steps are
  // five minutes from the start.
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

  // How far this run of presses has jumped, Kodi style ("» +2:30").
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
  if (bind_watch_seek_ != label)
    {
      bind_watch_seek_ = label;
      model_.DirtyVariable("watch_seek");
    }
  UpdateWatchOverlay();
  ShowWatchInfo(kWatchInfoSec);
}

// ---------------------------------------------------------------------------
// Tracks and preferences
// ---------------------------------------------------------------------------

void App::PickAudioTrack(const TrackInfo& track)
{
  if (!player_->SelectAudioTrack(track.id))
    return;
  // Preferred from now on, as the first of mpv's list; the last choice
  // before it stays as a fallback.
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
  // Subtitles stay on in that language, or off, from channel to channel.
  PlayerPrefs& prefs = config_.Player();
  const std::string lang = track ? track->lang : std::string();
  if (prefs.sub_lang != lang)
    {
      prefs.sub_lang = lang;
      SavePlayerPrefs();
    }
}

// The remote's audio key / the playback menu: next audio track, wrapping.
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

// Subtitles cycle through "off" and every track.
void App::CycleSubtitleTrack(int direction)
{
  std::vector<TrackInfo> choices = player_->SubtitleTracks();
  if (choices.empty())
    return;
  TrackInfo off;
  off.id = -1;
  off.label = "Off";
  choices.insert(choices.begin(), off);
  const size_t next = NextTrack(choices, player_->CurrentSubtitleStream(), direction);
  PickSubtitleTrack(next == 0 ? nullptr : &choices[next]);
}

void App::ApplyPlayerPrefs()
{
  const PlayerPrefs& prefs = config_.Player();
  player_->SetNightMode(prefs.night_mode);
  player_->SetFit((Player::Fit)std::clamp(prefs.fit, 0, (int)Player::Fit::Count - 1));
  player_->SetAudioDelay(prefs.audio_delay);
  player_->SetSubtitleScale(prefs.sub_scale);
  player_->SetPreferredLanguages(prefs.audio_lang, prefs.sub_lang);
}

void App::SavePlayerPrefs()
{
  PlayerPrefs& prefs = config_.Player();
  prefs.night_mode = player_->NightMode();
  prefs.fit = (int)player_->GetFit();
  prefs.audio_delay = player_->AudioDelay();
  prefs.sub_scale = player_->SubtitleScale();
  // Languages: from the next channel on.
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
// The playback menu and the resume dialog
// ---------------------------------------------------------------------------

void App::OpenWatchMenu()
{
  const bool recorded = player_->IsRecordingPlayback();
  menu_items_.clear();
  bind_menu_title_ = recorded ? "Playback" : "Live TV";
  bind_menu_meta_.clear();
  bind_menu_dialog_ = false;

  // Behind the menus: the way back to the picture, first.
  if (VideoBehind())
    menu_items_.push_back({"Full screen", nullptr, [this](int) { SetFullscreen(true); }});

  if (player_->AudioTracks().size() > 1)
    {
      menu_items_.push_back({"Audio",
	[this] { return player_->CurrentAudioLabel(); },
	[this](int d) { CycleAudioTrack(d < 0 ? -1 : +1); }});
    }
  if (!player_->SubtitleTracks().empty())
    {
      menu_items_.push_back({"Subtitles",
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
      menu_items_.push_back({"Speed",
	[this] { return SpeedLabel(player_->Speed()); },
	[this](int d) {
	  int i = 0;
	  while (i < kSpeedCount - 1 && kSpeeds[i] < player_->Speed() - 0.01)
	    i++;
	  i = d == 0 ? (i + 1) % kSpeedCount : std::clamp(i + (d < 0 ? -1 : 1), 0, kSpeedCount - 1);
	  player_->SetSpeed(kSpeeds[i]);
	}});
    }
  menu_items_.push_back({"Night mode",
    [this] { return std::string(player_->NightMode() ? "On" : "Off"); },
    [this](int) { player_->SetNightMode(!player_->NightMode()); SavePlayerPrefs(); }});
  menu_items_.push_back({"Picture",
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
	  menu_items_.push_back({"Subtitle delay",
	    [this] { return DelayLabel(player_->SubtitleDelay()); },
	    [this](int d) {
	      player_->SetSubtitleDelay(d == 0 ? 0.0 : player_->SubtitleDelay() + (d < 0 ? -0.1 : 0.1));
	    }});
	}
      menu_items_.push_back({"Subtitle size",
	[this] { return std::to_string((int)std::lround(player_->SubtitleScale() * 100)) + "%"; },
	[this](int d) {
	  const double scale = d == 0 ? 1.0 : player_->SubtitleScale() + (d < 0 ? -0.1 : 0.1);
	  player_->SetSubtitleScale(std::clamp(scale, 0.5, 2.5));
	  SavePlayerPrefs();
	}});
    }
  menu_items_.push_back({"Audio delay",
    [this] { return DelayLabel(player_->AudioDelay()); },
    [this](int d) {
      player_->SetAudioDelay(d == 0 ? 0.0 : player_->AudioDelay() + (d < 0 ? -0.05 : 0.05));
      SavePlayerPrefs();
    }});
  menu_items_.push_back({"Sleep timer",
    [this] { return SleepLabel(); },
    [this, recorded](int d) {
      // "End of programme" is the live choice; a recording stops at its
      // end anyway.
      const HtspClient::NowNext nn = recorded ? HtspClient::NowNext()
					      : client_.GetNowNext(playing_channel_id_);
      const time_t now = time(nullptr);
      const bool can_end = nn.has_now && nn.now.stop > now;
      const int count = can_end ? kSleepChoiceCount : kSleepChoiceCount - 1;
      // Where the timer is now among the choices, then one step on.
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
  menu_items_.push_back({"Stop", nullptr, [this](int) { StopPlayback(); }});

  sel_menu_ = 0;
  RebuildMenuRows();
  model_.DirtyVariable("menu_title");
  model_.DirtyVariable("menu_meta");
  model_.DirtyVariable("menu_dialog");
  bind_menu_open_ = true;
  model_.DirtyVariable("menu_open");
}

// Where the recording was left, and whether to go on from there: a small
// dialog in the middle of the screen, Kodi style.
void App::OpenResumeDialog(const HtspDvrEntry& entry)
{
  const uint32_t id = entry.id;
  const int64_t at = entry.play_position;
  menu_items_.clear();
  menu_items_.push_back({"Resume from " + FmtPosition(at * 1000000), nullptr,
    [this, id, at](int) { StartRecording(id, at); }});
  menu_items_.push_back({"Play from beginning", nullptr,
    [this, id](int) { StartRecording(id, 0); }});

  bind_menu_title_ = entry.title.empty() ? "Recording" : entry.title;
  // How far, of how long, when the length is known (the scheduled length;
  // the file may run a little longer with the padding).
  const int64_t length = entry.stop - entry.start;
  bind_menu_meta_.clear();
  bind_menu_progress_ = "0%";
  if (length > at)
    {
      bind_menu_meta_ = FmtPosition(at * 1000000) + " of " + FmtPosition(length * 1000000) +
			"  ·  " + FmtPosition((length - at) * 1000000) + " left";
      bind_menu_progress_ = Percent(at, length);
    }
  bind_menu_dialog_ = true;
  model_.DirtyVariable("menu_title");
  model_.DirtyVariable("menu_meta");
  model_.DirtyVariable("menu_progress");
  model_.DirtyVariable("menu_dialog");

  sel_menu_ = 0;
  RebuildMenuRows();
  bind_menu_open_ = true;
  model_.DirtyVariable("menu_open");
}

void App::CloseMenu()
{
  bind_menu_open_ = false;
  model_.DirtyVariable("menu_open");
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
  event.StopPropagation(); // the menu is modal
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
	  // Close first: the action may replace what the menu was for.
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
    ShowWatchInfo(kWatchInfoSec);
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void App::HandleKeyWatch(Rml::Event& event, int key)
{
  const bool recorded = player_ && player_->IsRecordingPlayback();
  const keymap::Cmd cmd = keymap::Command(key);

  // Both: the playback menu and the audio key.
  switch (cmd)
    {
    case keymap::Cmd::Options:
      ShowWatchInfo(kWatchInfoSec);
      OpenWatchMenu();
      event.StopPropagation();
      return;
    case keymap::Cmd::Audio:
      CycleAudioTrack(+1);
      ShowWatchInfo(kWatchInfoSec);
      event.StopPropagation();
      return;
    default:
      break;
    }

  if (recorded)
    {
      switch (cmd)
	{
	// Left/right and the remote's rewind/fast forward step by 10 s, more
	// the more they are pressed; up/down, the shoulder buttons and the
	// remote's skip keys by 5 minutes (up is forwards, as in most media
	// players).
	case keymap::Cmd::Left:
	case keymap::Cmd::SeekBack:
	  SeekStep(-1, false);
	  break;
	case keymap::Cmd::Right:
	case keymap::Cmd::SeekForward:
	  SeekStep(+1, false);
	  break;
	case keymap::Cmd::Up:
	case keymap::Cmd::PageDown:
	case keymap::Cmd::SkipForward:
	  SeekStep(+1, true);
	  break;
	case keymap::Cmd::Down:
	case keymap::Cmd::PageUp:
	case keymap::Cmd::SkipBack:
	  SeekStep(-1, true);
	  break;
	case keymap::Cmd::Ok:        // cross
	case keymap::Cmd::PlayPause: // the remote's play/pause button
	  player_->TogglePause();
	  UpdateWatchOverlay();
	  ShowWatchInfo(kWatchInfoSec);
	  break;
	case keymap::Cmd::Secondary: // square: toggle the info bar
	  if (bind_info_visible_ && !player_->IsPaused())
	    {
	      bind_info_visible_ = false;
	      model_.DirtyVariable("info_visible");
	    }
	  else
	    ShowWatchInfo(kWatchInfoSec);
	  break;
	// Circle and the remote's guide: the menus, playing on behind them
	// (Kodi's way). Stop stops.
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
    case keymap::Cmd::Up: // zap: previous channel in the list
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
	  ShowWatchInfo(kWatchInfoSec);
	break;
      }
    // Live TV cannot be paused, so play/pause falls back to the info bar
    // rather than doing nothing at all.
    case keymap::Cmd::Ok:
    case keymap::Cmd::PlayPause:
    case keymap::Cmd::Secondary:
      if (bind_info_visible_)
	{
	  bind_info_visible_ = false;
	  model_.DirtyVariable("info_visible");
	}
      else
	ShowWatchInfo(kWatchInfoSec);
      break;
    // Left, circle and the remote's guide: the channel list, the channel
    // playing on behind it (Kodi's way). Cross on it there comes back; on
    // another channel, tunes to that. Stop stops.
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
