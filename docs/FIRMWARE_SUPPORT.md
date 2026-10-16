# Firmware and compatibility

PoorDS4 requires a jailbroken PS5 environment with the SDK services and remote
syscall support used by the bridge. Firmware recognition and successful game
input are separate checks.

## Layout profiles

The current source contains exact profile candidates for:

| Firmware | Profile identifier |
| --- | --- |
| 6.02 | `0x06020004` |
| 8.60 | `0x08600004` |
| 10.01 | `0x10010000` |
| 11.60 | `0x11600005` |
| 12.40 | `0x12400009` |

An exact profile is accepted only when its runtime fingerprints and ABI checks
match. Structural discovery can recognize other compatible layouts, but a
firmware number alone does not establish support. Unknown or inconsistent
layouts are reported rather than assumed compatible.

## Validation scope

Current console evidence includes the DS4 source reader, native application
attachment, state and queued input, application changes between Eden, FC26 and
LEGO Party, and LEGO Party's connection lifecycle on 11.60. Eden input and
Isaac Repentance gameplay with a DS4 alone or a DS4 and DualSense have been
confirmed on that firmware. The regression suite checks shared read, metadata,
ownership, discovery and trigger contracts with modeled Sony calls, including
four independent source feeds. These controls are not physical four-DS4 tests.

Multiplayer with several DS4s, guest profiles, additional connection orders and
other firmware/game combinations need more testing. DS4 vibration does not work
in this build. DualSense haptic feedback and adaptive-trigger effects are not supported.

For a useful compatibility report, include the exact firmware/build, title
version, controller setup and fresh diagnostics. See [usage](USAGE.md) and
[testing](TESTING.md).
