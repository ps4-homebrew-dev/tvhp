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
#include "net/http_client.h"

namespace {

// Logos are small; anything past this is not one.
constexpr size_t kMaxIconBytes = 4 * 1024 * 1024;
// The largest single fileRead.
constexpr int64_t kChunk = 256 * 1024;
// A failed icon is asked for again this much later, this many times.
constexpr auto kRetryAfter = std::chrono::seconds(60);
constexpr int kMaxAttempts = 3;

const char* const kExtensions[] = {"png", "jpg", "svg", "webp", "gif"};

// "imagecache/123" or "/imagecache/123" -> 123; 0 for anything else.
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

bool IsUrl(const std::string& icon) {
  return !icon.compare(0, 7, "http://") || !icon.compare(0, 8, "https://");
}

// The host of an http(s) URL, lower-cased: "tv.local" of
// "http://user@TV.local:9981/x".
std::string UrlHost(const std::string& url) {
  size_t at = url.find("://");
  if (at == std::string::npos)
    return {};
  at += 3;
  const size_t end = url.find_first_of("/?#", at);
  std::string host = url.substr(at, end == std::string::npos ? std::string::npos : end - at);
  const size_t user = host.rfind('@');
  if (user != std::string::npos)
    host.erase(0, user + 1);
  if (!host.empty() && host[0] == '[') {
    host = host.substr(1, host.find(']') - 1);
  } else {
    const size_t port = host.find(':');
    if (port != std::string::npos)
      host.erase(port);
  }
  for (char& c : host)
    c = (char)std::tolower((unsigned char)c);
  return host;
}

// A file name for an URL: FNV-1a, the same from run to run.
std::string UrlName(const std::string& url) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : url) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf), "u%016llx", (unsigned long long)h);
  return buf;
}

std::string FileName(const std::string& icon) {
  const uint32_t id = ImageCacheId(icon);
  return id ? std::to_string(id) : UrlName(icon);
}

// What the bytes are, by their first ones.
const char* Extension(const std::string& d) {
  auto starts = [&](const char* magic, size_t at = 0) {
    return d.compare(at, std::strlen(magic), magic) == 0;
  };
  if (starts("\x89PNG"))
    return "png";
  if (d.size() > 3 && (uint8_t)d[0] == 0xff && (uint8_t)d[1] == 0xd8 && (uint8_t)d[2] == 0xff)
    return "jpg";
  if (starts("RIFF") && starts("WEBP", 8))
    return "webp";
  if (starts("GIF8"))
    return "gif";
  // SVG: text, an <svg> element somewhere near the start.
  if (d.substr(0, 1024).find("<svg") != std::string::npos)
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

void IconCache::Start(HtspClient* client, const std::string& server, const std::string& host,
                      const std::string& user, const std::string& pass) {
  Stop();
  if (root_.empty() || !client)
    return;

  // The server's own folder: its image cache numbers are its own.
  std::string name;
  for (char c : server)
    name += (std::isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
  dir_ = root_ + "/" + (name.empty() ? std::string("server") : name);
  MakeDirs(dir_);
  host_ = UrlHost("http://" + host);
  user_ = user;
  pass_ = pass;

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
  if (icon.empty() || dir_.empty())
    return {};

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(icon);
  if (it != entries_.end()) {
    Entry& e = it->second;
    if (e.state == State::Ready)
      return e.path;
    // Failed: once more, when it is time and tries are left.
    if (e.state == State::Failed && e.attempts < kMaxAttempts && Clock::now() >= e.retry_at &&
        client_ && !stop_) {
      e.state = State::Queued;
      queue_.push_back(icon);
      cv_.notify_all();
    }
    return {};
  }

  // Fetched by an earlier session? A few stats, once per icon.
  Entry& e = entries_[icon];
  const std::string base = dir_ + "/" + FileName(icon) + ".";
  for (const char* ext : kExtensions) {
    if (IsFile(base + ext)) {
      e.state = State::Ready;
      e.path = base + ext;
      return e.path;
    }
  }
  if (!client_ || stop_) {
    entries_.erase(icon); // no worker: ask again later
    return {};
  }
  if (!ImageCacheId(icon) && !IsUrl(icon)) {
    // "picon://..." (picons the server has no folder for), or something
    // else entirely: nothing this app can fetch.
    std::fprintf(stderr, "icons: %s: not an image cache file or a web address\n", icon.c_str());
    e.state = State::Failed;
    e.attempts = kMaxAttempts;
    return {};
  }
  queue_.push_back(icon);
  cv_.notify_all();
  return {};
}

void IconCache::Worker() {
  for (;;) {
    std::string icon;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_)
        return;
      icon = queue_.front();
      queue_.pop_front();
    }
    const std::string path = Fetch(icon);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_)
        return;
      Entry& e = entries_[icon];
      e.state = path.empty() ? State::Failed : State::Ready;
      e.path = path;
      if (path.empty()) {
        e.attempts++;
        e.retry_at = Clock::now() + kRetryAfter;
      }
    }
    if (!path.empty())
      generation_++;
  }
}

std::string IconCache::Fetch(const std::string& icon) {
  std::string error;
  const uint32_t id = ImageCacheId(icon);
  const std::string path = id ? FetchImageCache(id, error) : FetchUrl(icon, error);
  if (path.empty())
    std::fprintf(stderr, "icons: %s: %s\n", icon.c_str(), error.c_str());
  return path;
}

std::string IconCache::FetchImageCache(uint32_t id, std::string& error) {
  HtspClient* client = client_;
  if (!client || !client->Connected()) {
    error = "not connected";
    return {};
  }
  uint32_t handle = 0;
  int64_t size = -1;
  if (!client->FileOpen("imagecache/" + std::to_string(id), &handle, &size, error))
    return {};
  std::string data;
  bool ok = true;
  std::vector<char> chunk(kChunk);
  for (;;) {
    const int64_t got = client->FileRead(handle, chunk.data(), kChunk, (int64_t)data.size(), error);
    if (got < 0) {
      ok = false;
      break;
    }
    if (got == 0)
      break;
    data.append(chunk.data(), (size_t)got);
    if (data.size() > kMaxIconBytes) {
      error = "too large for an icon";
      ok = false;
      break;
    }
  }
  client->FileClose(handle);
  return ok ? Save(std::to_string(id), data, error) : std::string();
}

std::string IconCache::FetchUrl(const std::string& url, std::string& error) {
  net::HttpClient http;
  net::HttpClient::Request req;
  req.url = url;
  req.stall_timeout_ms = 10000;
  req.deadline_ms = 30000;
  req.max_bytes = kMaxIconBytes;
  // The server's own web port (its picons, say) wants its login; anyone
  // else's server gets none.
  if (!host_.empty() && UrlHost(url) == host_) {
    req.user = user_;
    req.password = pass_;
  }
  net::HttpClient::Response res;
  if (!http.Perform(req, res, error))
    return {};
  if (res.status != 200) {
    error = "HTTP " + std::to_string(res.status);
    return {};
  }
  return Save(UrlName(url), res.body, error);
}

std::string IconCache::Save(const std::string& name, const std::string& data, std::string& error) {
  const char* ext = Extension(data);
  if (!ext) {
    error = "not an image this app can show";
    return {};
  }
  // Written beside it and renamed, so a half-written file never shows.
  const std::string path = dir_ + "/" + name + "." + ext;
  const std::string tmp = path + ".part";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    error = tmp + ": " + std::strerror(errno);
    return {};
  }
  const bool written = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0 || !written || std::rename(tmp.c_str(), path.c_str()) != 0) {
    error = path + ": " + std::strerror(errno);
    std::remove(tmp.c_str());
    return {};
  }
  return path;
}
