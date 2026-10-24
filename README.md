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
**v0.2.0** brings together the controller compatibility and recovery fixes
made since RC51.
See [what changed](CHANGELOG.md) and the [firmware requirements](docs/FIRMWARE_SUPPORT.md).

## Features

- DS4 buttons, sticks, analog triggers and controller data forwarded from Sony's console input source.
- Shared handling for state reads, queued reads and controller information.
- Per-controller source attribution and handle-generation recovery.
- Automatic application handoff and source rediscovery.
- Separate status and stop payloads for diagnostics and normal shutdown.
- Preserved first-failure reports and optional detailed controller logs.

Tests for controller routing, queued reads and recovery pass on Windows and
Linux. Earlier builds in this release line confirmed Eden input and Isaac
Repentance gameplay with a single DS4 or a DS4 + DualSense on firmware 11.60.
Multiplayer with several DS4s, other firmware versions and additional connection
orders need more testing.
The latest controller-table correction passes captured-code checks on a PS5,
and live Isaac reads receive connected DS4 input. Gameplay in the affected
tester setup still needs a retest.
The current build's input delivery, optional logging and normal recovery were
also checked in Eden on 11.60.

DS4 vibration does not work in this build. The DS4 also lacks the DualSense's
haptic actuators and adaptive-trigger hardware, so those effects are not
supported. Vibration support is a low priority for now, but ideas, technical
findings and pull requests are welcome.

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

## Donations & Support

Donations help with development, additional controllers and testing equipment.

- **PayPal:** [paypal.me/nblurf](https://paypal.me/nblurf)
- **Bitcoin (BTC):** `1CW8JWSnWc5w7yKhYk7ixAFG487nU5Jrzc`
- **USDT (TRC20):** `TRcrCeonXzgyxjR4LGzo4JdfEHYGGqD3gX`

Bug reports, test logs and contributions are also welcome.

## Disclaimer

PoorDS4 is an independent, open-source project for controller interoperability,
not affiliated with or endorsed by Sony. Product names and trademarks belong to
their respective owners and are used only to identify compatibility.

PoorDS4 does not supply games, Sony firmware or cryptographic keys and does not
endorse piracy or unauthorized access to copyrighted content. Use it responsibly
and in accordance with applicable law.

Provided **as is**, without warranty, to the extent permitted by law; see
[LICENSE](LICENSE), sections 15–17. Crashes or data loss are possible—back up
important saves. This notice does not alter the GPL license.
