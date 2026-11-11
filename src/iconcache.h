// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel logos, kept on disk. A channel's channelIcon is one of:
//
//   imagecache/<n>   a file of the server's image cache (when that is on,
//                    and for local picons), read with fileOpen
//   http(s)://...    the logo's own address (the image cache off, as by
//                    default), fetched with libcurl; with the server's
//                    login when it is on the server's host
//
// A worker fetches them one at a time into <root>/<server>/<name>.<ext>,
// <name> being the image cache number or a hash of the URL, and <ext> what
// the bytes are (RmlUi picks a decoder by it). A failed one is tried again
// a minute later, a few times: a server may still be fetching it itself.
//
// The main thread asks (Path) and polls (Generation).
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

  // "" keeps (and fetches) none.
  void SetRoot(const std::string& root);

  // For a connected server; 'server' names its folder ("host_port"), and
  // its login goes with logos on 'host'.
  void Start(HtspClient* client, const std::string& server, const std::string& host,
             const std::string& user, const std::string& pass);
  // After the request in flight. Before the client disconnects.
  void Stop();

  // The file of a channelIcon, or "" and a fetch is queued.
  std::string Path(const std::string& icon);

  // Bumped when a logo arrives.
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
  // Its path, or "".
  std::string Fetch(const std::string& icon);
  std::string FetchImageCache(uint32_t id, std::string& error);
  std::string FetchUrl(const std::string& url, std::string& error);
  // "" when the bytes are no image this app shows.
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
