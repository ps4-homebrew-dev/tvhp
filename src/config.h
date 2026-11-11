#ifndef CONFIG_H
#define CONFIG_H

#include <cstddef>
#include <string>
#include <vector>

// One saved tvheadend server.
struct ServerProfile {
  std::string name; // display name; falls back to host when empty
  std::string host;
  std::string port = "9982";
  std::string user;
  std::string pass;

  // What the connect screen shows under the name.
  std::string Label() const;
  std::string Detail() const;
  int PortNumber() const;
  bool Valid() const { return !host.empty(); }
};

// How the player is set up, kept across sessions: the picture and sound
// adjustments of the playback menu, and the languages last picked, so a
// subtitle choice follows from channel to channel.
struct PlayerPrefs {
  int fit = 0;               // Player::Fit
  double audio_delay = 0.0;  // seconds
  double sub_scale = 1.0;
  std::string audio_lang;    // ISO 639, "" = the stream's default
  std::string sub_lang;      // "" = subtitles off
};

// The list of saved servers, persisted to tvhp.cfg (in the working directory
// on a PC, in the preference folder of the user on a PS4).
//
// File format (the old single-server format, a flat list of host=/port=/
// user=/pass= lines, is still read and silently migrated on the next save):
//
//   last=1
//   player.fit=0            (PlayerPrefs, before the first [server]; older
//   player.audio_delay=0     builds read them as an incomplete server and
//   player.sub_scale=1       drop it)
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
// Passwords are stored in the clear, exactly as the previous single-server
// config did. Anyone with access to the console's filesystem can read them.
class ServerConfig {
public:
  // Reads the config file, then applies the TVH_HOST / TVH_PORT / TVH_USER /
  // TVH_PASS environment overrides on top of the last-used entry (adding one
  // when the file is empty), so scripted launches keep working.
  void Load(const std::string& path);
  bool Save(const std::string& path) const;

  const std::vector<ServerProfile>& Servers() const { return servers_; }
  size_t Count() const { return servers_.size(); }
  bool Empty() const { return servers_.empty(); }

  // Index of the server used for the last successful connection, clamped to
  // the list. Returns 0 for an empty list.
  int LastUsed() const;
  void SetLastUsed(int index);

  const ServerProfile* At(int index) const;

  // Adds a profile and returns its index, or updates the one at 'index'.
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
