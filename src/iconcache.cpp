// SPDX-License-Identifier: GPL-3.0-or-later
//
// See iconcache.h.
#include "iconcache.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/stat.h>

#include "htsp.h"

namespace {

// Logos are small; anything past this is not one.
constexpr int64_t kMaxIconBytes = 4 * 1024 * 1024;
// The largest single fileRead.
constexpr int64_t kChunk = 256 * 1024;

const char* const kExtensions[] = {"png", "jpg", "svg", "webp", "gif"};

// "imagecache/123" or "/imagecache/123" -> 123; 0 for anything else (an
// http URL from a server whose image cache is off).
uint32_t ImageCacheId(const std::string& icon) {
  const char* s = icon.c_str();
  if (*s == '/')
    s++;
  if (std::strncmp(s, "imagecache/", 11))
    return 0;
  char* end = nullptr;
  const unsigned long id = std::strtoul(s + 11, &end, 10);
  return end && end != s + 11 && !*end ? (uint32_t)id : 0;
}

// What the bytes are, by their first ones.
const char* Extension(const std::vector<uint8_t>& d) {
  auto starts = [&](const char* magic, size_t at = 0) {
    const size_t n = std::strlen(magic);
    return d.size() >= at + n && !std::memcmp(d.data() + at, magic, n);
  };
  if (starts("\x89PNG"))
    return "png";
  if (d.size() > 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff)
    return "jpg";
  if (starts("RIFF") && starts("WEBP", 8))
    return "webp";
  if (starts("GIF8"))
    return "gif";
  // SVG: text, an <svg> element somewhere near the start.
  const std::string head(d.begin(), d.begin() + std::min<size_t>(d.size(), 1024));
  if (head.find("<svg") != std::string::npos)
    return "svg";
  return nullptr;
}

bool IsFile(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

void MakeDirs(const std::string& path) {
  for (size_t at = 1; at <= path.size(); at++)
    if (at == path.size() || path[at] == '/')
      ::mkdir(path.substr(0, at).c_str(), 0755);
}

} // namespace

IconCache::~IconCache() {
  Stop();
}

void IconCache::SetRoot(const std::string& root) {
  root_ = root;
}

void IconCache::Start(HtspClient* client, const std::string& server) {
  Stop();
  if (root_.empty() || !client)
    return;

  // The server's own folder: its image cache numbers are its own.
  std::string name;
  for (char c : server)
    name += (std::isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
  dir_ = root_ + "/" + (name.empty() ? std::string("server") : name);
  MakeDirs(dir_);

  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  queue_.clear();
  client_ = client;
  stop_ = false;
  thread_ = std::thread(&IconCache::Worker, this);
}

void IconCache::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  client_ = nullptr;
  // What was queued and never fetched may be asked for again.
  for (auto it = entries_.begin(); it != entries_.end();)
    it = it->second.state == State::Queued ? entries_.erase(it) : std::next(it);
}

std::string IconCache::Path(const std::string& icon) {
  const uint32_t id = ImageCacheId(icon);
  if (!id || dir_.empty())
    return {};

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(id);
  if (it != entries_.end())
    return it->second.state == State::Ready ? it->second.path : std::string();

  // Fetched by an earlier session? A few stats, once per icon.
  Entry& e = entries_[id];
  for (const char* ext : kExtensions) {
    const std::string path = dir_ + "/" + std::to_string(id) + "." + ext;
    if (IsFile(path)) {
      e.state = State::Ready;
      e.path = path;
      return path;
    }
  }
  if (!client_ || stop_) {
    entries_.erase(id); // no worker: ask again later
    return {};
  }
  queue_.push_back(id);
  cv_.notify_all();
  return {};
}

void IconCache::Worker() {
  for (;;) {
    uint32_t id;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_)
        return;
      id = queue_.front();
      queue_.pop_front();
    }
    const std::string path = Fetch(id);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_)
        return;
      Entry& e = entries_[id];
      e.state = path.empty() ? State::Failed : State::Ready;
      e.path = path;
    }
    if (!path.empty())
      generation_++;
  }
}

std::string IconCache::Fetch(uint32_t id) {
  HtspClient* client = client_;
  if (!client || !client->Connected())
    return {};

  std::string error;
  uint32_t handle = 0;
  int64_t size = -1;
  if (!client->FileOpen("imagecache/" + std::to_string(id), &handle, &size, error)) {
    std::fprintf(stderr, "icons: imagecache/%u: %s\n", id, error.c_str());
    return {};
  }
  std::vector<uint8_t> data;
  bool ok = true;
  for (;;) {
    std::vector<uint8_t> chunk(kChunk);
    const int64_t got = client->FileRead(handle, chunk.data(), kChunk, (int64_t)data.size(), error);
    if (got < 0) {
      ok = false;
      break;
    }
    if (got == 0)
      break;
    data.insert(data.end(), chunk.begin(), chunk.begin() + got);
    if ((int64_t)data.size() > kMaxIconBytes) {
      error = "too large for an icon";
      ok = false;
      break;
    }
  }
  client->FileClose(handle);

  const char* ext = ok ? Extension(data) : nullptr;
  if (!ok || !ext) {
    std::fprintf(stderr, "icons: imagecache/%u: %s\n", id,
                 ok ? "not an image this app can show" : error.c_str());
    return {};
  }

  // Written beside it and renamed, so a half-written file never shows.
  const std::string path = dir_ + "/" + std::to_string(id) + "." + ext;
  const std::string tmp = path + ".part";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "icons: %s: %s\n", tmp.c_str(), std::strerror(errno));
    return {};
  }
  const bool written = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0 || !written || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return {};
  }
  return path;
}
