# Architecture

## Source side

PoorDS4 attaches only to the system `SceRemotePlay` process during source
discovery. It resolves public `libScePad` functions, identifies a connected
DS4, validates one controller-information result, then starts a small reader
thread owned by RemotePlay. The reader publishes 120-byte `ScePadData`
snapshots and exits if its owning payload disappears.

Controller discovery scans all candidate users and pad indices. A live state
sample alone is never treated as proof of DS4 identity: public VID/PID data,
`scePadIsDS4Connected`, or the exact 11.60 identity table must confirm it.

## Game admission

The supervisor accepts exactly one `eboot.bin` process whose `libScePad`, libc,
and libkernel modules are ready. Before inspecting that process, it snapshots
the RemotePlay reader and requires a ready, healthy, connected DS4. Reader
loss blocks admission, cooperatively stops the old reader, invalidates its
source identity, and completes rediscovery before the launch grace restarts.
It then applies a 1.5-second launch grace and validates:

- six required pad exports and bounded 256-byte fingerprints;
- wrapper shapes, common internal targets, and executable mappings;
- controller-information ABI evidence from the live source process;
- the dynamically discovered client-table address and entry invariants;
- one unambiguous global player handle;
- each caller-owned PLT relocation and its current original destination;
- a runtime-proven virtual-to-physical mapping path before direct writes.

Exact 8.60, 11.60, and 12.40 manifests provide additional evidence but do not supply
the addresses used by the bridge. Unknown versions must pass the same
structural checks.

## Game-side bridge

The installer uses the existing kstuff remote-syscall facility to allocate and
lock two 16 KiB pages in the game: one RX page for small pad gateways and one
RW page for state. Only validated pad import slots are redirected. No game
code page, Sony library code, heap allocation, thread, or socket is modified.

The supervisor writes the inactive 120-byte frame, then atomically publishes
its sequence, finite lease, active flag, and packet count. Each gateway either
returns a stable translated snapshot for the selected handle or immediately
falls through to the saved Sony function. Non-selected handles always use the
original path.

## Player selection

Pad indices are local to a PS5 user and cannot identify global P1/P2 ordering.
PoorDS4 therefore inspects the game's global table. It prefers one exact DS4
entry, then one disconnected entry matching both the source user and local pad
index, otherwise one unique disconnected entry, and finally one sole entry.
The user/index pair disambiguates titles that preallocate multiple inactive
slots without treating a local index as global. Multiple remaining candidates
fail closed; a sole connected entry must also match the source identity so a
native DualSense is never silently taken over.

## Cleanup and recovery

Normal cleanup first disables publication, proves every import still points to
either PoorDS4's gateway or its saved Sony function, restores imports in
reverse order, waits for in-flight calls, and unmaps the anonymous allocation.

Suspend cleanup restores imports without ptrace, target syscalls, or unmapping.
The game discards the unreachable allocation when it exits. If the supervisor
is terminated abruptly, the next injection accepts a stale bridge only after
all gateways resolve to one valid bridge ABI v1 argument block with matching owners.

The product contains no game-ptrace fallback. Unknown layouts are abandoned
rather than attached to, because live testing proved that even a brief
write-free game attach can poison some titles' startup state.
