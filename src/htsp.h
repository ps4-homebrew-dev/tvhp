#ifndef HTSP_CLIENT_H
#define HTSP_CLIENT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "htsmsg.h"
#include "net.h"
}

struct HtspChannel {
  uint32_t id = 0;
  uint32_t number = 0;
  std::string name;
  // The logo as the server names it: "imagecache/123", an http(s) URL (the
  // image cache off), "picon://..." or "" (see IconCache).
  std::string icon;
};

// An EPG event (eventAdd/eventUpdate). Times are unix seconds.
struct HtspEvent {
  uint32_t id = 0;
  uint32_t channel_id = 0;
  int64_t start = 0;
  int64_t stop = 0;
  std::string title;
  std::string subtitle;
  std::string summary;
  std::string description;
  uint32_t season = 0;  // 0 = not set
  uint32_t episode = 0; // 0 = not set
};

// A scheduled, running or finished recording (dvrEntryAdd/dvrEntryUpdate).
// Times are unix seconds.
struct HtspDvrEntry {
  uint32_t id = 0;
  uint32_t channel_id = 0;
  uint32_t event_id = 0;
  int64_t start = 0;
  int64_t stop = 0;
  std::string title;
  std::string subtitle;
  std::string summary;
  std::string description;
  std::string channel_name;
  std::string state; // "scheduled", "recording", "completed", "missed", "invalid"
  std::string error;
  int64_t data_size = 0;
  uint32_t season = 0;
  uint32_t episode = 0;
  bool enabled = true;
  // Where playback was left (seconds, 0 = the start) and how often it was
  // watched to the end; the server keeps both (HTSP 27+), for every client.
  uint32_t play_position = 0;
  uint32_t play_count = 0;

  bool IsScheduled() const { return state == "scheduled"; }
  bool IsRecording() const { return state == "recording"; }
  bool IsFinished() const { return state == "completed"; }
  // There is a file to play; a running recording's grows as it plays.
  bool HasFile() const { return IsFinished() || IsRecording(); }
  // Upcoming or running: it can be cancelled.
  bool IsPending() const { return IsScheduled() || IsRecording(); }
};

// An elementary stream of a subscriptionStart.
struct HtspStreamInfo {
  uint32_t index = 0;
  std::string type; // "H264", "HEVC", "MPEG2VIDEO", "AC3", "AAC", ...
  uint32_t width = 0, height = 0;   // video
  uint32_t channels = 0, rate = 0;  // audio
  std::string language;             // ISO 639-2, "" when not given
  // DVB subtitles: the pages of the service's subtitles.
  uint32_t composition_id = 0, ancillary_id = 0;
  // The codec's global header ("meta") when the server sends one, such as an
  // AAC AudioSpecificConfig for packets without ADTS headers.
  std::vector<uint8_t> extradata;
};

// A packet (muxpkt). Timestamps are microseconds.
struct HtspMuxPacket {
  uint32_t stream = 0;
  int64_t pts = INT64_MIN; // INT64_MIN = unknown
  int64_t dts = INT64_MIN;
  int64_t duration = 0;
  char frametype = 0; // 'I', 'P', 'B' or 0
  std::vector<uint8_t> payload;
};

// An HTSP client, on libhts (as Kodi's pvr.hts):
//   hello                -> server capabilities + 32-byte auth challenge
//   authenticate         -> digest = SHA1(password + challenge)
//   enableAsyncMetadata  -> server pushes channelAdd/eventAdd/dvrEntryAdd/...
//                           then initialSyncCompleted
//   subscribe(channelId) -> subscriptionStart followed by a stream of muxpkt
//   fileOpen("/dvrfile/ID") + fileRead/fileStat -> recording playback
class HtspClient {
public:
  ~HtspClient();

  // Connects, says hello and authenticates. Blocks. Can be called again
  // after a failure or a Disconnect().
  bool Connect(const std::string& host, int port, const std::string& user, const std::string& pass, std::string& error);

  // Starts the reader thread and asks for channels, the EPG and the
  // recordings, which then keep coming as they change.
  void StartAsync();

  // --- Snapshots, from any thread --------------------------------------
  // Bumped on every change of channels, events or recordings, and on the
  // initial sync and a disconnect.
  uint64_t Generation() const { return generation.load(); }

  // StartAsync() has run and the connection is alive.
  bool Connected() const { return running.load(); }

  // The server has sent everything it had at the start.
  bool SyncCompleted() const;

  // Channels sorted by number, then name.
  std::vector<HtspChannel> GetChannels() const;

  // A channel's events from the one on now, by start; 0 = no limit.
  std::vector<HtspEvent> GetChannelEvents(uint32_t channel_id, size_t max_events = 0) const;

  // What a channel has on now.
  struct NowNext {
    bool has_now = false;
    HtspEvent now;
  };
  NowNext GetNowNext(uint32_t channel_id) const;

  // --- DVR (recordings) --------------------------------------------------
  // Pending recordings first (soonest first), then the rest (newest
  // first).
  std::vector<HtspDvrEntry> GetDvrEntries() const;

  // The recording of an EPG event, if any.
  bool GetDvrEntryForEvent(uint32_t event_id, HtspDvrEntry* out) const;
  bool GetDvrEntry(uint32_t dvr_id, HtspDvrEntry* out) const;

  // Schedules a recording; its entry follows as a dvrEntryAdd.
  bool RecordEvent(uint32_t event_id, std::string& error);

  // Where playback was left (0 = the start) and, with 'watched', one more
  // viewing to the end. The local copy changes at once; the request is not
  // waited for. Not for servers before HTSP 27, which keep no position.
  void SavePlayPosition(uint32_t dvr_id, uint32_t seconds, bool watched);
  // Watched (once, from the start next time) or never played. Not waited
  // for either.
  void SetWatched(uint32_t dvr_id, bool watched);

  // Cancelling stops or unschedules a recording and keeps what was
  // recorded; deleting removes it and its file.
  bool CancelDvrEntry(uint32_t dvr_id, std::string& error);
  bool DeleteDvrEntry(uint32_t dvr_id, std::string& error);

  // --- Live TV -------------------------------------------------------------
  // On the reader thread; not to block.
  struct StreamCallbacks {
    std::function<void(std::vector<HtspStreamInfo>)> on_start;
    std::function<void(HtspMuxPacket&&)> on_packet;
    std::function<void(std::string)> on_status; // a problem, "" when over, or why it stopped
    std::function<void()> on_stop;              // after on_status
  };
  void SetStreamCallbacks(StreamCallbacks cbs);

  // Replaces any subscription.
  bool Subscribe(uint32_t channel_id, std::string& error);
  void Unsubscribe();

  // --- Files (HTSP 8+) -----------------------------------------------------
  // "/dvrfile/<id>" for recordings, "imagecache/<id>" for logos. From any
  // thread.
  bool FileOpen(const std::string& path, uint32_t* handle, int64_t* size, std::string& error);
  // Up to 'size' bytes at 'offset' (< 0: where the last read ended): the
  // count, 0 at the end, -1 on error.
  int64_t FileRead(uint32_t handle, void* buf, int64_t size, int64_t offset, std::string& error);
  bool FileStat(uint32_t handle, int64_t* size, std::string& error);
  void FileClose(uint32_t handle);

  void Disconnect();

  bool SupportsFileApi() const { return htsp_version >= 8; }
  bool SupportsPlayPosition() const { return htsp_version >= 27; }

private:
  // These take 'msg'. SendRequest waits for the reply (nullptr on a timeout
  // or a lost connection), which the caller then owns.
  htsmsg_t* SendRequest(htsmsg_t* msg, int timeout_ms, std::string& error);

  // A request answered with success or an error only.
  bool SimpleRequest(htsmsg_t* msg, std::string& error);
  // Without waiting for the reply.
  void SendNoReply(htsmsg_t* msg);
  bool SendMessage(htsmsg_t* msg, std::string& error);
  // The handshake's, before the reader thread runs.
  htsmsg_t* ReadMessage(int timeout_ms, std::string& error);

  void ReaderLoop();
  void HandleAsyncMessage(htsmsg_t* msg); // does not take 'msg'

  // With channels_mutex held.
  void UpsertEventLocked(htsmsg_t* msg, bool is_update);
  void RemoveEventLocked(uint32_t event_id);
  void PruneExpiredLocked(time_t now);

  void UpsertDvrEntryLocked(htsmsg_t* msg);
  void RemoveDvrEntryLocked(uint32_t dvr_id);

  socket_t sock = -1;
  std::mutex write_mutex;

  std::thread reader;
  std::atomic<bool> running{false};

  // Requests waiting for their reply, by seq.
  struct Pending {
    htsmsg_t* reply = nullptr;
    bool done = false;
  };
  std::mutex pending_mutex;
  std::condition_variable pending_cv;
  std::map<uint32_t, Pending*> pending;
  uint32_t next_seq = 1;

  // Guards what follows, up to initial_sync_done.
  mutable std::mutex channels_mutex;
  std::map<uint32_t, HtspChannel> channels; // by channelId
  // Events by channel and start, and where each is (eventUpdate and
  // eventDelete carry only the eventId).
  std::map<uint32_t, std::map<int64_t, HtspEvent>> epg; // channelId -> start -> event
  struct EventLocator { uint32_t channel_id = 0; int64_t start = 0; };
  std::map<uint32_t, EventLocator> event_index; // eventId -> locator
  std::map<uint32_t, HtspDvrEntry> dvr;         // dvrId -> entry
  std::map<uint32_t, uint32_t> dvr_by_event;    // eventId -> dvrId
  time_t last_prune = 0;
  bool initial_sync_done = false;

  std::atomic<uint64_t> generation{0};

  int htsp_version = 0;

  StreamCallbacks stream_cbs;
  std::atomic<uint32_t> subscription_id{0}; // 0 = no active subscription
  uint32_t next_subscription_id = 1;
};

#endif
