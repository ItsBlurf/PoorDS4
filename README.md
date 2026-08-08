# PoorDS4

> PoorDS4 would not have been possible without the excellent work behind
> [Ghostcontrol](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher)
> by StonedModder. Its controller research and PS5 payload foundation made this
> project possible, this project will eventually be merged to it later on if stoned Modder agrees even though its different now.
> I'm only using this for testing and gathering logs from testers until its fully stable and make this implementation much cleaner
> PoorDS4 also relies on the
> [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) and the remote-syscall
> interface documented by [kstuff-lite](https://github.com/EchoStretch/kstuff-lite).

PoorDS4 lets a wireless DualShock 4 paired with a jailbroken PS5
control native PS5 games. It does not require USB, a DualSense, a second user,
or a profile-selection prompt. Native DualSense controllers remain on Sony's
original input path and can be used independently for local multiplayer.

This is experimental homebrew that modifies a running game's pad imports.
Compatibility checks fail closed, but untested firmware and games can still
crash. Save your gamesave before testing (it broke the save file of a game during very early builds testings, never happened again but there is stil the risk so this is your disclaimer to save your work).

## Requirements

- A jailbroken PS5 with a compatible HEN/kstuff environment and ELF loader.
- A wireless DualShock 4 paired with the PS5 and connected to a logged-in user.
- Firmware 11.60 for the currently live-tested configuration. See
  [firmware support](docs/FIRMWARE_SUPPORT.md) before testing another version.

## Quick start

1. Download `PoorDS4rc37.elf` from the latest release.
2. Connect the DS4 to the PS5 user that should control the game.
3. Send the ELF once to the console's payload loader. The game may already be
   running or may be launched afterward.
4. Wait for the `wireless DS4 active` notification, then play normally.

Only run one automatic instance. PoorDS4 follows later game launches without
reinjection. It performs safe cleanup before rest mode; reinject after waking.
Use `PoorDS4-stop.elf` before replacing a running build.

## Controller compatibility

The source reader recognizes Sony DS4 v1 (`054c:05c4`), DS4 v2
(`054c:09cc`), and Sony's wireless adapter (`054c:0ba0`). It also accepts a
controller when the PS5's `scePadIsDS4Connected` API positively identifies it
as a DS4. Genuine Sony wireless DS4 revisions are supported; third-party
clones and adapters with different identities require testing.

One DS4 is translated per PoorDS4 instance. The selected DS4 may be the first,
second, or later connected controller, provided the game's pad table contains
one unambiguous destination slot.

## How it works

1. Enumerate the logged-in and Invite users' pad slots.
2. Identify a live DS4 through public device metadata and Sony's DS4 API.
3. Read its 120-byte `ScePadData` state at 120 Hz in `SceRemotePlay`.
4. Wait for one fully initialized native game process.
5. Validate the game's pad ABI, client table, player slot, import owners, and
   target mappings before changing anything.
6. Redirect only validated pad imports to a game-owned anonymous mapping and
   publish translated frames through a double buffer.

The game is never ptraced, stopped, or used to run a borrowed thread. No eboot
or `libScePad` code page is patched, and no game heap, socket, or thread is
created. See the [architecture document](docs/ARCHITECTURE.md) for the exact
safety and cleanup invariants.

## Firmware compatibility

| Firmware | Status |
| --- | --- |
| 11.60 (`0x11600005`) | Live-tested with multiple games, reconnects, game switching, multiplayer, and rest cleanup |
| 12.40 (`0x12400009`) | Exact manifest verified from supplied reports; RC37 hardware test still required |
| Other | Eligible only after all runtime structural checks pass; hardware-unverified |

Compatibility is based on proven ABI structure, not a broad `11.xx` or `12.xx`
version assumption. Unknown layouts fail closed and produce a report instead
of installing hooks. See [docs/FIRMWARE_SUPPORT.md](docs/FIRMWARE_SUPPORT.md).

## Build

Install the official [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk),
set `PS5_PAYLOAD_SDK`, then build from the repository root:

```sh
make -C payload clean
make -C payload all status stop
```

On the Windows PS5 development workspace used for release builds, load the
environment and select its target wrapper explicitly:

```powershell
. .\ps5dev-env.ps1
make -C payload CC=ps5-clang.cmd clean
make -C payload CC=ps5-clang.cmd all status stop audit
```

RC37 release assets use ps5-payload-sdk v0.42:

| Output | Purpose |
| --- | --- |
| `PoorDS4rc37.elf` | Automatic wireless DS4 bridge |
| `PoorDS4-status.elf` | Read-only bridge status snapshot |
| `PoorDS4-stop.elf` | Cooperative stop request |

## Logs and compatibility reports

Diagnostics are stored under `/data/poords4/`:

- `game-pad-bridge.log` and `game-pad-bridge.log.1`
- `game-pad-bridge-supervisor.txt`
- `game-pad-bridge-last.txt`
- `game-pad-bridge-status.txt` after running the status ELF
- `reports/source-fw-XXXXXXXX-pid-N.txt`
- `reports/fw-XXXXXXXX-pid-N.txt`

For a firmware or game incompatibility, copy the complete `/data/poords4/`
directory and include the steps that reproduced the failure. Source and game
reports identify `poords4_rc` and `report_schema`. Review reports before posting
them publicly because they contain runtime process addresses and controller
diagnostics.

## Attribution and license

PoorDS4 is a focused wireless-DS4 derivative of Ghostcontrol. The original USB
implementations and binaries are intentionally not distributed in this tree.
See [NOTICE.md](NOTICE.md) for detailed credits and [LICENSE](LICENSE) for the
GPL-3.0-or-later terms.
