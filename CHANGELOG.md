# Changelog

## v0.2.0 — Unreleased

- Bind newly opened game clients to their independently observed DS4 user before
  the game's first controller-information check, including multiplayer joins.
- Preserve native controller identity when handles or player indexes change;
  reject duplicate clients that would copy one physical feed to another player.
- Locate the controller table from Sony's handle lookup and retain recovery
  routing for higher client indexes.

### Fixed

- Recognize longer controller-handle lookup loops, preventing an unrelated
  synchronization record from being mistaken for the controller table and
  leaving Isaac without input despite the active notification.
- Discover callable pad imports in ordinary relocation tables as well as PLT tables.
- Preserve complete import/NID identity and padded SDK symbol boundaries.
- Keep source ownership and native controller attribution separate across users and client indexes.
- Recover reader/application changes with current process and source state.
- Keep input and controller metadata on the same connection generation.
- Validate source snapshots across non-atomic transport copies.
- Observe new DS4 devices for already signed-in users and clean up owned handles correctly.
- Allow newly opened discovery handles to acquire device metadata across bounded
  reader iterations; release pending probes on shutdown and reject uncertain cleanup.
- Preserve installed game controller identities while refreshing the source reader.
- Cache compatible trigger requests using live queued input rather than an unused receiver flag.
- Retain the real DS4 packet on empty queued reads, preventing false device-loss updates in LEGO Party and similar consumers.
- Preserve native read-argument errors and route recovery-shortcut counters by validated logical controller slot.

### Changed

- Restore the current controller implementation to the RC51 release line.
- Add self-contained compiled host checks and a clearer source/build guide.
- Keep normal controller, status and stop payloads separate.

### Validation

The regression suite passes on Windows and Linux. Earlier builds in this
release line confirmed Eden input and Isaac Repentance gameplay with a DS4
alone or a DS4 and DualSense on 11.60. Continuing a saved co-op run still
requires controllers for its
existing players. Multiplayer with several DS4s, other firmware versions and
additional connection orders need more testing. The latest controller-table
correction passes captured-code controls on a PS5, and live Isaac reads receive
connected DS4 input. Gameplay in the affected tester setup still needs a retest.
DS4 vibration does not work in this build. The DS4 also lacks the DualSense's
haptic actuators and
adaptive-trigger hardware, so those effects are not supported. Vibration
support is a low priority for now, but ideas, technical findings and pull
requests are welcome.
See [testing](docs/TESTING.md) and
[compatibility](docs/FIRMWARE_SUPPORT.md).

## Published versions

- [RC51](https://github.com/ItsBlurf/PoorDS4/releases/tag/v0.1.0-rc51)
- [RC44](https://github.com/ItsBlurf/PoorDS4/releases/tag/v0.1.0-rc44)
- [All releases](https://github.com/ItsBlurf/PoorDS4/releases)

Published release notes and Git history retain the earlier version history.
