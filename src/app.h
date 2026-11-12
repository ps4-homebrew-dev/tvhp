#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <RmlUi/Core.h>

#include "config.h"
#include "htsp.h"
#include "iconcache.h"
#include "keymap.h"
#include "player.h"

// Owns the HTSP client, the player and the RmlUi document, and mediates
// between them. Everything here runs on the main thread; what the HTSP
// reader thread changes is picked up by polling client_.Generation() in
// Update().
class App : public Rml::EventListener {
public:
  App();
  ~App() override;

  struct Options {
    std::string config_path = "tvhp.cfg";
    std::string assets_dir = "assets";
    std::string fonts_dir = "fonts";
    std::string cache_dir;  // channel icons; "" = not kept
  };

  // Creates the data model and loads main.rml. Call before the first
  // context update.
  bool Initialize(Rml::Context* context, const Options& options, std::string& error);
  void Shutdown();

  // Per-frame housekeeping; call before context->Update().
  void Update();

  // Draws the video beneath the UI; call between Backend::BeginFrame() and
  // context->Render().
  void RenderVideo(int width, int height);

  // Rml::EventListener: every keydown, before the document sees it.
  void ProcessEvent(Rml::Event& event) override;

private:
  enum class View { Connect, Main };
  // The main view shows either the channel guide or the recordings library,
  // as two tabs along the top: Up from the top of a list onto them and
  // Left / Right there, or L1 / R1, or Square.
  enum class Section { Channels, Recordings };
  enum class Zone {
    ServerList, // connect view: saved servers
    ServerForm, // connect view: the edit fields
    Channels,   // main view: channel list
    Epg,        // main view: programme guide
    Recordings, // main view: recordings list
    Tabs,       // main view: the Guide / Recordings tabs
  };

  // Rows of the data model's lists.
  struct ServerRow {
    Rml::String name;
    Rml::String detail;
  };
  struct ChannelRow {
    Rml::String number;
    Rml::String name;
    Rml::String now_title;
    Rml::String now_time;
    Rml::String progress_style; // "42%", bound via data-style-width
    Rml::String icon = "none";  // the logo, as a decorator (IconDecorator)
    bool has_now = false;
    bool playing = false; // the channel that plays (behind the menus)
  };
  struct EpgRow {
    Rml::String time;  // "20:00 - 21:30"
    Rml::String day;   // "Today", "Tomorrow", "Wed 15 Jul"
    Rml::String title;
    Rml::String rec;   // "REC", "SCHEDULED" or empty
    bool now = false;  // currently airing
    bool recording = false;
  };
  struct RecRow {
    Rml::String title;
    Rml::String meta;    // "SVT1  -  Today 20:00"
    Rml::String state;   // "Recording", "Scheduled", "Failed", duration...
    Rml::String progress = "0%"; // how far it was watched; data-style-width
    Rml::String icon = "none";   // its channel's logo, as a decorator
    bool pending = false;
    bool failed = false;
    bool resumable = false; // left part-way: the progress bar shows
    bool watched = false;   // watched to the end at least once
    bool playing = false;   // the recording that plays (behind the menus)
  };
  // A row of the playback menu, or of a question such as "resume?".
  struct MenuRow {
    Rml::String label;
    Rml::String value; // the current setting, "" for plain actions
  };
  // An entry of a menu. 'value' reports the current setting; 'change' gets
  // -1/+1 for left/right and 0 for cross. Entries without a value are
  // actions, which close the menu.
  struct MenuItem {
    std::string label;
    std::function<std::string()> value;
    std::function<void(int)> change;
  };
  bool SetupDataModel(Rml::Context* context, std::string& error);
  // Sets a bound variable, and marks it dirty if it changed.
  template <typename T> void Set(T& variable, const T& value, const char* name) {
    if (variable == value)
      return;
    variable = value;
    model_.DirtyVariable(name);
  }

  // --- Connect screen ----------------------------------------------------
  void RebuildServerRows();
  void LoadFormFromSelection();  // saved server -> dialog fields
  // The add/edit dialog, modal over the server list: a blank server, or the
  // highlighted one.
  void OpenServerDialog(bool add_new);
  void CloseServerDialog();
  ServerProfile FormToProfile() const;
  // Saves the dialog's fields and closes it; connecting is left to the list.
  void SaveServer();
  void StartConnect();
  void PollConnectState();
  // Connecting by itself: at launch, and after a lost connection, trying
  // again ever less often until a key is pressed on the server list.
  void ScheduleReconnect(int index, double delay);
  void CancelReconnect();
  void DeleteSelectedServer();
  void OpenServerMenu();
  int CurrentFormRow(bool* in_input) const;
  void FocusFormField(int delta);  // up/down: label -> label, input -> input
  // Focuses a row's text field with its text selected, so what the PS4's
  // keyboard types replaces it (it can only append).
  bool EnterFormInput(int row);
  bool FocusFormInput();  // right: label -> input
  bool FocusFormLabel();  // left: input -> label (only at caret start)
  void HandleKeyConnect(Rml::Event& event, int key);

  // --- Main screen -------------------------------------------------------
  void SwitchToMain();
  void SwitchToConnect(const std::string& status);
  // Disconnects and goes back to the server list.
  void DisconnectToLogin();
  void SetSection(Section section);
  void RefreshFromClient(bool force);
  void RebuildChannelRows();
  void RebuildEpgRows();
  void RebuildDetail();
  void RebuildRecordingRows();
  void RebuildRecordingDetail();
  void MoveSelection(int delta);
  // The tabs: Up from the top of a list onto them, Down back.
  bool HandleKeyTabs(keymap::Cmd cmd); // false: not a tabs key
  bool AtTopOfList() const;
  void EnterTabs();
  void LeaveTabs();
  void ShowSection(Section section);
  void SetZone(Zone zone);
  void ActivateSelection();
  void PlayChannel(uint32_t channel_id);
  void StopPlayback();

  // --- Recording (DVR) ---------------------------------------------------
  void ToggleRecordSelectedEvent();
  void PlaySelectedRecording();
  // Triangle: cancels a pending recording or deletes a finished one, after
  // asking.
  void RemoveSelectedRecording();
  void CancelOrDeleteRecording(const HtspDvrEntry& entry, bool destructive);
  const HtspDvrEntry* SelectedRecording() const;

  // --- Watching ---------------------------------------------------------
  void EnterWatch();
  void ExitWatch();
  void ShowWatchInfo(); // for kWatchInfoSec
  void HideWatchInfo();
  void UpdateWatchOverlay();
  void EnsureRowVisible(const char* list_id, int index, float row_pitch);
  // A notice for a few seconds, with an icon (src/icons.h) in front.
  void ShowToast(const std::string& text, const char* icon = nullptr);

  // Back from full screen leaves the video playing behind the menus, under a
  // shade, as in Kodi; Back again, or selecting what plays, brings it back.
  void SetFullscreen(bool on);
  bool VideoBehind() const { return bind_watching_ && !bind_fullscreen_; }
  // Marks the list rows of what plays.
  void MarkPlayingRows();

  // --- Channel logos -------------------------------------------------------
  // A channel's logo as a decorator ("image(...) contain"), or "none" while
  // IconCache has none.
  Rml::String IconDecorator(uint32_t channel_id);
  // Logos have arrived: show them.
  void ApplyIcons();

  // --- Playback ------------------------------------------------------------
  void StartRecording(uint32_t dvr_id, int64_t start_seconds);
  // Tells the server how far the recording that plays has got ('ended': all
  // the way), so it resumes there on any client. Sent when playback stops
  // and every kPositionSaveSec, unless nothing has changed.
  void SaveRecordingPosition(bool ended = false);
  // A seek step; steps grow while pressed in quick succession, and a bubble
  // says how far the run has gone.
  void SeekStep(int sign, bool large);
  void CycleAudioTrack(int direction);
  void CycleSubtitleTrack(int direction);
  // A track picked by hand: its language is preferred from then on, so
  // subtitles stay on (or off) from one channel to the next.
  void PickAudioTrack(const TrackInfo& track);
  void PickSubtitleTrack(const TrackInfo* track); // nullptr = off
  void ApplyPlayerPrefs();
  void SavePlayerPrefs();
  std::string SleepLabel() const;
  void UpdateSleepTimer();

  // Menus are a blade on the right (Options) or a dialog in the middle (a
  // question to answer first).
  void ShowMenu(const std::string& title, std::vector<MenuItem> items, bool dialog,
		const std::string& meta = {}, const std::string& progress = "0%");
  void OpenWatchMenu();
  void OpenResumeDialog(const HtspDvrEntry& entry);
  // Play, or Resume and Play from beginning.
  std::vector<MenuItem> PlayItems(const HtspDvrEntry& entry);
  // Options in the guide or the recordings: what can be done with what is
  // selected; over a video behind the menus, the playback menu.
  void OpenOptionsMenu();
  void OpenContextMenu(const std::string& title, std::vector<MenuItem> items);
  // "Delete X?": the action, and Cancel; 'detail' says what it means.
  void OpenConfirm(const std::string& question, const std::string& detail,
		   const std::string& action, std::function<void()> run);
  void CloseMenu();
  void RebuildMenuRows();
  void HandleKeyMenu(Rml::Event& event, int key);

  void HandleKeyMain(Rml::Event& event, int key);
  void HandleKeyWatch(Rml::Event& event, int key);

  // Bound to the data model.
  Rml::DataModelHandle model_;
  Rml::String bind_view_ = "connect";
  Rml::String bind_section_ = "channels";
  Rml::String bind_zone_ = "channels";
  Rml::String bind_dialog_;        // "", "add" or "edit"
  Rml::String bind_dialog_title_;
  Rml::String bind_status_;       // connect screen status line
  Rml::String bind_clock_;        // topbar clock
  Rml::String bind_date_;         // topbar date, "Tuesday 10 November"
  Rml::String bind_toast_icon_;
  Rml::String bind_toast_;
  Rml::String bind_epg_channel_;  // heading of the guide pane
  Rml::String bind_epg_icon_ = "none";   // its channel's logo
  Rml::String bind_watch_num_;  // the number, for a channel without a logo
  Rml::String bind_watch_icon_ = "none"; // the logo of the channel playing
  Rml::String bind_detail_title_;
  Rml::String bind_detail_meta_;
  Rml::String bind_detail_desc_;
  Rml::String bind_detail_rec_;   // "Recording scheduled"
  Rml::String bind_player_status_;
  bool bind_watching_ = false;     // something plays (full screen or behind)
  bool bind_fullscreen_ = false;   // ... full screen, the menus hidden
  bool bind_info_visible_ = false; // the info bar (hides by itself)
  Rml::String bind_watch_name_;
  Rml::String bind_watch_now_;
  Rml::String bind_watch_time_;
  Rml::String bind_watch_progress_ = "0%"; // data-style-width; never empty
  bool bind_watch_recorded_ = false;       // playing a recording, not live
  bool bind_watch_paused_ = false;
  Rml::String bind_watch_ends_;    // "Ends at 21:40"
  Rml::String bind_watch_seek_;    // seek bubble: "» +2:30"
  Rml::String bind_watch_seek_at_ = "0%"; // where it sits on the bar
  Rml::String bind_watch_vtrack_;  // "1080p · H.264"
  Rml::String bind_watch_atrack_;  // "Swedish · Dolby Digital · 5.1"
  Rml::String bind_watch_strack_;  // subtitles shown, "" when off
  Rml::String bind_watch_speed_;   // "1.5×", "" at normal speed
  Rml::String bind_watch_sleep_;   // "Sleep 25 min", "" when off

  bool bind_menu_open_ = false;
  bool bind_menu_dialog_ = false;  // a dialog rather than the blade
  Rml::String bind_menu_title_;
  Rml::String bind_menu_meta_;
  Rml::String bind_menu_progress_ = "0%"; // data-style-width; "0%" = no bar
  std::vector<MenuRow> menu_rows_;
  std::vector<MenuItem> menu_items_;
  int sel_menu_ = 0;

  std::vector<ServerRow> server_rows_;
  int sel_server_ = 0;
  int server_count_ = 0;
  Rml::String cfg_name_;
  Rml::String cfg_host_;
  Rml::String cfg_port_ = "9982";
  Rml::String cfg_user_;
  Rml::String cfg_pass_;

  std::vector<ChannelRow> channel_rows_;
  std::vector<EpgRow> epg_rows_;
  std::vector<RecRow> rec_rows_;
  int sel_channel_ = 0;
  int sel_epg_ = 0;
  int sel_rec_ = 0;
  int channel_count_ = 0;
  int rec_count_ = 0;

  // Copies of the client's state behind the rows.
  std::vector<HtspChannel> channels_;
  std::vector<HtspEvent> epg_events_;
  std::vector<HtspDvrEntry> recordings_;
  uint32_t selected_channel_id_ = 0;
  uint32_t selected_dvr_id_ = 0;

  ServerConfig config_;
  HtspClient client_;
  std::unique_ptr<Player> player_;
  IconCache icons_;
  uint64_t seen_icons_ = 0;
  std::string config_path_;
  Rml::ElementDocument* document_ = nullptr;

  View view_ = View::Connect;
  Section section_ = Section::Channels;
  Zone zone_ = Zone::ServerList;
  Zone tabs_return_ = Zone::Channels; // where Up onto the tabs came from
  uint32_t playing_channel_id_ = 0;
  uint32_t playing_dvr_id_ = 0;

  // Connecting runs on a thread of its own; connect_error_ is the worker's
  // until the state leaves Busy.
  enum class ConnectState { Idle, Busy, Success, Failed };
  std::thread connect_thread_;
  std::atomic<ConnectState> connect_state_{ConnectState::Idle};
  std::string connect_error_;
  int connecting_index_ = -1;
  int auto_index_ = -1;             // the server connected to by itself
  double auto_at_ = 0.0;            // when its next try starts
  int auto_tries_ = 0;
  uint32_t resume_channel_id_ = 0;  // live TV to tune back to, once connected

  uint64_t seen_generation_ = 0;
  double last_refresh_time_ = 0.0;
  double toast_deadline_ = 0.0;
  int clock_yday_ = -1;       // the day the date was written for
  double info_deadline_ = 0.0; // when the info bar hides
  bool scroll_channels_pending_ = false;
  bool scroll_epg_pending_ = false;
  bool scroll_rec_pending_ = false;
  bool scroll_server_pending_ = false;
  bool scroll_menu_pending_ = false;

  // A run of seeks: how far it has gone, in how many steps, and until when
  // the next press still belongs to it.
  int64_t seek_accum_us_ = 0;
  int seek_run_ = 0;
  double seek_bubble_until_ = 0.0;

  // When the position was last sent, and what (-2: counted as watched).
  double position_saved_at_ = 0.0;
  int64_t position_saved_s_ = -1;

  // Sleep timer: when playback stops (Now() seconds, 0 = off), and whether
  // that is the end of the programme on air.
  double sleep_at_ = 0.0;
  bool sleep_end_of_programme_ = false;
};
