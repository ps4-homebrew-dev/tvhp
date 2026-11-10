// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel icons, kept on disk. A channel's logo comes as Tvheadend names it
// in channelIcon, which is one of:
//
//   imagecache/<n>   a file of the server's image cache (on when the server
//                    has "image cache" enabled, and always for local picons,
//                    file://). HTSP's fileOpen reads it like a recording,
//                    over the same connection and its login.
//   http(s)://...    the logo's own address, which is what a server with
//                    the image cache off (Tvheadend's default) sends.
//                    Fetched with libcurl, with the server's login when it
//                    is on the server's own host.
//
// A worker thread fetches what is asked for, one at a time, into
// <root>/<server>/<name>.<png|jpg|svg|webp|gif> (RmlUi picks the decoder by
// the extension, so the file is named for what it holds; <name> is the
// image cache number, or a hash of the URL). Once there, an icon is not
// fetched again. One that could not be fetched is tried again a minute
// later, a few times: a server with its image cache on fetches logos in the
// background, and may not have one yet when it is first asked.
//
// Threads: the main thread asks (Path) and polls (Generation); the worker
// does the requests and the file I/O.
#ifndef ICONCACHE_H
#define ICONCACHE_H

#include <atomic>
#include <chrono>
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
  // ("host_port"); 'host', 'user' and 'pass' are its login, sent along to
  // logos on that host. Stops whatever ran for the one before.
  void Start(HtspClient* client, const std::string& server, const std::string& host,
             const std::string& user, const std::string& pass);
  // Stops the worker, after the request in flight. Call before the client
  // disconnects or reconnects.
  void Stop();

  // Main thread. The file of 'icon' (a channel's channelIcon) when it is
  // on disk, else "" and a fetch is queued (again, after a failure, once
  // the retry time has come).
  std::string Path(const std::string& icon);

  // Bumped whenever an icon has arrived: then is the time to ask again.
  uint64_t Generation() const { return generation_.load(); }

private:
  using Clock = std::chrono::steady_clock;
  enum class State { Queued, Ready, Failed };
  struct Entry {
    State state = State::Queued;
    std::string path;
    int attempts = 0;
    Clock::time_point retry_at;
  };

  void Worker();
  // Fetches one icon into the server's folder; its path, or "".
  std::string Fetch(const std::string& icon);
  std::string FetchImageCache(uint32_t id, std::string& error);
  std::string FetchUrl(const std::string& url, std::string& error);
  // The file the bytes go to, named for what they are; "" when they are not
  // an image this app shows.
  std::string Save(const std::string& name, const std::string& data, std::string& error);

  std::string root_;
  std::string dir_; // root_/server
  std::string host_, user_, pass_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<std::string, Entry> entries_; // by channelIcon
  std::deque<std::string> queue_;
  bool stop_ = false;
  HtspClient* client_ = nullptr;
  std::thread thread_;
  std::atomic<uint64_t> generation_{0};
};

#endif
