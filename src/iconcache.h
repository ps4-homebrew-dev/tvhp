// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel icons, kept on disk. Tvheadend names a channel's logo
// "imagecache/<n>", a file of its image cache, which HTSP's fileOpen reads
// like a recording: so the icons come over the same connection as
// everything else, with its login, and need no web port.
//
// A worker thread fetches what is asked for, one at a time, into
// <root>/<server>/<n>.<png|jpg|svg|webp|gif> (RmlUi picks the decoder by
// the extension, so the file is named for what it holds). Once there, an
// icon is not fetched again: Tvheadend gives a changed logo a new number.
//
// Threads: the main thread asks (Path) and polls (Generation); the worker
// does the HTSP requests and the file I/O.
#ifndef ICONCACHE_H
#define ICONCACHE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

class HtspClient;

class IconCache {
public:
  ~IconCache();

  // Where the icons go; "" keeps none (and fetches none).
  void SetRoot(const std::string& root);

  // Starts fetching for a connected server. 'server' names its folder
  // ("host_port"). Stops whatever ran for the one before.
  void Start(HtspClient* client, const std::string& server);
  // Stops the worker, after the request in flight. Call before the client
  // disconnects or reconnects.
  void Stop();

  // Main thread. The file of 'icon' (a channel's channelIcon) when it is
  // on disk, else "" and, the first time, a fetch is queued.
  std::string Path(const std::string& icon);

  // Bumped whenever an icon has arrived: then is the time to ask again.
  uint64_t Generation() const { return generation_.load(); }

private:
  enum class State { Queued, Ready, Failed };
  struct Entry {
    State state = State::Queued;
    std::string path;
  };

  void Worker();
  // Fetches one icon over HTSP into the server's folder; its path, or "".
  std::string Fetch(uint32_t id);

  std::string root_;
  std::string dir_; // root_/server

  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint32_t, Entry> entries_; // by image cache number
  std::deque<uint32_t> queue_;
  bool stop_ = false;
  HtspClient* client_ = nullptr;
  std::thread thread_;
  std::atomic<uint64_t> generation_{0};
};

#endif
