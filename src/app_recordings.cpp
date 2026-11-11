// The recordings: their rows and details, and recording, cancelling,
// deleting and playing them.
#include <algorithm>
#include <cstdio>

#include "app.h"
#include "app_internal.h"
#include "icons.h"
#include "format.h"

using namespace appdetail;
using namespace textfmt;

void App::RebuildRecordingRows()
{
  recordings_ = client_.GetDvrEntries();
  rec_count_ = (int)recordings_.size();

  // The same recording stays selected.
  if (selected_dvr_id_)
    {
      for (size_t i = 0; i < recordings_.size(); ++i)
	if (recordings_[i].id == selected_dvr_id_)
	  {
	    sel_rec_ = (int)i;
	    break;
	  }
    }
  sel_rec_ = std::clamp(sel_rec_, 0, std::max(0, rec_count_ - 1));
  selected_dvr_id_ = recordings_.empty() ? 0 : recordings_[sel_rec_].id;

  rec_rows_.clear();
  rec_rows_.reserve(recordings_.size());
  for (const HtspDvrEntry& e : recordings_)
    {
      RecRow row;
      row.title = e.title.empty() ? "Untitled recording" : e.title;

      if (!e.channel_name.empty())
	row.meta = e.channel_name + "  ·  ";
      row.meta += FmtDay(e.start) + " " + FmtHM(e.start);
      const std::string episode = FmtEpisode(e.season, e.episode);
      if (!episode.empty())
	row.meta += "  ·  " + episode;
      row.state = DvrStateLabel(e);
      row.pending = e.IsPending();
      row.failed = !e.error.empty() || e.state == "missed" || e.state == "invalid";
      // How far it was watched, of its scheduled length.
      const int64_t length = e.stop - e.start;
      row.resumable = e.HasFile() && e.play_position >= kMinResumeSec && length > 0;
      if (row.resumable)
	{
	  char pct[16];
	  std::snprintf(pct, sizeof(pct), "%.1f%%",
			std::min(100.0, e.play_position * 100.0 / (double)length));
	  row.progress = pct;
	}
      row.watched = e.HasFile() && e.play_count > 0 && !row.resumable;
      row.playing = bind_watching_ && e.id == playing_dvr_id_;
      row.icon = IconDecorator(e.channel_id);
      rec_rows_.push_back(std::move(row));
    }

  model_.DirtyVariable("recordings");
  model_.DirtyVariable("rec_count");
  model_.DirtyVariable("sel_rec");
}

const HtspDvrEntry* App::SelectedRecording() const
{
  if (sel_rec_ < 0 || sel_rec_ >= (int)recordings_.size())
    return nullptr;
  return &recordings_[sel_rec_];
}

void App::RebuildRecordingDetail()
{
  bind_detail_title_.clear();
  bind_detail_meta_.clear();
  bind_detail_desc_.clear();
  bind_detail_rec_.clear();

  const HtspDvrEntry* e = SelectedRecording();
  if (!e)
    {
      bind_detail_title_ = client_.SyncCompleted() ? "No recordings" : "Loading recordings...";
      bind_detail_desc_ = client_.SyncCompleted()
	? "Recordings you schedule from the guide show up here."
	: "Fetching the recording list from the server.";
    }
  else
    {
      selected_dvr_id_ = e->id;
      bind_detail_title_ = e->title.empty() ? "Untitled recording" : e->title;

      if (!e->channel_name.empty())
	bind_detail_meta_ = e->channel_name + "  ·  ";
      bind_detail_meta_ += FmtDay(e->start) + "  ·  " + FmtRange(e->start, e->stop) + "  ·  " +
			   FmtDuration(e->start, e->stop);
      if (e->data_size > 0)
	bind_detail_meta_ += "  ·  " + std::to_string(e->data_size / (1024 * 1024)) + " MB";

      bind_detail_desc_ = Synopsis(e->title, e->subtitle, e->description, e->summary);
      if (bind_detail_desc_.empty())
	bind_detail_desc_ = "No description available.";

      bind_detail_rec_ = DvrStateLabel(*e);
      if (e->HasFile() && e->play_position >= kMinResumeSec)
	bind_detail_rec_ += "  ·  Resume from " + FmtPosition(e->play_position * 1000000LL);
      else if (e->HasFile() && e->play_count > 0)
	bind_detail_rec_ += "  ·  Watched";
    }

  model_.DirtyVariable("detail_title");
  model_.DirtyVariable("detail_meta");
  model_.DirtyVariable("detail_desc");
  model_.DirtyVariable("detail_rec");
}

void App::ToggleRecordSelectedEvent()
{
  if (sel_epg_ < 0 || sel_epg_ >= (int)epg_events_.size())
    return;
  const HtspEvent& ev = epg_events_[sel_epg_];

  HtspDvrEntry entry;
  const bool have = client_.GetDvrEntryForEvent(ev.id, &entry);

  if (have && entry.IsPending())
    {
      CancelOrDeleteRecording(entry, false);
      return;
    }
  if (have && entry.IsFinished())
    {
      ShowToast("Already recorded: see Recordings", icon::kFilm);
      return;
    }

  std::string error;
  if (!client_.RecordEvent(ev.id, error))
    {
      ShowToast(error.empty() ? "The server refused to schedule it" : error, icon::kWarning);
      return;
    }

  const std::string title = ev.title.empty() ? std::string("programme") : ev.title;
  ShowToast("Recording scheduled: " + title, icon::kScheduled);
  // The server's dvrEntryAdd follows; the badge shows at once.
  RefreshFromClient(true);
}

// Deleting removes the file; cancelling unschedules or stops a recording
// and keeps what was recorded. Both ask first.
void App::CancelOrDeleteRecording(const HtspDvrEntry& entry, bool destructive)
{
  const uint32_t id = entry.id;
  const std::string title = entry.title.empty() ? std::string("this recording") : entry.title;
  std::string question, action;
  if (destructive)
    {
      question = "Delete " + title + "?";
      action = "Delete";
    }
  else if (entry.IsRecording())
    {
      question = "Stop recording " + title + "?";
      action = "Stop recording";
    }
  else
    {
      question = "Cancel the recording of " + title + "?";
      action = "Cancel recording";
    }
  OpenConfirm(question, destructive ? "The file is removed from the server."
				    : std::string(), action,
    [this, id, destructive] {
      std::string error;
      const bool ok = destructive ? client_.DeleteDvrEntry(id, error)
				  : client_.CancelDvrEntry(id, error);
      if (!ok)
	{
	  ShowToast(error.empty() ? "The server refused the request" : error, icon::kWarning);
	  return;
	}
      if (playing_dvr_id_ == id)
	StopPlayback();
      ShowToast(destructive ? "Recording deleted" : "Recording cancelled",
		destructive ? icon::kDelete : icon::kUnschedule);
      RefreshFromClient(true);
    });
}

void App::PlaySelectedRecording()
{
  const HtspDvrEntry* e = SelectedRecording();
  if (!e)
    return;
  // The recording that plays behind the menus: back to it.
  if (VideoBehind() && e->id == playing_dvr_id_)
    {
      SetFullscreen(true);
      return;
    }
  if (!e->HasFile())
    {
      ShowToast(e->IsScheduled() ? "This recording hasn't started yet"
				 : "There is no file for this recording", icon::kWarning);
      return;
    }
  if (!client_.SupportsFileApi())
    {
      ShowToast("This server is too old to stream recordings over HTSP", icon::kWarning);
      return;
    }

  // Left part-way, here or on another client: resume?
  if (client_.SupportsPlayPosition() && e->play_position >= kMinResumeSec)
    {
      OpenResumeDialog(*e);
      return;
    }
  StartRecording(e->id, 0);
}

void App::RemoveSelectedRecording()
{
  const HtspDvrEntry* e = SelectedRecording();
  if (!e)
    return;
  CancelOrDeleteRecording(*e, !e->IsPending());
}
