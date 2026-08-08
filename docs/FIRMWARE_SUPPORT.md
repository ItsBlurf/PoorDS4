# Firmware support

PoorDS4 RC37 separates firmware-independent DS4 discovery from the
firmware-sensitive game bridge. It never assumes that a whole numbered family
such as `11.xx` or `12.xx` shares private `libScePad` internals.

## Admission modes

### Exact manifests

An exact manifest requires the full firmware identifier plus matching offsets
and 256-byte hashes for these exports:

1. `scePadReadState`
2. `scePadReadStateExt`
3. `scePadRead`
4. `scePadReadExt`
5. `scePadGetDataInternal`
6. `scePadGetControllerInformation`

Exact matching is evidence and a useful diagnostic label. The active bridge
still discovers runtime addresses, player entries, imports, and mappings
dynamically.

### Structural runtime admission

An unlisted firmware can be admitted without adding a manifest when every
bounded check succeeds:

- all five read/data wrappers have recognized finite shapes;
- state and extended-state wrappers converge on one executable target;
- read and extended-read wrappers converge on one executable target;
- the data target and controller-information export are executable;
- RemotePlay successfully executes controller-information on the selected
  live DS4 and returns a plausible public output layout;
- all six game fingerprints match the same-firmware `libScePad` fingerprints
  captured from RemotePlay, removing dependence on one compiler prologue;
- the client table is uniquely discovered from the validated read-state code;
- table stride, bounds, handles, and selected global player entry are valid;
- every hook is a bounded caller-owned PLT relocation still pointing to its
  expected Sony function;
- dynamic pmap discovery translates live game bytes consistently with mdbg.

Any failed or ambiguous condition rejects the game before hooks are installed.
A failed install unmaps its temporary allocation and rolls back any imports;
the supervisor retries transient launch snapshots five times.

## Evidence matrix

| Firmware | Admission | Current evidence |
| --- | --- | --- |
| 11.60 (`0x11600005`) | Exact and structural | Live: JoJo menu/battle/relaunch, Pragmata, FC26, Tekken, both DS4/DS5 connection orders, separate P1/P2, reconnect, game switching, rest cleanup, and stale recovery |
| 12.40 (`0x12400009`) | Exact | All supplied reports agree on the six offsets/hashes and wrapper relationships; RC37 no-ptrace hardware run is still outstanding |
| Other | Structural only | Eligible by runtime proof; hardware-unverified until a report and controller test are supplied |

“Eligible” is intentionally different from “guaranteed.” Firmware with the
same ABI should pass without a rebuild; firmware that changes table layout,
wrappers, imports, or mapping facilities fails closed.

## Compatibility reports

Copy the complete `/data/poords4/` directory after a test. The important
files are:

- `reports/source-fw-XXXXXXXX-pid-N.txt`: source library fingerprints and the
  live controller-information ABI result;
- `reports/fw-XXXXXXXX-pid-N.txt`: game wrapper/target evidence, table entries,
  player selection, import owners, mapping proof, and final result;
- `game-pad-bridge.log`: lifecycle, retries, health counters, and cleanup;
- `game-pad-bridge-supervisor.txt`: current state and last game PID.

The reports are designed to identify which structural condition changed so a
new firmware can be evaluated without adding an unsafe broad version bypass.
