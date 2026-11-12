# tvhp

A [Tvheadend](https://tvheadend.org/) client for jailbroken PS4 consoles.
Most of the code was written with Claude.ai.

tvhp talks HTSP directly to your Tvheadend server and plays streams with
libmpv. No transcoding proxy or browser needed.

![tvhp](screenshot.jpg)

## Features

- Live TV with channel list, now/next and a per-channel programme guide
- One-button recording from the guide
- Browse, play, resume and delete recordings (including ones still recording)
- Resume positions stored on the server, so they're shared with Kodi etc.
- Audio track, subtitle (DVB), speed, delay and sleep timer settings
- Channel logos, cached on the console
- Multiple servers, with automatic reconnect
- Works with a DualShock 4, remote control or keyboard

## Requirements

- A jailbroken PS4 that can install fake packages (`.pkg`)
- A Tvheadend server (4.2 or newer) with an HTSP user (default port 9982)

## Install

Grab the `.pkg` from the [releases page](../../releases) and install it with
your package installer. TVHP will show up on the home screen.

Settings are stored in `/download0/.data/tvhp/tvhp.cfg` and logos in
`/download0/.cache/tvhp/icons/`.

> **Upgrading from an older version?** Earlier builds were packaged as a game
> and stored servers elsewhere, so you'll need to add your server again once.

## Controls

| Action | Controller | Remote | Keyboard |
| --- | --- | --- | --- |
| Select / play / pause | Cross | Enter | Enter |
| Back | Circle | Back | Esc |
| Context menu | Options | Menu | Insert |
| Switch tabs | L1 / R1 | Up, then Left / Right | Up, then Left / Right |
| Guide ↔ recordings | Square | Display | Tab |
| Delete | Triangle | Red | Delete |
| Seek ±10 s | Left / Right | Rewind / Fast forward | Left / Right |
| Seek ±5 min | Up / Down | Skip | Page Up / Down |
| Change channel (while watching) | L1 / R1 | — | — |

Pressing Options while something is playing opens the playback settings. The
full key mapping is in `src/keymap.h` and `src/gamepad.cpp`.

## Building

You'll need SDL2, SDL2_image, RmlUi 6.2, libmpv and libavformat.

On a PC:

```sh
cmake -S . -B build && cmake --build build
./build/tvhp
```

Set `TVH_HOST`, `TVH_PORT`, `TVH_USER` and `TVH_PASS` to preset a server, and
run `./build/tvhp --help` for other options.

For the PS4 you'll need the
[ps4-homebrew-dev SDK](https://github.com/ps4-homebrew-dev) 0.8.7 or later:

```sh
source $PS4_HOMEBREW_SDK/toolchain/orbis.sh
cmake -S . -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=$PS4_HOMEBREW_SDK/toolchain/orbis.cmake
cmake --build build-ps4
```

## Known limitations

- No series or manual (time-based) recordings
- No teletext subtitles
- No transcoding profile selection
- `picon://` logos aren't supported; set a picon path in Tvheadend instead
- Switching audio track on live TV can cause a few seconds of silence

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
