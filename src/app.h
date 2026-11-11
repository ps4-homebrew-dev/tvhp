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
// between them. All RmlUi access happens on the main thread; changes coming
// from the HTSP reader thread are picked up by polling client.Generation()
// once per frame in Update().
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

  // Creates the data model and loads main.rml from the assets. Must be
  // called before the first context update.
  bool Initialize(Rml::Context* context, const Options& options, std::string& error);
  void Shutdown();

  // Per-frame housekeeping; call before context->Update().
  void Update();

  // Hook for the player to draw video beneath the UI; call between
  // Backend::BeginFrame() and context->Render().
  void RenderVideo(int width, int height);

  // Rml::EventListener (document-level keydown, capture phase).
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

  // Rows exposed to the RmlUi data model.
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
  // An entry of the menu. 'value' reports the current setting; 'change' is
  // called with -1/+1 for left/right and 0 for select. Entries without a
  // value are actions: they close the menu when selected.
  struct MenuItem {
    std::string label;
    std::function<std::string()> value;
    std::function<void(int)> change;
  };
  bool SetupDataModel(Rml::Context* context, std::string& error);

  // --- Connect screen ----------------------------------------------------
  void RebuildServerRows();
  void LoadFormFromSelection();  // saved server -> dialog fields
  // The add/edit dialog is modal over the server list. 'add_new' starts from
  // a blank profile; otherwise the highlighted server is loaded for editing.
  void OpenServerDialog(bool add_new);
  void CloseServerDialog();
  ServerProfile FormToProfile() const;
  // Commits the edit fields to the profile list and closes the dialog.
  // Connecting is a separate step, done from the server list.
  void SaveServer();
  void StartConnect();
  void PollConnectState();
  void DeleteSelectedServer();
  void OpenServerMenu();
  void SetConnectZone(Zone zone);
  int CurrentFormRow(bool* in_input) const;
  void FocusFormField(int delta);  // up/down: label -> label, input -> input
  // Focuses a row's text field with its text selected, so what the PS4
  // keyboard types replaces it (the keyboard can only append).
  bool EnterFormInput(int row);
  bool FocusFormInput();  // right: label -> input
  bool FocusFormLabel();  // left: input -> label (only at caret start)
  void HandleKeyConnect(Rml::Event& event, int key);

  // --- Main screen -------------------------------------------------------
  void SwitchToMain();
  void SwitchToConnect(const std::string& status);
  // Drops the HTSP connection and goes back to the server list, so another
  // server can be picked.
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
  // Triangle: cancels a pending recording or deletes a finished one.
  void RemoveSelectedRecording();
  void CancelOrDeleteRecording(const HtspDvrEntry& entry, bool destructive);
  const HtspDvrEntry* SelectedRecording() const;

  // --- Full-screen playback ("watching") ---------------------------------
  void EnterWatch();
  void ExitWatch();
  void ShowWatchInfo(double seconds);
  void UpdateWatchOverlay();
  void EnsureRowVisible(const char* list_id, int index, float row_pitch);
  // A notice for a few seconds, with an icon (src/icons.h) in front.
  void ShowToast(const std::string& text, const char* icon = nullptr);

  // --- Video behind the menus (from jtplay) --------------------------------
  // Back from full screen leaves the video playing behind the guide or the
  // recordings, under a shade, as in Kodi; selecting what plays (or the
  // Options menu's "Full screen") goes back to it, and Back from the top of
  // the menus stops it.
  void SetFullscreen(bool on);

  // --- Channel icons --------------------------------------------------------
  // The logo of a channel as an RmlUi decorator ("image(...) contain"), or
  // "none" while there is none (yet: IconCache fetches it).
  Rml::String IconDecorator(uint32_t channel_id);
  // Icons have arrived: the rows, the guide and the player show them.
  void ApplyIcons();
  bool VideoBehind() const { return bind_watching_ && !bind_fullscreen_; }
  // The list rows of what plays, marked as such.
  void MarkPlayingRows();

  // --- Playback (from jtplay) ----------------------------------------------
  // Recordings: asks whether to resume when the server has a position for
  // it, then plays.
  void StartRecording(uint32_t dvr_id, int64_t start_seconds);
  // Tells the server how far the recording that plays has got ('ended':
  // all the way), so it resumes there on any client. Sent when playback
  // stops and every kPositionSaveSec; skipped when nothing has changed.
  void SaveRecordingPosition(bool ended = false);
  // Left/right and the remote's rewind/fast forward: steps that grow while
  // pressed in quick succession, with a bubble saying how far in all.
  void SeekStep(int sign, bool large);
  void CycleAudioTrack(int direction);
  void CycleSubtitleTrack(int direction);
  // A track picked by hand: its language is remembered and preferred from
  // then on, so subtitles stay on (or off) from one channel to the next.
  void PickAudioTrack(const TrackInfo& track);
  void PickSubtitleTrack(const TrackInfo* track); // nullptr = off
  void ApplyPlayerPrefs();
  void SavePlayerPrefs();
  std::string SleepLabel() const;
  void UpdateSleepTimer();

  // The playback menu (Options while watching) and the resume question:
  // a side blade, or a small dialog in the middle of the screen.
  void OpenWatchMenu();
  void OpenResumeDialog(const HtspDvrEntry& entry);
  // The Options button on the guide or the recordings (PS4 style): what can
  // be done with what is selected. Over a video playing behind the menus,
  // the playback menu instead.
  void OpenOptionsMenu();
  // A menu of plain actions (no settings), titled for what it is about.
  void OpenContextMenu(const std::string& title, std::vector<MenuItem> items);
  // "Delete X?": the action, and Cancel. 'detail' says what it means.
  void OpenConfirm(const std::string& question, const std::string& detail,
		   const std::string& action, std::function<void()> run);
  void CloseMenu();
  void RebuildMenuRows();
  void HandleKeyMenu(Rml::Event& event, int key);

  void HandleKeyMain(Rml::Event& event, int key);
  void HandleKeyWatch(Rml::Event& event, int key);

  // Data model bound state (main thread only).
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
  Rml::String bind_watch_icon_ = "none"; // the logo of the channel playing
  Rml::String bind_detail_title_;
  Rml::String bind_detail_meta_;
  Rml::String bind_detail_desc_;
  Rml::String bind_detail_rec_;   // "Recording scheduled" banner
  Rml::String bind_player_status_;
  bool bind_watching_ = false;     // something plays (full screen or behind)
  bool bind_fullscreen_ = false;   // ... full screen, the menus hidden
  bool bind_info_visible_ = false; // watch info bar shown (auto-hides)
  Rml::String bind_watch_name_;
  Rml::String bind_watch_now_;
  Rml::String bind_watch_time_;
  Rml::String bind_watch_progress_ = "0%"; // data-style-width; never empty
  bool bind_watch_recorded_ = false;       // playing a recording, not live
  bool bind_watch_paused_ = false;
  Rml::String bind_watch_ends_;    // "Ends at 21:40"
  Rml::String bind_watch_seek_;    // seek bubble: "» +2:30"
  Rml::String bind_watch_seek_at_ = "0%"; // where it rides on the bar
  Rml::String bind_watch_vtrack_;  // "1080p · H.264"
  Rml::String bind_watch_atrack_;  // "Swedish · Dolby Digital · 5.1"
  Rml::String bind_watch_strack_;  // subtitles shown, "" when off
  Rml::String bind_watch_speed_;   // "1.5×", "" at normal speed
  Rml::String bind_watch_sleep_;   // "Sleep 25 min", "" when off

  // Menu bindings.
  bool bind_menu_open_ = false;
  bool bind_menu_dialog_ = false;  // centered dialog rather than side blade
  Rml::String bind_menu_title_;
  Rml::String bind_menu_meta_;
  Rml::String bind_menu_progress_ = "0%"; // data-style-width; "0%" = no bar
  std::vector<MenuRow> menu_rows_;
  std::vector<MenuItem> menu_items_;
  int sel_menu_ = 0;

  // Connect screen bindings.
  std::vector<ServerRow> server_rows_;
  int sel_server_ = 0;
  int server_count_ = 0;
  Rml::String cfg_name_;
  Rml::String cfg_host_;
  Rml::String cfg_port_ = "9982";
  Rml::String cfg_user_;
  Rml::String cfg_pass_;

  // Main screen bindings.
  std::vector<ChannelRow> channel_rows_;
  std::vector<EpgRow> epg_rows_;
  std::vector<RecRow> rec_rows_;
  int sel_channel_ = 0;
  int sel_epg_ = 0;
  int sel_rec_ = 0;
  int channel_count_ = 0;
  int rec_count_ = 0;
  bool connecting_ = false;

  // Backing data (main thread copies of client state).
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

  // Connect worker.
  enum class ConnectState { Idle, Busy, Success, Failed };
  std::thread connect_thread_;
  std::atomic<ConnectState> connect_state_{ConnectState::Idle};
  std::string connect_error_; // only touched by worker until state != Busy
  int connecting_index_ = -1; // profile being connected to

  uint64_t seen_generation_ = 0;
  double last_refresh_time_ = 0.0;
  double toast_deadline_ = 0.0;
  double info_deadline_ = 0.0; // watch info bar auto-hide
  bool scroll_channels_pending_ = false;
  bool scroll_epg_pending_ = false;
  bool scroll_rec_pending_ = false;
  bool scroll_server_pending_ = false;
  bool scroll_menu_pending_ = false;

  // Seek run: how far the presses so far have jumped, how many there were,
  // and until when the next one still belongs to the run.
  int64_t seek_accum_us_ = 0;
  int seek_run_ = 0;
  double seek_bubble_until_ = 0.0;

  // Resume: when the position was last sent, and what was sent.
  double position_saved_at_ = 0.0;
  int64_t position_saved_s_ = -1;
  int64_t start_position_s_ = 0; // where the recording was started

  // Sleep timer: when playback stops (Now() seconds), 0 = off; and whether
  // that is the end of the programme on air.
  double sleep_at_ = 0.0;
  bool sleep_end_of_programme_ = false;
};
