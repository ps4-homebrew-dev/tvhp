#ifndef CONFIG_H
#define CONFIG_H

#include <cstddef>
#include <string>
#include <vector>

// A saved Tvheadend server.
struct ServerProfile {
  std::string name; // "" shows the host
  std::string host;
  std::string port = "9982";
  std::string user;
  std::string pass;

  std::string Label() const;  // the name, or the host
  std::string Detail() const; // "host:port  -  user"
  int PortNumber() const;
  bool Valid() const { return !host.empty(); }
};

// The playback menu's settings and the languages last picked, kept.
struct PlayerPrefs {
  int fit = 0;               // Player::Fit
  double audio_delay = 0.0;  // seconds
  double sub_scale = 1.0;
  std::string audio_lang;    // ISO 639, "" = the stream's default
  std::string sub_lang;      // "" = subtitles off
};

// The saved servers and the player settings, in tvhp.cfg. Keys before any
// [server] are the old single-server format's, still read:
//
//   last=1
//   player.fit=0
//   player.audio_delay=0
//   player.sub_scale=1
//   player.alang=swe
//   player.slang=
//   [server]
//   name=Living room
//   host=192.168.1.10
//   port=9982
//   user=tv
//   pass=secret
//   [server]
//   ...
//
// Passwords are kept in the clear.
class ServerConfig {
public:
  // Reads the file, then TVH_HOST / TVH_PORT / TVH_USER / TVH_PASS over the
  // last used server (or a new one).
  void Load(const std::string& path);
  bool Save(const std::string& path) const;

  const std::vector<ServerProfile>& Servers() const { return servers_; }
  size_t Count() const { return servers_.size(); }
  bool Empty() const { return servers_.empty(); }

  // The server that last connected, clamped to the list (0 when empty).
  int LastUsed() const;
  void SetLastUsed(int index);

  const ServerProfile* At(int index) const;

  int Add(const ServerProfile& profile);
  void Update(int index, const ServerProfile& profile);
  void Remove(int index);

  PlayerPrefs& Player() { return player_; }
  const PlayerPrefs& Player() const { return player_; }

private:
  std::vector<ServerProfile> servers_;
  PlayerPrefs player_;
  int last_used_ = 0;
};

#endif
