// SPDX-License-Identifier: GPL-3.0-or-later
//
// See player.h.
#include "player.h"

#include <algorithm>
#include <cctype>
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

// A reported duration that moves by more than this once playback has started
// is an estimate, not the length of the file (see Player::DurationReliable).
constexpr int64_t kDurationDriftUs = 2 * 1000000;

// Statuses that only say something is on its way; mpv opening the stream
// is the end of them.
constexpr const char* kTuning = "Tuning...";
constexpr const char* kOpening = "Opening recording...";

// Property ids for mpv_observe_property's reply_userdata.
enum : uint64_t {
  kPropTimePos = 1,
  kPropDuration,
  kPropPause,
  kPropSeekable,
  kPropTrackList,
  kPropVid,
  kPropAid,
  kPropSid,
  kPropSpeed,
  kPropEof,
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

int64_t PropertyInt(const mpv_event_property* prop, int64_t fallback) {
  if (prop->format != MPV_FORMAT_INT64 || !prop->data)
    return fallback;
  return *static_cast<const int64_t*>(prop->data);
}

bool PropertyFlag(const mpv_event_property* prop) {
  return prop->format == MPV_FORMAT_FLAG && prop->data &&
         *static_cast<const int*>(prop->data);
}

// What a track's codec is called on a box or a menu rather than inside
// FFmpeg: "H.264", "Dolby Digital". Unknown ones as they are, upper-cased.
std::string CodecName(const std::string& codec) {
  static const struct { const char* ffmpeg; const char* name; } kNames[] = {
    {"h264", "H.264"}, {"hevc", "HEVC"}, {"av1", "AV1"}, {"vp9", "VP9"},
    {"mpeg2video", "MPEG-2"}, {"mpeg4", "MPEG-4"}, {"vc1", "VC-1"},
    {"aac", "AAC"}, {"aac_latm", "AAC"}, {"ac3", "Dolby Digital"},
    {"eac3", "Dolby Digital+"}, {"dts", "DTS"}, {"mp3", "MP3"}, {"mp2", "MP2"},
    {"opus", "Opus"}, {"flac", "FLAC"},
    {"subrip", "SRT"}, {"ass", "ASS"}, {"webvtt", "WebVTT"},
    {"dvb_subtitle", "DVB"}, {"dvb_teletext", "Teletext"},
    {"hdmv_pgs_subtitle", "PGS"}, {"dvd_subtitle", "VobSub"},
  };
  for (const auto& n : kNames)
    if (codec == n.ffmpeg)
      return n.name;
  std::string upper = codec;
  for (char& c : upper)
    c = (char)std::toupper((unsigned char)c);
  return upper;
}

// A picture's size as it is sold: "4K", "1080p", "720p", "SD".
std::string ResolutionName(int64_t width, int64_t height) {
  if (width <= 0 || height <= 0)
    return {};
  if (width >= 3800 || height >= 2100) return "4K";
  if (width >= 2500 || height >= 1400) return "1440p";
  if (width >= 1900 || height >= 1000) return "1080p";
  if (width >= 1260 || height >= 700) return "720p";
  return "SD";
}

// mpv's channel layout as a menu says it: "Mono", "Stereo", "5.1".
std::string ChannelsName(const std::string& layout) {
  if (layout.empty())
    return {};
  if (layout == "mono") return "Mono";
  if (layout == "stereo") return "Stereo";
  // "5.1(side)" and the like: the count is what matters.
  const size_t paren = layout.find('(');
  return paren == std::string::npos ? layout : layout.substr(0, paren);
}

// The ISO 639-2 codes broadcasters tag their streams with, as a person
// reads them. Unknown codes stay as they are.
std::string LanguageName(const std::string& code) {
  static const struct { const char* code; const char* name; } kNames[] = {
    {"eng", "English"}, {"swe", "Swedish"}, {"nor", "Norwegian"}, {"nob", "Norwegian"},
    {"dan", "Danish"}, {"fin", "Finnish"}, {"isl", "Icelandic"}, {"ice", "Icelandic"},
    {"deu", "German"}, {"ger", "German"}, {"fra", "French"}, {"fre", "French"},
    {"spa", "Spanish"}, {"ita", "Italian"}, {"nld", "Dutch"}, {"dut", "Dutch"},
    {"por", "Portuguese"}, {"pol", "Polish"}, {"ces", "Czech"}, {"cze", "Czech"},
    {"hun", "Hungarian"}, {"ell", "Greek"}, {"gre", "Greek"}, {"tur", "Turkish"},
    {"rus", "Russian"}, {"ukr", "Ukrainian"}, {"ara", "Arabic"}, {"heb", "Hebrew"},
    {"jpn", "Japanese"}, {"kor", "Korean"}, {"zho", "Chinese"}, {"chi", "Chinese"},
    {"sme", "Sami"}, {"est", "Estonian"}, {"lav", "Latvian"}, {"lit", "Lithuanian"},
    {"qaa", "Original"}, {"mul", "Multiple"},
  };
  for (const auto& n : kNames)
    if (code == n.code)
      return n.name;
  return code;
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

bool Player::Initialize(std::string& error, const std::string& fonts_dir) {
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
    // Subtitles are picked by language (SetPreferredLanguages); with no
    // preferred language they start off, as before. A language picked by
    // hand means subtitles on, even where the sound is in it too.
    {"subs-with-matching-audio", "yes", true},
    {"cache-pause", "yes", false},
    {"cache-pause-initial", "yes", false},
    {"cache-pause-wait", "1", false},
    {"demuxer-readahead-secs", "5", false},
    {"demuxer-max-bytes", "64MiB", false},
    {"demuxer-max-back-bytes", "16MiB", false},
  };
  for (const Option& o : options) {
    const int rc = mpv_set_option_string(mpv_, o.name, o.value);
    if (rc < 0 && !o.quiet)
      std::fprintf(stderr, "mpv: option %s=%s: %s\n", o.name, o.value,
                   mpv_error_string(rc));
  }

  // Subtitles in the UI's typeface, from the app's own font directory, so
  // text subtitles render with and without a system font setup. (DVB
  // subtitles are pictures and need none.)
  if (!fonts_dir.empty()) {
    mpv_set_option_string(mpv_, "sub-fonts-dir", fonts_dir.c_str());
    mpv_set_option_string(mpv_, "osd-fonts-dir", fonts_dir.c_str());
    mpv_set_option_string(mpv_, "sub-font", "LatoLatin");
    mpv_set_option_string(mpv_, "sub-bold", "yes");
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
  mpv_observe_property(mpv_, kPropVid, "vid", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, kPropAid, "aid", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, kPropSid, "sid", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, kPropSpeed, "speed", MPV_FORMAT_DOUBLE);
  mpv_observe_property(mpv_, kPropEof, "eof-reached", MPV_FORMAT_FLAG);

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
  loaded_ = false;
  eof_ = false;
  seekable_ = false;
  paused_ = false;
  position_us_ = -1;
  duration_us_ = -1;
  duration_unreliable_ = false;
  video_id_ = -1;
  audio_id_ = -1;
  sub_id_ = -1;
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  video_tracks_.clear();
  audio_tracks_.clear();
  sub_tracks_.clear();
}

void Player::ResetPerStream(int64_t start_seconds) {
  mpv_set_property_string(mpv_, "speed", "1");
  speed_ = 1.0;
  mpv_set_property_string(mpv_, "sub-delay", "0");
  sub_delay_ = 0.0;
  // "start" applies to every file loaded after it is set, so it is set (or
  // reset) for each one.
  const std::string start = start_seconds > 0 ? std::to_string(start_seconds) : "none";
  mpv_set_property_string(mpv_, "start", start.c_str());
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
  ResetPerStream(0);

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
    std::fprintf(stderr, "live: %s\n", desc.c_str());
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
    if (session != session_)
      return;
    // All well again: "Tuning..." stays until the picture comes, a problem
    // that has gone away goes.
    if (status.empty() && StatusText() == kTuning)
      return;
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

  SetStatus(kTuning);
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

bool Player::PlayRecording(HtspClient& client, uint32_t dvr_id, int64_t start_seconds,
                           std::string& error) {
  Stop(client);
  if (!mpv_) {
    error = "the player is not available";
    return false;
  }
  if (!client.SupportsFileApi()) {
    error = "this server is too old to stream recordings over HTSP";
    return false;
  }
  ResetPerStream(start_seconds);

  dvr_client_ = &client;
  dvr_id_ = dvr_id;
  source_ = Source::Recording;
  active_ = true;
  SetStatus(kOpening);
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
      // A message repeated back to back is counted, not printed again: a
      // flood of them (a broken stream, every packet) would otherwise keep
      // this thread busy writing to a slow console while events pile up.
      if (log_repeat_text_ == msg->text) {
        log_repeats_++;
      } else {
        if (log_repeats_ > 0)
          std::fprintf(stderr, "mpv: last message repeated %d more times\n", log_repeats_);
        log_repeats_ = 0;
        log_repeat_text_ = msg->text;
        std::fprintf(stderr, "mpv[%s] %s: %s", msg->prefix, msg->level, msg->text);
      }
      if (msg->log_level <= MPV_LOG_LEVEL_ERROR) {
        last_error = msg->text;
        while (!last_error.empty() && (last_error.back() == '\n' || last_error.back() == '.'))
          last_error.pop_back();
      }
      break;
    }

    case MPV_EVENT_START_FILE:
      last_error.clear();
      loaded_ = false;
      eof_ = false;
      duration_unreliable_ = false;
      break;

    case MPV_EVENT_FILE_LOADED: {
      loaded_ = true;
      // The picture is on its way: what was only a promise of it goes.
      const std::string status = StatusText();
      if (status == kTuning || status == kOpening)
        SetStatus({});
      break;
    }

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
      case kPropDuration: {
        // A real duration is fixed once playback has started. One that keeps
        // moving (a recording still being written) is not something to
        // compare the position against.
        const int64_t now = PropertyUs(prop);
        const int64_t before = duration_us_.load();
        if (now >= 0 && before >= 0 && position_us_.load() >= 0 &&
            std::llabs(now - before) > kDurationDriftUs)
          duration_unreliable_ = true;
        duration_us_ = now;
        break;
      }
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
      case kPropVid:
        video_id_ = (int)PropertyInt(prop, -1);
        break;
      case kPropAid:
        audio_id_ = (int)PropertyInt(prop, -1);
        break;
      case kPropSid:
        sub_id_ = (int)PropertyInt(prop, -1); // "no" arrives as no int
        break;
      case kPropSpeed:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
          speed_ = *static_cast<const double*>(prop->data);
        break;
      case kPropEof:
        eof_ = PropertyFlag(prop);
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

// The tracks from mpv's track-list, with labels built from the metadata
// (language, title, the accessibility flags DVB carries) plus the codec
// and, depending on the kind, the channel layout or the picture size:
// "Swedish · Dolby Digital · 5.1", "English · Audio description · AAC ·
// Stereo", "1080p · H.264", "Finnish · Hard of hearing · DVB".
void Player::UpdateTracks(const mpv_node* list) {
  std::vector<TrackInfo> video, audio, subs;

  if (list && list->format == MPV_FORMAT_NODE_ARRAY && list->u.list) {
    for (int i = 0; i < list->u.list->num; i++) {
      const mpv_node* track = &list->u.list->values[i];
      const std::string type = NodeString(MapGet(track, "type"));
      const bool is_video = type == "video";
      const bool is_sub = type == "sub";
      if (!is_video && !is_sub && type != "audio")
        continue;
      // Cover art is a video track in name only.
      if (is_video && NodeFlag(MapGet(track, "albumart")))
        continue;

      std::string label;
      auto append = [&label](const std::string& part) {
        if (!part.empty())
          label += (label.empty() ? "" : " · ") + part;
      };
      std::string lang = NodeString(MapGet(track, "lang"));
      if (lang == "und")
        lang.clear();
      append(LanguageName(lang));
      append(NodeString(MapGet(track, "title")));
      // mpv 0.37 and later; older ones say nothing, and nothing is added.
      if (NodeFlag(MapGet(track, "visual-impaired")))
        append("Audio description");
      if (NodeFlag(MapGet(track, "hearing-impaired")))
        append("Hard of hearing");

      const std::string codec = CodecName(NodeString(MapGet(track, "codec")));
      if (is_sub) {
        append(codec);
        if (NodeFlag(MapGet(track, "forced")))
          append("Forced");
      } else if (is_video) {
        append(ResolutionName(NodeInt(MapGet(track, "demux-w"), 0),
                              NodeInt(MapGet(track, "demux-h"), 0)));
        append(codec);
      } else {
        append(codec);
        // A layout the container doesn't name comes out as "unknownN".
        std::string channels = NodeString(MapGet(track, "demux-channels"));
        if (channels.empty() || channels.compare(0, 7, "unknown") == 0) {
          const int64_t n = NodeInt(MapGet(track, "demux-channel-count"), 0);
          channels = n == 1 ? "mono" : n == 2 ? "stereo" : n > 0 ? std::to_string(n) + "ch" : "";
        }
        append(ChannelsName(channels));
      }

      std::vector<TrackInfo>& into = is_sub ? subs : is_video ? video : audio;
      TrackInfo t;
      t.id = (int)NodeInt(MapGet(track, "id"), -1);
      t.label = label.empty() ? "Track " + std::to_string(into.size() + 1) : label;
      t.lang = lang;
      into.push_back(std::move(t));
    }
  }

  std::lock_guard<std::mutex> lock(tracks_mutex_);
  video_tracks_ = std::move(video);
  audio_tracks_ = std::move(audio);
  sub_tracks_ = std::move(subs);
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

void Player::SetSpeed(double speed) {
  if (!mpv_ || !active_ || source_ != Source::Recording)
    return;
  speed_ = speed; // at once, for the UI; the property event confirms it
  mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
}

// ---------------------------------------------------------------------------
// Tracks (main thread)
// ---------------------------------------------------------------------------

std::string Player::CurrentVideoLabel() const {
  const int id = video_id_;
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  for (const TrackInfo& t : video_tracks_)
    if (t.id == id)
      return t.label;
  return {};
}

std::vector<TrackInfo> Player::AudioTracks() const {
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  return audio_tracks_;
}

std::string Player::CurrentAudioLabel() const {
  const int id = audio_id_;
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  for (const TrackInfo& t : audio_tracks_)
    if (t.id == id)
      return t.label;
  return {};
}

bool Player::SelectAudioTrack(int id) {
  if (!mpv_ || !active_ || id == CurrentAudioStream())
    return false;
  bool known = false;
  for (const TrackInfo& t : AudioTracks())
    known |= t.id == id;
  if (!known)
    return false;
  int64_t value = id;
  mpv_set_property_async(mpv_, 0, "aid", MPV_FORMAT_INT64, &value);
  audio_id_ = id; // at once, for the UI
  return true;
}

std::vector<TrackInfo> Player::SubtitleTracks() const {
  std::lock_guard<std::mutex> lock(tracks_mutex_);
  return sub_tracks_;
}

bool Player::SelectSubtitleTrack(int id) {
  if (!mpv_ || !active_ || id == CurrentSubtitleStream())
    return false;
  if (id >= 0) {
    bool known = false;
    for (const TrackInfo& t : SubtitleTracks())
      known |= t.id == id;
    if (!known)
      return false;
  }
  const std::string value = id >= 0 ? std::to_string(id) : "no";
  mpv_set_property_string(mpv_, "sid", value.c_str());
  sub_id_ = id;
  return true;
}

// ---------------------------------------------------------------------------
// Adjustments (main thread)
// ---------------------------------------------------------------------------

void Player::SetDouble(const char* name, double value) {
  if (mpv_)
    mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_DOUBLE, &value);
}

void Player::SetFit(Fit fit) {
  fit_ = fit;
  if (!mpv_)
    return;
  // Fill crops to the screen (panscan), Stretch ignores the aspect ratio.
  mpv_set_property_string(mpv_, "keepaspect", fit == Fit::Stretch ? "no" : "yes");
  SetDouble("panscan", fit == Fit::Fill ? 1.0 : 0.0);
}

void Player::SetAudioDelay(double seconds) {
  audio_delay_ = seconds;
  SetDouble("audio-delay", seconds);
}

void Player::SetSubtitleDelay(double seconds) {
  sub_delay_ = seconds;
  SetDouble("sub-delay", seconds);
}

void Player::SetSubtitleScale(double scale) {
  sub_scale_ = scale;
  SetDouble("sub-scale", scale);
}

void Player::SetPreferredLanguages(const std::string& audio, const std::string& subs) {
  if (!mpv_)
    return;
  mpv_set_property_string(mpv_, "alang", audio.c_str());
  mpv_set_property_string(mpv_, "slang", subs.c_str());
}
