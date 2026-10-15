# Usage and troubleshooting

## Start and stop

Connect the DS4 to the console under the profile that will run the application,
then send the controller payload to the existing ELF loader. The loader commonly
listens on TCP port 9021. Start the game or homebrew normally.

The bridge remains in the background across application changes. Its source
reader uses the console's Sony input service. A PC Remote Play stream or virtual
controller is not required.

To replace or unload the bridge, send `PoorDS4-stop.elf`. Wait for normal
shutdown before loading another controller build. The stop payload targets the
bridge's own supervisor and reader.

## Recovery shortcut

Hold **L1 + R1 + L2 + R2** on a connected, recognized controller to request
bridge recovery. The shortcut uses consecutive input samples, so its timing
depends on the application's polling rate. Each logical controller has its own
counter. Unknown handles and disconnected records cannot request recovery.

## Diagnostics

Send `PoorDS4-status.elf` to export a bounded report. The status payload is
read-only with respect to controller input. Runtime files are under
`/data/poords4/`, including:

- `game-pad-bridge-supervisor.txt`: supervisor state and current process bindings.
- `game-pad-bridge-status.txt`: reader, controller slots and game callbacks.

A successful transfer means the loader received the file. Check the payload's
startup/status output to establish that it executed and attached.

| Observation | What to check |
| --- | --- |
| Waiting for a source | DS4 connection, signed-in profile and Sony input-service availability. |
| Waiting for a game | Start a supported native application and obtain a fresh status report. |
| Source connected, no game calls | Application imports and the bridge installation result in the report. |
| Input stops after switching apps | Current game/source bindings and recovery state. |
| Repeated disconnect notice | Connection state, queued-read results and metadata reacquisition counts. |

## Reporting a problem

Include the firmware, payload version/build, game or homebrew version,
controller models and connection order. Describe whether the problem occurs
at startup, while idle, during input, after hot-plug or after an app change.
Attach a fresh status report and relevant runtime log. Remove console addresses,
account identifiers and other private information before posting.

The [compatibility guide](FIRMWARE_SUPPORT.md) distinguishes recognized layouts
from console-tested behavior. [Testing](TESTING.md) describes the remaining
field coverage.
