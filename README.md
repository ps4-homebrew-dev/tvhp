# tvhp

A [Tvheadend](https://tvheadend.org/) client for jailbroken PS4 and PS5 consoles.
Most of the code was written with Claude.ai.

![tvhp](screenshot.jpg)

## Prerequisites
- A jailbroken PS4 or PS5 that can install fake packages (`.pkg`)
- A Tvheadend server (4.2 or newer) with an HTSP user (default port 9982)

## Install
Grab the `.pkg` from the [releases page](../../releases) and install it with
your package installer.

## Building
For the PS4 you'll need the
[ps4-homebrew-dev SDK](https://github.com/ps4-homebrew-dev) 0.8.7 or later.
Then, just run:
```sh
/opt/ps4-homebrew-sdk/bin/orbis-cmake -S . -B build-ps4
make -C build-ps4
```

On a PC, you'll need SDL2, SDL2_image, RmlUi 6.2, libmpv and libavformat.
Then, just run:
```sh
cmake -S . -B build-pc
make -C build-pc
```

## Known limitations
- No series or manual (time-based) recordings
- No teletext subtitles
- No transcoding profile selection
- `picon://` logos aren't supported; set a picon path in Tvheadend instead
- Switching audio track on live TV can cause a few seconds of silence

## License
GPL-3.0-or-later, see [LICENSE](LICENSE).
