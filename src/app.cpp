// The application shell: the data model, the per-frame update, switching
// views, and routing keys. The views are in app_connect.cpp, app_guide.cpp,
// app_recordings.cpp and app_watch.cpp.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "app.h"
#include "app_internal.h"
#include "clock.h"
#include "format.h"
#include "gamepad.h"
#include "icons.h"
#include "keymap.h"

using namespace appdetail;


App::App() : player_(std::make_unique<Player>()) {}

App::~App() = default;

bool App::Initialize(Rml::Context* context, const Options& options, std::string& error) {
  config_path_ = options.config_path;
  icons_.SetRoot(options.cache_dir.empty() ? std::string() : options.cache_dir + "/icons");
  config_.Load(config_path_);
  sel_server_ = config_.LastUsed();

  if (const char* trace = std::getenv("TVHP_INPUT_LOG"); trace && *trace && *trace != '0')
    SetInputTrace(true);

  if (!SetupDataModel(context, error))
    return false;

  const std::string main_rml = options.assets_dir + "/main.rml";
  if (!(document_ = context->LoadDocument(main_rml))) {
    error = "failed to load " + main_rml;
    return false;
  }

  document_->Show();
  document_->AddEventListener(Rml::EventId::Keydown, this, true);

  // The player needs the window's GL context, current here. Without a
  // player the guide and the recordings still work.
  std::string player_error;
  if (player_->Initialize(player_error, options.fonts_dir))
    ApplyPlayerPrefs();
  else
    Rml::Log::Message(Rml::Log::LT_WARNING, "No playback: %s", player_error.c_str());

  RebuildServerRows();
  LoadFormFromSelection();

  SetZone(Zone::ServerList);
  // First run: straight to adding a server. Else to the last one used.
  if (config_.Empty())
    OpenServerDialog(true);
  else
    ScheduleReconnect(config_.LastUsed(), 0.0);

  return true;
}

void App::Shutdown() {
  gamepad::SetTrace(nullptr); // it calls back into this

  if (connect_thread_.joinable())
    connect_thread_.join();

  icons_.Stop();
  if (player_) {
    // While the server is still connected.
    SaveRecordingPosition();
    player_->Stop(client_);
    player_->Shutdown();
  }

  client_.Disconnect();

  if (document_) {
    document_->RemoveEventListener(Rml::EventId::Keydown, this, true);
    document_->Close();
    document_ = nullptr;
  }
}

bool App::SetupDataModel(Rml::Context* context, std::string& error) {
  Rml::DataModelConstructor ctor = context->CreateDataModel("tv");
  if (!ctor) {
    error = "failed to create data model";
    return false;
  }

  if (auto row = ctor.RegisterStruct<ServerRow>()) {
    row.RegisterMember("name", &ServerRow::name);
    row.RegisterMember("detail", &ServerRow::detail);
  }
  ctor.RegisterArray<std::vector<ServerRow>>();

  if (auto row = ctor.RegisterStruct<ChannelRow>()) {
    row.RegisterMember("number", &ChannelRow::number);
    row.RegisterMember("name", &ChannelRow::name);
    row.RegisterMember("now_title", &ChannelRow::now_title);
    row.RegisterMember("now_time", &ChannelRow::now_time);
    row.RegisterMember("progress_style", &ChannelRow::progress_style);
    row.RegisterMember("has_now", &ChannelRow::has_now);
    row.RegisterMember("icon", &ChannelRow::icon);
    row.RegisterMember("playing", &ChannelRow::playing);
  }
  ctor.RegisterArray<std::vector<ChannelRow>>();

  if (auto row = ctor.RegisterStruct<EpgRow>()) {
    row.RegisterMember("time", &EpgRow::time);
    row.RegisterMember("day", &EpgRow::day);
    row.RegisterMember("title", &EpgRow::title);
    row.RegisterMember("rec", &EpgRow::rec);
    row.RegisterMember("now", &EpgRow::now);
    row.RegisterMember("recording", &EpgRow::recording);
  }
  ctor.RegisterArray<std::vector<EpgRow>>();

  if (auto row = ctor.RegisterStruct<RecRow>()) {
    row.RegisterMember("title", &RecRow::title);
    row.RegisterMember("meta", &RecRow::meta);
    row.RegisterMember("state", &RecRow::state);
    row.RegisterMember("pending", &RecRow::pending);
    row.RegisterMember("failed", &RecRow::failed);
    row.RegisterMember("progress", &RecRow::progress);
    row.RegisterMember("icon", &RecRow::icon);
    row.RegisterMember("resumable", &RecRow::resumable);
    row.RegisterMember("watched", &RecRow::watched);
    row.RegisterMember("playing", &RecRow::playing);
  }
  ctor.RegisterArray<std::vector<RecRow>>();

  if (auto row = ctor.RegisterStruct<MenuRow>()) {
    row.RegisterMember("label", &MenuRow::label);
    row.RegisterMember("value", &MenuRow::value);
  }
  ctor.RegisterArray<std::vector<MenuRow>>();

  ctor.Bind("view", &bind_view_);
  ctor.Bind("section", &bind_section_);
  ctor.Bind("zone", &bind_zone_);
  ctor.Bind("dialog", &bind_dialog_);
  ctor.Bind("dialog_title", &bind_dialog_title_);
  ctor.Bind("status", &bind_status_);
  ctor.Bind("clock", &bind_clock_);
  ctor.Bind("date", &bind_date_);
  ctor.Bind("toast", &bind_toast_);
  ctor.Bind("toast_icon", &bind_toast_icon_);
  ctor.Bind("fullscreen", &bind_fullscreen_);
  ctor.Bind("servers", &server_rows_);
  ctor.Bind("sel_server", &sel_server_);
  ctor.Bind("server_count", &server_count_);
  ctor.Bind("name", &cfg_name_);
  ctor.Bind("host", &cfg_host_);
  ctor.Bind("port", &cfg_port_);
  ctor.Bind("user", &cfg_user_);
  ctor.Bind("pass", &cfg_pass_);
  ctor.Bind("channels", &channel_rows_);
  ctor.Bind("epg", &epg_rows_);
  ctor.Bind("recordings", &rec_rows_);
  ctor.Bind("sel_channel", &sel_channel_);
  ctor.Bind("sel_epg", &sel_epg_);
  ctor.Bind("sel_rec", &sel_rec_);
  ctor.Bind("channel_count", &channel_count_);
  ctor.Bind("rec_count", &rec_count_);
  ctor.Bind("epg_channel", &bind_epg_channel_);
  ctor.Bind("epg_icon", &bind_epg_icon_);
  ctor.Bind("watch_num", &bind_watch_num_);
  ctor.Bind("watch_icon", &bind_watch_icon_);
  ctor.Bind("detail_title", &bind_detail_title_);
  ctor.Bind("detail_meta", &bind_detail_meta_);
  ctor.Bind("detail_desc", &bind_detail_desc_);
  ctor.Bind("detail_rec", &bind_detail_rec_);
  ctor.Bind("player_status", &bind_player_status_);
  ctor.Bind("watching", &bind_watching_);
  ctor.Bind("info_visible", &bind_info_visible_);
  ctor.Bind("watch_name", &bind_watch_name_);
  ctor.Bind("watch_now", &bind_watch_now_);
  ctor.Bind("watch_time", &bind_watch_time_);
  ctor.Bind("watch_progress", &bind_watch_progress_);
  ctor.Bind("watch_recorded", &bind_watch_recorded_);
  ctor.Bind("watch_paused", &bind_watch_paused_);
  ctor.Bind("watch_ends", &bind_watch_ends_);
  ctor.Bind("watch_seek", &bind_watch_seek_);
  ctor.Bind("watch_seek_at", &bind_watch_seek_at_);
  ctor.Bind("watch_vtrack", &bind_watch_vtrack_);
  ctor.Bind("watch_atrack", &bind_watch_atrack_);
  ctor.Bind("watch_strack", &bind_watch_strack_);
  ctor.Bind("watch_speed", &bind_watch_speed_);
  ctor.Bind("watch_sleep", &bind_watch_sleep_);
  ctor.Bind("menu_open", &bind_menu_open_);
  ctor.Bind("menu_dialog", &bind_menu_dialog_);
  ctor.Bind("menu_title", &bind_menu_title_);
  ctor.Bind("menu_meta", &bind_menu_meta_);
  ctor.Bind("menu_progress", &bind_menu_progress_);
  ctor.Bind("menu", &menu_rows_);
  ctor.Bind("sel_menu", &sel_menu_);

  ctor.BindEventCallback("save", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    SaveServer();
  });
  ctor.BindEventCallback("cancel", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    CloseServerDialog();
  });
  ctor.BindEventCallback("select_server", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    const int index = args[0].Get<int>();
    SetZone(Zone::ServerList);
    CancelReconnect();
    if (index == sel_server_) {
      StartConnect(); // a second click connects
      return;
    }
    sel_server_ = index;
    scroll_server_pending_ = true;
    LoadFormFromSelection();
    model_.DirtyVariable("sel_server");
  });
  ctor.BindEventCallback("select_channel", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    const int index = args[0].Get<int>();
    SetZone(Zone::Channels);
    if (index == sel_channel_)
      {
	ActivateSelection(); // a second click watches
	return;
      }
    sel_channel_ = index;
    scroll_channels_pending_ = true;
    RebuildEpgRows();
    RebuildDetail();
    model_.DirtyVariable("sel_channel");
  });
  ctor.BindEventCallback("select_epg", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    SetZone(Zone::Epg);
    sel_epg_ = args[0].Get<int>();
    scroll_epg_pending_ = true;
    RebuildDetail();
    model_.DirtyVariable("sel_epg");
  });
  ctor.BindEventCallback("select_section", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty() || bind_menu_open_)
      return;
    SetSection(args[0].Get<Rml::String>() == "recordings" ? Section::Recordings : Section::Channels);
  });
  ctor.BindEventCallback("select_rec", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    const int index = args[0].Get<int>();
    SetZone(Zone::Recordings);
    if (index == sel_rec_)
      {
	ActivateSelection();
	return;
      }
    sel_rec_ = index;
    scroll_rec_pending_ = true;
    RebuildRecordingDetail();
    model_.DirtyVariable("sel_rec");
  });
  model_ = ctor.GetModelHandle();
  return true;
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

void App::SwitchToMain() {
  view_ = View::Main;
  bind_view_ = "main";
  bind_status_.clear();
  section_ = Section::Channels;
  bind_section_ = "channels";
  zone_ = Zone::Channels;
  bind_zone_ = "channels";
  model_.DirtyVariable("view");
  model_.DirtyVariable("status");
  model_.DirtyVariable("section");
  model_.DirtyVariable("zone");
  RefreshFromClient(true);
}

// Safe to call when already disconnected.
void App::DisconnectToLogin() {
  CancelReconnect();
  resume_channel_id_ = 0;
  StopPlayback();
  icons_.Stop();
  client_.Disconnect();
  seen_generation_ = 0;
  channels_.clear();
  epg_events_.clear();
  recordings_.clear();
  channel_rows_.clear();
  epg_rows_.clear();
  rec_rows_.clear();
  selected_channel_id_ = 0;
  selected_dvr_id_ = 0;
  model_.DirtyVariable("channels");
  model_.DirtyVariable("epg");
  model_.DirtyVariable("recordings");
  SwitchToConnect({});
}

void App::SwitchToConnect(const std::string& status) {
  StopPlayback();
  icons_.Stop(); // before the next Connect() reuses the client
  view_ = View::Connect;
  bind_view_ = "connect";
  bind_status_ = status;
  model_.DirtyVariable("view");
  model_.DirtyVariable("status");
  RebuildServerRows();
  CloseServerDialog();
  SetZone(Zone::ServerList);
}

void App::SetSection(Section section) {
  if (section_ == section)
    return;
  section_ = section;
  bind_section_ = (section == Section::Channels) ? "channels" : "recordings";
  zone_ = (section == Section::Channels) ? Zone::Channels : Zone::Recordings;
  bind_zone_ = (section == Section::Channels) ? "channels" : "recordings";
  model_.DirtyVariable("section");
  model_.DirtyVariable("zone");

  if (section == Section::Recordings) {
    RebuildRecordingRows();
    RebuildRecordingDetail();
    scroll_rec_pending_ = true;
  } else {
    RebuildEpgRows();
    RebuildDetail();
    scroll_channels_pending_ = true;
  }
}

void App::Update() {
  PollConnectState();

  {
    const time_t now = (time_t)tvclock::Now();
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[8] = {};
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (bind_clock_ != buf) {
      bind_clock_ = buf;
      model_.DirtyVariable("clock");
    }
    // Its own check: the server's clock may move the date and not the time.
    if (tm.tm_yday != clock_yday_) {
      clock_yday_ = tm.tm_yday;
      Set(bind_date_, Rml::String(textfmt::FmtLongDate(now)), "date");
    }
  }

  if (view_ == View::Main) {
    if (!client_.Connected()) {
      // Lost: back to the server list, connecting again by itself, and to
      // the channel that was on.
      const int index = connecting_index_;
      const ServerProfile* p = config_.At(index);
      if (player_ && player_->IsPlaying() && !player_->IsRecordingPlayback())
	resume_channel_id_ = playing_channel_id_;
      SwitchToConnect("Lost the connection to " + (p ? p->Label() : std::string("the server")));
      ScheduleReconnect(index, kReconnectDelaysSec[0]);
      return;
    }
    RefreshFromClient(false);
    if (icons_.Generation() != seen_icons_)
      ApplyIcons();
    // Back from a lost connection: the channel that was on, once known.
    if (resume_channel_id_ && client_.SyncCompleted()) {
      const uint32_t id = resume_channel_id_;
      resume_channel_id_ = 0;
      for (const HtspChannel& ch : channels_)
	if (ch.id == id)
	  PlayChannel(id);
    }
  } else if (auto_index_ >= 0 && connect_state_ == ConnectState::Idle && Now() >= auto_at_) {
    sel_server_ = auto_index_;
    scroll_server_pending_ = true;
    model_.DirtyVariable("sel_server");
    StartConnect();
  }

  if (bind_watching_ && player_)
    {
      // A recording played to its end: counted as watched, and back to the
      // list.
      if (player_->AtEnd())
	{
	  HtspDvrEntry entry;
	  const bool known = client_.GetDvrEntry(playing_dvr_id_, &entry);
	  SaveRecordingPosition(true);
	  playing_dvr_id_ = 0; // saved; StopPlayback() has nothing to add
	  StopPlayback();
	  ShowToast(known && !entry.title.empty() ? "Finished: " + entry.title : "Finished",
		    icon::kDone);
	  return;
	}
      // Now and then, in case the app is closed without stopping.
      if (playing_dvr_id_ && !player_->IsPaused() &&
	  Now() - position_saved_at_ > kPositionSaveSec)
	{
	  position_saved_at_ = Now();
	  SaveRecordingPosition();
	}
    }
  UpdateSleepTimer();

  // Behind the menus, kept current for the way back to full screen.
  if (VideoBehind())
    UpdateWatchOverlay();

  // The info bar hides by itself, except over a paused recording or the
  // playback menu.
  if (bind_watching_ && bind_fullscreen_ && bind_info_visible_)
    {
      UpdateWatchOverlay();
      if (Now() > info_deadline_ && !bind_menu_open_ && !(player_ && player_->IsPaused()))
	HideWatchInfo();
    }
  // Menu values follow what changes under them (tracks announced late, the
  // sleep timer counting down).
  if (bind_menu_open_ && !bind_menu_dialog_)
    {
      for (size_t i = 0; i < menu_items_.size() && i < menu_rows_.size(); i++)
	{
	  const Rml::String value = menu_items_[i].value ? menu_items_[i].value() : "";
	  Set(menu_rows_[i].value, value, "menu");
	}
    }

  if (Now() > toast_deadline_)
    Set(bind_toast_, Rml::String(), "toast");

  // Scrolling waits for the frame's update; it uses the previous layout,
  // which is fine with fixed row heights.
  if (scroll_server_pending_)
    {
      EnsureRowVisible("server-list", sel_server_, kServerRowPitch);
      scroll_server_pending_ = false;
    }
  if (scroll_channels_pending_)
    {
      EnsureRowVisible("channel-list", sel_channel_, kChannelRowPitch);
      scroll_channels_pending_ = false;
    }
  if (scroll_epg_pending_)
    {
      EnsureRowVisible("epg-list", sel_epg_, kEpgRowPitch);
      scroll_epg_pending_ = false;
    }
  if (scroll_rec_pending_)
    {
      EnsureRowVisible("rec-list", sel_rec_, kRecRowPitch);
      scroll_rec_pending_ = false;
    }
  if (scroll_menu_pending_)
    {
      EnsureRowVisible("menu-rows", sel_menu_, kMenuRowPitch);
      scroll_menu_pending_ = false;
    }
}

void App::RenderVideo(int width, int height)
{
  if (player_)
    player_->RenderVideo(width, height);
}

void App::RefreshFromClient(bool force)
{
  const uint64_t gen = client_.Generation();
  const double now = Now();
  if (!force && gen == seen_generation_ && now - last_refresh_time_ < kRefreshIntervalSec)
    return;
  seen_generation_ = gen;
  last_refresh_time_ = now;

  if (section_ == Section::Channels)
    {
      RebuildChannelRows();
      RebuildEpgRows();
      RebuildDetail();
    }
  else
    {
      RebuildRecordingRows();
      RebuildRecordingDetail();
    }

  Set(bind_player_status_, player_ ? Rml::String(player_->StatusText()) : Rml::String(),
      "player_status");
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void App::SetZone(Zone zone)
{
  if (zone_ == zone)
    return;
  zone_ = zone;
  switch (zone)
    {
    case Zone::Channels:   bind_zone_ = "channels"; break;
    case Zone::Epg:        bind_zone_ = "epg"; break;
    case Zone::Recordings: bind_zone_ = "recordings"; break;
    case Zone::Tabs:       bind_zone_ = "tabs"; break;
    default:               bind_zone_ = "channels"; break;
    }
  model_.DirtyVariable("zone");

  if (zone == Zone::Channels || zone == Zone::Epg)
    RebuildDetail();
}

bool App::HandleKeyTabs(keymap::Cmd cmd)
{
  switch (cmd)
    {
    case keymap::Cmd::Left:
    case keymap::Cmd::TabPrev:
      ShowSection(Section::Channels);
      return true;
    case keymap::Cmd::Right:
    case keymap::Cmd::TabNext:
      ShowSection(Section::Recordings);
      return true;
    case keymap::Cmd::Secondary:
      ShowSection(section_ == Section::Channels ? Section::Recordings : Section::Channels);
      return true;
    case keymap::Cmd::Down:
    case keymap::Cmd::Ok:
      LeaveTabs();
      return true;
    // Nothing above the tabs, and nothing selected to act on.
    case keymap::Cmd::Up:
    case keymap::Cmd::PageUp:
    case keymap::Cmd::PageDown:
    case keymap::Cmd::Home:
    case keymap::Cmd::End:
    case keymap::Cmd::PlayPause:
    case keymap::Cmd::Remove:
      return true;
    default:
      return false;
    }
}

bool App::AtTopOfList() const
{
  switch (zone_)
    {
    case Zone::Channels:   return sel_channel_ <= 0 || channel_rows_.empty();
    case Zone::Epg:        return sel_epg_ <= 0 || epg_rows_.empty();
    case Zone::Recordings: return sel_rec_ <= 0 || rec_rows_.empty();
    default:               return false;
    }
}

void App::EnterTabs()
{
  tabs_return_ = zone_;
  SetZone(Zone::Tabs);
}

// Back into the section shown, where it was left if it is the same one.
void App::LeaveTabs()
{
  if (section_ == Section::Recordings)
    SetZone(Zone::Recordings);
  else if (tabs_return_ == Zone::Epg && !epg_rows_.empty())
    {
      SetZone(Zone::Epg);
      scroll_epg_pending_ = true;
      model_.DirtyVariable("sel_epg");
    }
  else
    SetZone(Zone::Channels);
}

// Switches section without leaving the tabs when they have the focus.
void App::ShowSection(Section section)
{
  if (section == section_)
    return;
  const bool on_tabs = zone_ == Zone::Tabs;
  SetSection(section);
  tabs_return_ = zone_;
  if (on_tabs)
    SetZone(Zone::Tabs);
}

void App::MoveSelection(int delta)
{
  switch (zone_)
    {
    case Zone::ServerList:
      {
	// The servers, and "Add a server" after them.
	const int next = StepIndex(sel_server_, delta, (int)server_rows_.size() + 1);
	if (next == sel_server_)
	  return;
	sel_server_ = next;
	scroll_server_pending_ = true;
	model_.DirtyVariable("sel_server");
	LoadFormFromSelection();
	break;
      }
    case Zone::Channels:
      {
	if (channel_rows_.empty())
	  return;
	const int next = StepIndex(sel_channel_, delta, (int)channel_rows_.size());
	if (next == sel_channel_)
	  return;
	sel_channel_ = next;
	sel_epg_ = 0;
	scroll_channels_pending_ = true;
	model_.DirtyVariable("sel_channel");
	RebuildEpgRows();
	RebuildDetail();
	break;
      }
    case Zone::Epg:
      {
	if (epg_rows_.empty())
	  return;
	const int next = StepIndex(sel_epg_, delta, (int)epg_rows_.size());
	if (next == sel_epg_)
	  return;
	sel_epg_ = next;
	scroll_epg_pending_ = true;
	model_.DirtyVariable("sel_epg");
	RebuildDetail();
	break;
      }
    case Zone::Recordings:
      {
	if (rec_rows_.empty())
	  return;
	const int next = StepIndex(sel_rec_, delta, (int)rec_rows_.size());
	if (next == sel_rec_)
	  return;
	sel_rec_ = next;
	scroll_rec_pending_ = true;
	model_.DirtyVariable("sel_rec");
	RebuildRecordingDetail();
	break;
      }
    default:
      break;
    }
}

void App::ActivateSelection()
{
  switch (zone_)
    {
    case Zone::Channels:
      if (sel_channel_ >= 0 && sel_channel_ < (int)channels_.size())
	{
	  // The channel that plays behind the menus: back to it, not tuned
	  // in again.
	  if (VideoBehind() && channels_[sel_channel_].id == playing_channel_id_)
	    SetFullscreen(true);
	  else
	    PlayChannel(channels_[sel_channel_].id);
	}
      break;
    case Zone::Epg:
      ToggleRecordSelectedEvent();
      break;
    case Zone::Recordings:
      PlaySelectedRecording();
      break;
    default:
      break;
    }
}


void App::EnsureRowVisible(const char* list_id, int index, float row_pitch)
{
  Rml::Element* list = document_ ? document_->GetElementById(list_id) : nullptr;
  if (!list || index < 0)
    return;

  const float view_h = list->GetClientHeight();
  if (view_h <= 0.0f)
    return;

  // The selected row in the middle, where the list allows.
  float target = index * row_pitch - (view_h - row_pitch) * 0.5f;
  const float max_scroll = std::max(0.0f, list->GetScrollHeight() - view_h);
  target = std::clamp(target, 0.0f, max_scroll);
  list->SetScrollTop(target);
}

void App::ShowToast(const std::string& text, const char* icon)
{
  bind_toast_ = text;
  bind_toast_icon_ = icon ? icon : "";
  toast_deadline_ = Now() + kToastSec;
  model_.DirtyVariable("toast");
  model_.DirtyVariable("toast_icon");
}

// Every input event SDL delivers goes to the log, and each press to a toast
// as well, whether anything reacts to it or not: what a TV remote's buttons
// arrive as (over HDMI-CEC, say) is otherwise anyone's guess.
void App::SetInputTrace(bool on)
{
  input_trace_ = on;
  if (!on)
    {
      gamepad::SetTrace(nullptr);
      return;
    }
  gamepad::SetTrace([this](const std::string& line, bool press) {
    SDL_Log("input: %s", line.c_str());
    if (press)
      ShowToast(line, icon::kGamepad);
  });
}

App::MenuItem App::InputTraceItem()
{
  return {"Show input",
	  [this] { return std::string(input_trace_ ? "On" : "Off"); },
	  [this](int) { SetInputTrace(!input_trace_); }};
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void App::ProcessEvent(Rml::Event& event)
{
  if (event.GetId() != Rml::EventId::Keydown)
    return;

  const int key = event.GetParameter<int>("key_identifier", 0);
  if (bind_menu_open_)
    HandleKeyMenu(event, key);
  else if (view_ == View::Connect)
    HandleKeyConnect(event, key);
  else
    HandleKeyMain(event, key);
}


void App::HandleKeyMain(Rml::Event& event, int key)
{
  if (bind_watching_ && bind_fullscreen_)
    {
      HandleKeyWatch(event, key);
      return;
    }

  // Over a video behind the menus, a few keys are about it.
  if (VideoBehind())
    {
      switch (keymap::Command(key))
	{
	case keymap::Cmd::Options: // the playback menu, with "Full screen"
	  OpenWatchMenu();
	  event.StopPropagation();
	  return;
	case keymap::Cmd::Audio:
	  CycleAudioTrack(+1);
	  event.StopPropagation();
	  return;
	case keymap::Cmd::Subtitles:
	  CycleSubtitleTrack(+1);
	  event.StopPropagation();
	  return;
	case keymap::Cmd::PlayPause:
	  // A recording pauses; on live TV the key plays what is selected.
	  if (player_->IsRecordingPlayback())
	    {
	      const bool paused = player_->TogglePause();
	      ShowToast(paused ? "Paused" : "Playing", paused ? icon::kPause : icon::kPlay);
	      event.StopPropagation();
	      return;
	    }
	  break;
	case keymap::Cmd::SeekBack:
	case keymap::Cmd::SeekForward:
	  if (player_->CanSeek())
	    {
	      const bool back = keymap::Command(key) == keymap::Cmd::SeekBack;
	      player_->SeekRelative((back ? -10 : 10) * 1000000LL);
	      ShowToast(back ? "-10 s" : "+10 s", back ? icon::kRewind : icon::kForward);
	    }
	  event.StopPropagation();
	  return;
	default:
	  break;
	}
    }

  if (zone_ == Zone::Tabs && HandleKeyTabs(keymap::Command(key)))
    {
      event.StopPropagation();
      return;
    }

  switch (keymap::Command(key))
    {
    // Up from the top of a list goes onto the tabs, as on the PS4's own
    // screens; a TV remote has no L1 / R1.
    case keymap::Cmd::Up:
      if (AtTopOfList())
	EnterTabs();
      else
	MoveSelection(-1);
      break;
    case keymap::Cmd::Down:
      MoveSelection(+1);
      break;
    case keymap::Cmd::TabPrev:
      SetSection(Section::Channels);
      break;
    case keymap::Cmd::TabNext:
      SetSection(Section::Recordings);
      break;
    case keymap::Cmd::PageUp: // L2 / "p+"
      MoveSelection(-8);
      break;
    case keymap::Cmd::PageDown: // R2 / "p-"
      MoveSelection(+8);
      break;
    case keymap::Cmd::Home:
      MoveSelection(-1000000);
      break;
    case keymap::Cmd::End:
      MoveSelection(+1000000);
      break;
    case keymap::Cmd::Right:
      if (section_ == Section::Channels && !epg_rows_.empty())
	{
	  SetZone(Zone::Epg);
	  scroll_epg_pending_ = true;
	  model_.DirtyVariable("sel_epg");
	}
      break;
    case keymap::Cmd::Left:
      if (zone_ == Zone::Epg)
	SetZone(Zone::Channels);
      break;

    case keymap::Cmd::Secondary:
      SetSection(section_ == Section::Channels ? Section::Recordings : Section::Channels);
      break;

    // The remote's Guide key: the channels, then into their programmes.
    case keymap::Cmd::Guide:
      if (section_ != Section::Channels)
	SetSection(Section::Channels);
      else if (zone_ == Zone::Channels && !epg_rows_.empty())
	{
	  SetZone(Zone::Epg);
	  scroll_epg_pending_ = true;
	  model_.DirtyVariable("sel_epg");
	}
      break;

    case keymap::Cmd::Options:
      OpenOptionsMenu();
      break;

    // Triangle: Options > Delete, in the recordings only.
    case keymap::Cmd::Remove:
      if (section_ != Section::Recordings)
	return;
      RemoveSelectedRecording();
      break;

    // Back goes up a level, and back to a video behind the menus. It never
    // stops playback or disconnects: Stop, and Options, do.
    case keymap::Cmd::Back:
      if (zone_ == Zone::Epg)
	SetZone(Zone::Channels);
      else if (zone_ == Zone::Tabs)
	LeaveTabs();
      else if (VideoBehind())
	SetFullscreen(true);
      else if (section_ == Section::Recordings)
	SetSection(Section::Channels);
      break;

    case keymap::Cmd::Stop:
      if (!player_ || !player_->IsPlaying())
	return;
      StopPlayback();
      ShowToast("Playback stopped", icon::kStop);
      break;

    // Play plays what is selected; in the programmes, where cross records,
    // it does nothing.
    case keymap::Cmd::PlayPause:
      if (zone_ == Zone::Epg)
	return;
      ActivateSelection();
      break;

    case keymap::Cmd::Ok:
      ActivateSelection();
      break;

    default:
      return; // not ours: let it through
    }
  event.StopPropagation();
}

