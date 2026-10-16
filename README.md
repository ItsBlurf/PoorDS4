# PoorDS4

Wireless DualShock 4 compatibility for native PS5 games and homebrew.

PoorDS4 forwards a DS4 connected to the console through the controller APIs used
by the running application. It runs in the background and follows application
changes. Native DualSense controllers keep their own input path.

[Download](https://github.com/ItsBlurf/PoorDS4/releases) ·
[Usage](docs/USAGE.md) · [Build](docs/BUILD.md) ·
[Compatibility](docs/FIRMWARE_SUPPORT.md) ·
[Report a bug](https://github.com/ItsBlurf/PoorDS4/issues)

## Getting started

1. Connect the DS4 to the PS5 and select the profile used by the game.
2. Load `PoorDS4.elf` through the console's ELF loader.
3. Start a native PS5 game or homebrew application. The payload attaches automatically.

Use the filename supplied with a published release when installing that release.
The current source targets **v0.2.0**, an unreleased compatibility update
incorporating the controller repairs made since RC51.
See [what changed](CHANGELOG.md) and the [firmware requirements](docs/FIRMWARE_SUPPORT.md).

## Features

- DS4 buttons, sticks, analog triggers and controller data forwarded from Sony's console input source.
- Shared handling for state reads, queued reads and controller information.
- Per-controller source attribution and handle-generation recovery.
- Automatic application handoff and source rediscovery.
- Separate status and stop payloads for diagnostics and normal shutdown.

Controller routing, connection metadata, queued-read continuity and trigger
compatibility have regression tests. Recent console checks cover the active
DS4 reader, Eden, FC26 and LEGO Party's input paths. DS4-only and mixed DS4 +
DualSense gameplay have also been confirmed in Isaac Repentance on 11.60.
Additional physical DS4s, controller hot-plug orders and secondary-controller
rumble still need field testing.

## Payloads

| File | Purpose |
| --- | --- |
| `PoorDS4.elf` | Start the background controller bridge. |
| `PoorDS4-status.elf` | Export current reader, bridge and controller diagnostics. |
| `PoorDS4-stop.elf` | Request normal shutdown of the running bridge. |

Load one controller bridge at a time. Use the stop payload before replacing a
running build. [Usage and troubleshooting](docs/USAGE.md) explains the runtime
states and diagnostic files.

## Development

The project uses [ps5-payload-sdk v0.43](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.43).
Build instructions and host regression checks are documented separately:

- [Build from source](docs/BUILD.md)
- [Testing and validation scope](docs/TESTING.md)
- [Architecture and source guide](docs/ARCHITECTURE.md)
- [Contributing](CONTRIBUTING.md)

## Credits and license

PoorDS4 builds on [Ghostcontrol](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher),
with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and
[kstuff-lite](https://github.com/EchoStretch/kstuff-lite).
See [NOTICE](NOTICE.md) for attribution. Licensed under [GPL-3.0-or-later](LICENSE).
