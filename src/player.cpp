// SPDX-License-Identifier: GPL-3.0-or-later
//
// See player.h.
#include "player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>

#include "tsmux.h"

namespace {

constexpr const char* kLiveProtocol = "tvhp-live";
constexpr const char* kDvrProtocol = "tvhp-dvr";

// The largest single HTSP fileRead. Big enough to keep the request rate low
// on a 1080i transport stream, small enough that a seek doesn't stall.
constexpr uint64_t kMaxFileRead = 256 * 1024;

// How often the end of a recording that is still running is looked at
// again for new data.
constexpr auto kGrowPoll = std::chrono::milliseconds(500);

// Property ids for mpv_observe_property's reply_userdata.
enum : uint64_t {
  kPropTimePos = 1,
  kPropDuration,
  kPropPause,
  kPropSeekable,
  kPropTrackList,
};

const mpv_node* MapGet(const mpv_node* map, const char* key) {
  if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list)
    return nullptr;
  for (int i = 0; i < map->u.list->num; i++)
    if (!std::strcmp(map->u.list->keys[i], key))
      return &map->u.list->values[i];
  return nullptr;
}

std::string NodeString(const mpv_node* node) {
  return (node && node->format == MPV_FORMAT_STRING && node->u.string)
           ? std::string(node->u.string) : std::string();
}

int64_t NodeInt(const mpv_node* node, int64_t fallback) {
  if (!node)
    return fallback;
  if (node->format == MPV_FORMAT_INT64)
    return node->u.int64;
  if (node->format == MPV_FORMAT_DOUBLE)
    return (int64_t)node->u.double_;
  return fallback;
}

bool NodeFlag(const mpv_node* node) {
  return node && node->format == MPV_FORMAT_FLAG && node->u.flag;
}

// Seconds (an mpv double property) to microseconds, -1 if unknown.
int64_t PropertyUs(const mpv_event_property* prop) {
  if (prop->format != MPV_FORMAT_DOUBLE || !prop->data)
    return -1;
  const double s = *static_cast<const double*>(prop->data);
  return std::isfinite(s) && s >= 0 ? (int64_t)(s * 1e6) : -1;
}

bool PropertyFlag(const mpv_event_property* prop) {
  return prop->format == MPV_FORMAT_FLAG && prop->data &&
         *static_cast<const int*>(prop->data);
}

void* GetProcAddress(void*, const char* name) {
  return SDL_GL_GetProcAddress(name);
}

// "tvhp-dvr://123" -> 123; false if the uri isn't one of 'protocol'.
bool ParseUri(const char* uri, const char* protocol, uint64_t* out) {
  const size_t n = std::strlen(protocol);
  if (std::strncmp(uri, protocol, n) || std::strncmp(uri + n, "://", 3))
    return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(uri + n + 3, &end, 10);
  if (!end || end == uri + n + 3 || *end)
    return false;
  *out = v;
  return true;
}

// --- tvhp-live: a TsMux -----------------------------------------------------

struct LiveCookie {
  std::shared_ptr<TsMux> mux;
};

int64_t LiveRead(void* cookie, char* buf, uint64_t size) {
  return static_cast<LiveCookie*>(cookie)->mux->Read(buf, size);
}

void LiveCancel(void* cookie) {
  static_cast<LiveCookie*>(cookie)->mux->Cancel();
}

void LiveClose(void* cookie) {
  delete static_cast<LiveCookie*>(cookie);
}

// --- tvhp-dvr: a recording over the HTSP file API -------------------------

struct DvrFile {
  HtspClient* client = nullptr;
  uint32_t dvr_id = 0;
  uint32_t handle = 0;
  int64_t pos = 0;
  std::atomic<bool> cancelled{false};
};

int64_t DvrRead(void* cookie, char* buf, uint64_t size) {
  DvrFile* f = static_cast<DvrFile*>(cookie);
  size = std::min(size, kMaxFileRead);
  for (;;) {
    if (f->cancelled)
      return 0;
    std::string error;
    const int64_t got = f->client->FileRead(f->handle, buf, (int64_t)size, f->pos, error);
    if (got < 0) {
      std::fprintf(stderr, "htsp fileRead failed: %s\n", error.c_str());
      return -1;
    }
    if (got > 0) {
      f->pos += got;
      return got;
    }
    // The end of the file - for now, if it is still being recorded: wait
    // for more rather than end playback at what has been written so far.
    HtspDvrEntry entry;
    if (!f->client->Connected() || !f->client->GetDvrEntry(f->dvr_id, &entry) ||
        !entry.IsRecording())
      return 0;
    const auto until = std::chrono::steady_clock::now() + kGrowPoll;
    while (!f->cancelled && std::chrono::steady_clock::now() < until)
      SDL_Delay(20);
  }
}

int64_t DvrSeek(void* cookie, int64_t offset) {
  DvrFile* f = static_cast<DvrFile*>(cookie);
  if (offset < 0)
    return MPV_ERROR_GENERIC;
  f->pos = offset;
  return offset;
}

int64_t DvrSize(void* cookie) {
  DvrFile* f = static_cast<DvrFile*>(cookie);
  std::string error;
  int64_t size = -1;
  if (!f->client->FileStat(f->handle, &size, error) || size < 0)
    return MPV_ERROR_UNSUPPORTED;
  return size;
}

void DvrCancel(void* cookie) {
  static_cast<DvrFile*>(cookie)->cancelled = true;
}

void DvrClose(void* cookie) {
  DvrFile* f = static_cast<DvrFile*>(cookie);
  f->client->FileClose(f->handle);
  delete f;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifetime (main thread)
// ---------------------------------------------------------------------------

Player::Player() = default;

Player::~Player() {
  Shutdown();
}

bool Player::Initialize(std::string& error) {
  if (mpv_)
    return true;

  mpv_ = mpv_create();
  if (!mpv_) {
    error = "mpv_create failed";
    return false;
  }

  // The window is the app's; mpv draws through the render API and takes no
  // input. 'quiet' options are left out silently by an mpv that doesn't know
  // them (the built-in Lua scripts differ between versions; none is wanted).
  struct Option { const char* name; const char* value; bool quiet; };
  static const Option options[] = {
    // Bilinear scaling, no dithering, no LUT-based scalers: what the PS4's
    // GPU affords at 1080p next to the UI.
    {"profile", "fast", false},
    {"vo", "libmpv", false},
    {"idle", "yes", false},
    // A recording that has played to its end stays on its last frame,
    // paused, as does a live channel whose stream the server stopped.
    {"keep-open", "yes", false},
    {"terminal", "no", false},
    {"config", "no", false},
    {"load-scripts", "no", false},
    {"osc", "no", true},
    {"load-stats-overlay", "no", true},
    {"load-console", "no", true},
    {"load-osd-console", "no", true},
    {"load-auto-profiles", "no", true},
    {"load-select", "no", true},
    {"load-commands", "no", true},
    {"load-context-menu", "no", true},
    {"load-positioning", "no", true},
    {"ytdl", "no", true},
    {"input-default-bindings", "no", false},
    {"input-vo-keyboard", "no", false},
    {"osd-level", "0", false},
    {"hwdec", "no", false},
    {"audio-display", "no", false},
    // DVB subtitles and teletext aren't muxed; nothing else is wanted.
    {"sid", "no", false},
#if defined(__ORBIS__)
    // The demuxer cache comes out of the same pool as the GPU's memory.
    {"demuxer-max-bytes", "64MiB", false},
    {"demuxer-max-back-bytes", "16MiB", false},
#endif
  };
  for (const Option& o : options) {
    const int rc = mpv_set_option_string(mpv_, o.name, o.value);
    if (rc < 0 && !o.quiet)
      std::fprintf(stderr, "mpv: option %s=%s: %s\n", o.name, o.value,
                   mpv_error_string(rc));
  }

  // Extra mpv options for testing and tuning, as "name=value" words:
  //   TVHP_MPV="hwdec=auto-safe alang=eng" ./tvhp
  if (const char* extra = std::getenv("TVHP_MPV")) {
    std::string words = extra;
    size_t at = 0;
    while (at < words.size()) {
      const size_t end = std::min(words.find(' ', at), words.size());
      const std::string word = words.substr(at, end - at);
      const size_t eq = word.find('=');
      if (eq != std::string::npos) {
        const int rc = mpv_set_option_string(mpv_, word.substr(0, eq).c_str(),
                                             word.substr(eq + 1).c_str());
        if (rc < 0)
          std::fprintf(stderr, "mpv: option %s: %s\n", word.c_str(), mpv_error_string(rc));
      }
      at = end + 1;
    }
  }

  int rc = mpv_initialize(mpv_);
  if (rc < 0) {
    error = std::string("mpv_initialize: ") + mpv_error_string(rc);
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    return false;
  }

  mpv_stream_cb_add_ro(mpv_, kLiveProtocol, this, &Player::OpenLive);
  mpv_stream_cb_add_ro(mpv_, kDvrProtocol, this, &Player::OpenRecording);

  mpv_opengl_init_params gl_init = {};
  gl_init.get_proc_address = GetProcAddress;
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
    {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  rc = mpv_render_context_create(&render_, mpv_, params);
  if (rc < 0) {
    error = std::string("mpv's OpenGL renderer: ") + mpv_error_string(rc);
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    render_ = nullptr;
    return false;
  }

  // mpv's messages go to stderr; TVHP_MPV_LOG=v (or debug) says more.
  const char* log_level = std::getenv("TVHP_MPV_LOG");
  mpv_request_log_messages(mpv_, log_level && *log_level ? log_level : "warn");
  mpv_observe_property(mpv_, kPropTimePos, "time-pos", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, kPropDuration, "duration", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, kPropPause, "pause", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, kPropSeekable, "seekable", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv_, kPropTrackList, "track-list", MPV_FORMAT_NODE);

  quitting_ = false;
  event_thread_ = std::thread(&Player::EventThread, this);
  return true;
}

void Player::Shutdown() {
  if (!mpv_)
    return;
  active_ = false;
  {
    std::lock_guard<std::mutex> lock(live_mutex_);
    if (live_)
      live_->Close();
    live_.reset();
  }
  quitting_ = true;
  mpv_wakeup(mpv_);
  if (event_thread_.joinable())
    event_thread_.join();
  // The renderer goes first, while the GL context is still current.
  if (render_)
    mpv_render_context_free(render_);
  render_ = nullptr;
  mpv_terminate_destroy(mpv_);
  mpv_ = nullptr;
}

void Player::SetStatus(const std::string& text) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  status_ = text;
}

std::string Player::StatusText() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_;
}

void Player::ResetFileState() {
  seekable_ = false;
  paused_ = false;
  position_us_ = -1;
  duration_us_ = -1;
}

void Player::LoadFile(const std::string& url, const char* options) {
  // A file that played to its end was left paused there (keep-open).
  mpv_set_property_string(mpv_, "pause", "no");
  // loadfile <url> <flags> [<index>] <options>; the index came with mpv
  // 0.38 (client API 2.3). mpv_command_async() is safe from any thread, the
  // HTSP reader's included.
#if MPV_CLIENT_API_VERSION >= MPV_MAKE_VERSION(2, 3)
  const char* cmd[] = {"loadfile", url.c_str(), "replace", "-1", options, nullptr};
  if (!options)
    cmd[3] = nullptr;
#else
  const char* cmd[] = {"loadfile", url.c_str(), "replace", options, nullptr};
#endif
  mpv_command_async(mpv_, 0, cmd);
}

// ---------------------------------------------------------------------------
// Live TV
// ---------------------------------------------------------------------------

bool Player::PlayChannel(HtspClient& client, uint32_t channel_id, std::string& error) {
  Stop(client);
  if (!mpv_) {
    error = "the player is not available";
    return false;
  }

  const uint64_t session = ++session_;

  // Callbacks fire on the HTSP reader thread.
  HtspClient::StreamCallbacks cbs;
  cbs.on_start = [this, session](std::vector<HtspStreamInfo> streams) {
    if (session != session_)
      return;
    // Every start gets a transport stream of its own, and mpv a new file:
    // when the server restarts the stream (the programme's streams
    // changed), the muxer is set up for the new ones from scratch.
    auto mux = std::make_shared<TsMux>();
    const std::string desc = mux->Start(streams);
    uint64_t serial;
    {
      std::lock_guard<std::mutex> lock(live_mutex_);
      if (live_)
        live_->Close();
      live_.reset();
      if (desc.empty()) {
        SetStatus("No playable streams");
        return;
      }
      live_ = mux;
      serial = ++live_serial_;
    }
    SetStatus(desc);
    // The stream is a transport stream; telling the demuxer so spares it
    // probing a source that only delivers in real time.
    LoadFile(std::string(kLiveProtocol) + "://" + std::to_string(serial),
             "demuxer-lavf-format=mpegts");
  };
  cbs.on_packet = [this, session](HtspMuxPacket&& pkt) {
    if (session != session_)
      return;
    std::shared_ptr<TsMux> mux;
    {
      std::lock_guard<std::mutex> lock(live_mutex_);
      mux = live_;
    }
    if (mux)
      mux->Write(pkt);
  };
  cbs.on_status = [this, session](std::string status) {
    if (session == session_ && !status.empty())
      SetStatus(status);
  };
  cbs.on_stop = [this, session]() {
    if (session != session_)
      return;
    std::lock_guard<std::mutex> lock(live_mutex_);
    if (live_)
      live_->Finish();
  };
  client.SetStreamCallbacks(std::move(cbs));

  SetStatus("Tuning...");
  source_ = Source::Live;
  active_ = true;
  if (!client.Subscribe(channel_id, error)) {
    active_ = false;
    source_ = Source::None;
    session_++;
    client.SetStreamCallbacks({});
    SetStatus({});
    return false;
  }
  return true;
}

int Player::OpenLive(void* user_data, char* uri, mpv_stream_cb_info* info) {
  Player* self = static_cast<Player*>(user_data);
  uint64_t serial = 0;
  if (!ParseUri(uri, kLiveProtocol, &serial))
    return MPV_ERROR_LOADING_FAILED;

  std::lock_guard<std::mutex> lock(self->live_mutex_);
  // A file mpv was asked for before the stream restarted is gone.
  if (!self->live_ || serial != self->live_serial_)
    return MPV_ERROR_LOADING_FAILED;
  info->cookie = new LiveCookie{self->live_};
  info->read_fn = LiveRead;
  info->close_fn = LiveClose;
  info->cancel_fn = LiveCancel;
  return 0;
}

// ---------------------------------------------------------------------------
// Recordings
// ---------------------------------------------------------------------------

bool Player::PlayRecording(HtspClient& client, uint32_t dvr_id, std::string& error) {
  Stop(client);
  if (!mpv_) {
    error = "the player is not available";
    return false;
  }
  if (!client.SupportsFileApi()) {
    error = "this server is too old to stream recordings over HTSP";
    return false;
  }

  dvr_client_ = &client;
  dvr_id_ = dvr_id;
  source_ = Source::Recording;
  active_ = true;
  SetStatus("Opening recording...");
  LoadFile(std::string(kDvrProtocol) + "://" + std::to_string(dvr_id), nullptr);
  return true;
}

int Player::OpenRecording(void* user_data, char* uri, mpv_stream_cb_info* info) {
  Player* self = static_cast<Player*>(user_data);
  uint64_t id = 0;
  HtspClient* client = self->dvr_client_;
  if (!ParseUri(uri, kDvrProtocol, &id) || !client || id != self->dvr_id_)
    return MPV_ERROR_LOADING_FAILED;

  uint32_t handle = 0;
  int64_t size = -1;
  std::string error;
  if (!client->FileOpen("/dvrfile/" + std::to_string(id), &handle, &size, error)) {
    std::fprintf(stderr, "htsp fileOpen failed: %s\n", error.c_str());
    self->SetStatus("Unable to open the recording" + (error.empty() ? "" : ": " + error));
    return MPV_ERROR_LOADING_FAILED;
  }

  DvrFile* f = new DvrFile;
  f->client = client;
  f->dvr_id = (uint32_t)id;
  f->handle = handle;
  info->cookie = f;
  info->read_fn = DvrRead;
  info->seek_fn = DvrSeek;
  info->size_fn = DvrSize;
  info->close_fn = DvrClose;
  info->cancel_fn = DvrCancel;
  return 0;
}

// ---------------------------------------------------------------------------
// Stopping (main thread)
// ---------------------------------------------------------------------------

void Player::Stop(HtspClient& client) {
  const Source was = source_.load();
  session_++;
  active_ = false;
  source_ = Source::None;

  // The live stream ends first, so a demuxer blocked reading it lets go.
  {
    std::lock_guard<std::mutex> lock(live_mutex_);
    if (live_)
      live_->Close();
    live_.reset();
  }
  if (mpv_) {
    const char* stop[] = {"stop", nullptr};
    mpv_command(mpv_, stop);
  }

  if (was == Source::Live) {
    client.Unsubscribe();
    client.SetStreamCallbacks({});
  }
  dvr_client_ = nullptr;
  dvr_id_ = 0;

  ResetFileState();
  SetStatus({});
}

// ---------------------------------------------------------------------------
// Events (event thread)
// ---------------------------------------------------------------------------

void Player::EventThread() {
  std::string last_error;
  while (!quitting_) {
    mpv_event* event = mpv_wait_event(mpv_, -1);
    switch (event->event_id) {
    case MPV_EVENT_SHUTDOWN:
      quitting_ = true;
      return;

    case MPV_EVENT_LOG_MESSAGE: {
      const auto* msg = static_cast<const mpv_event_log_message*>(event->data);
      std::fprintf(stderr, "mpv[%s] %s: %s", msg->prefix, msg->level, msg->text);
      if (msg->log_level <= MPV_LOG_LEVEL_ERROR) {
        last_error = msg->text;
        while (!last_error.empty() && (last_error.back() == '\n' || last_error.back() == '.'))
          last_error.pop_back();
      }
      break;
    }

    case MPV_EVENT_START_FILE:
      last_error.clear();
      break;

    case MPV_EVENT_END_FILE: {
      const auto* end = static_cast<const mpv_event_end_file*>(event->data);
      if (end->reason == MPV_END_FILE_REASON_ERROR && active_) {
        std::string why = last_error.empty() ? mpv_error_string(end->error) : last_error;
        std::fprintf(stderr, "mpv: playback failed: %s\n", why.c_str());
        // A recording that could not be opened has said why already.
        if (StatusText().rfind("Unable to open", 0) != 0)
          SetStatus("Playback failed: " + why);
      }
      break;
    }

    case MPV_EVENT_PROPERTY_CHANGE: {
      const auto* prop = static_cast<const mpv_event_property*>(event->data);
      switch (event->reply_userdata) {
      case kPropTimePos:
        position_us_ = PropertyUs(prop);
        break;
      case kPropDuration:
        duration_us_ = PropertyUs(prop);
        break;
      case kPropPause:
        paused_ = PropertyFlag(prop);
        break;
      case kPropSeekable:
        seekable_ = PropertyFlag(prop);
        break;
      case kPropTrackList:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
          UpdateTracks(static_cast<const mpv_node*>(prop->data));
        break;
      }
      break;
    }

    case MPV_EVENT_COMMAND_REPLY:
    case MPV_EVENT_SET_PROPERTY_REPLY:
      if (event->error < 0)
        std::fprintf(stderr, "mpv: %s\n", mpv_error_string(event->error));
      break;

    default:
      break;
    }
  }
}

// The status of a recording, from what mpv found in the file: the selected
// video and audio tracks, "h264 1920x1080 + ac3 5.1(side)". Live TV keeps
// the server's description and its status messages instead.
void Player::UpdateTracks(const mpv_node* list) {
  if (source_ != Source::Recording || !list || list->format != MPV_FORMAT_NODE_ARRAY ||
      !list->u.list)
    return;

  std::string video, audio;
  for (int i = 0; i < list->u.list->num; i++) {
    const mpv_node* track = &list->u.list->values[i];
    if (!NodeFlag(MapGet(track, "selected")))
      continue;
    const std::string type = NodeString(MapGet(track, "type"));
    std::string desc = NodeString(MapGet(track, "codec"));
    if (type == "video" && !NodeFlag(MapGet(track, "albumart"))) {
      const int64_t w = NodeInt(MapGet(track, "demux-w"), 0);
      const int64_t h = NodeInt(MapGet(track, "demux-h"), 0);
      if (w > 0 && h > 0)
        desc += " " + std::to_string(w) + "x" + std::to_string(h);
      video = desc;
    } else if (type == "audio") {
      const std::string channels = NodeString(MapGet(track, "demux-channels"));
      if (!channels.empty() && channels.compare(0, 7, "unknown") != 0)
        desc += " " + channels;
      audio = desc;
    }
  }
  std::string status = video;
  if (!audio.empty())
    status += (status.empty() ? "" : " + ") + audio;
  if (!status.empty())
    SetStatus(status);
}

// ---------------------------------------------------------------------------
// Main thread
// ---------------------------------------------------------------------------

void Player::RenderVideo(int width, int height) {
  // Rendering starts with the loadfile, not after it: mpv hands the first
  // frame over as the file opens, and gives up on a renderer that doesn't
  // take it within 200 ms.
  if (!render_ || !active_)
    return;

  mpv_render_context_update(render_);

  mpv_opengl_fbo fbo = {};
  fbo.fbo = 0;
  fbo.w = width;
  fbo.h = height;
  int flip_y = 1;
  // The UI renders every frame anyway; mpv is not to hold it up waiting
  // for the next video frame's time.
  int block = 0;
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
    {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
    {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
    {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context_render(render_, params);
}

void Player::SeekRelative(int64_t delta_us) {
  if (!active_ || !CanSeek())
    return;

  // Seeking past the end would end the file; stop a second short instead.
  const int64_t pos = PositionUs();
  const int64_t dur = DurationUs();
  char amount[32];
  if (delta_us > 0 && pos >= 0 && dur > 0 && pos + delta_us > dur - 1000000) {
    std::snprintf(amount, sizeof(amount), "%.3f",
                  std::max<int64_t>(0, dur - 1000000) / 1e6);
    const char* cmd[] = {"seek", amount, "absolute", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    return;
  }
  std::snprintf(amount, sizeof(amount), "%.3f", delta_us / 1e6);
  const char* cmd[] = {"seek", amount, "relative", nullptr};
  mpv_command_async(mpv_, 0, cmd);
}

void Player::SetPaused(bool paused) {
  if (!mpv_ || source_ != Source::Recording)
    return; // live TV doesn't wait for anyone
  paused_ = paused; // at once, for the UI; the property event confirms it
  int flag = paused ? 1 : 0;
  mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

bool Player::TogglePause() {
  SetPaused(!paused_);
  return paused_;
}
