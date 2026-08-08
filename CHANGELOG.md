# Changelog

All notable changes to this project are documented here. Release tags follow
Semantic Versioning; wireless-bridge candidates use `0.1.0-rcN`.

## [0.1.0-rc37] - 2026-08-08

### Changed

- Remove unused `libScePad`, `libSceNet`, pthread, and dynamic-loader link
  inputs. The automatic payload now adds only its direct user-service
  dependency; the status and stop utilities use only the SDK runtime libraries
  they import.
- Add build-audit assertions that prevent those unrelated dependencies from
  returning to future release payloads.

## [0.1.0-rc36] - 2026-08-08

### Changed

- Rename the project, payloads, runtime paths, report fields, notifications,
  and internal public identifiers to PoorDS4.
- Move runtime diagnostics to `/data/poords4/` and name the release payloads
  `PoorDS4rc36.elf`, `PoorDS4-status.elf`, and `PoorDS4-stop.elf`.
- Prepare the focused wireless bridge as a public fork of Ghostcontrol, with
  prominent attribution to StonedModder, ps5-payload-sdk, and kstuff-lite.
- Replace development-era bridge layout labels with the stable `bridge ABI v1`
  name while retaining binary compatibility for safe stale-bridge cleanup.
- Make the default Makefile use the official SDK compiler; Windows workspace
  builds select `ps5-clang.cmd` explicitly.

## [0.1.0-rc35] - 2026-08-08

### Fixed

- Require a freshly validated wireless reader before inspecting or modifying
  a newly launched game. Live RC34 transitions showed that a simultaneous DS4
  re-publication and game exit could carry `reader_pid=-1` into the next game,
  create an unnecessary zero-frame bridge session, and recover only afterward.
- Invalidate the stale source identity while waiting, then restart game launch
  admission from its grace period after the DS4 reader is restored.
- Retain an unhealthy reader's identity until cooperative stop is confirmed,
  preventing duplicate RemotePlay readers after a transient stop failure.
- Continue automatic DS4 discovery after a retained bridge is removed cleanly;
  RC34 could exit at that point and require manual reinjection.
- Return to the game watcher when a title exits during bridge-ready polling,
  instead of terminating automatic mode during that launch/close race.
- Add RC and report-schema identifiers to both firmware reports, and log a
  stable name alongside each numeric game-session termination reason.

### Verified

- The published RC34 bridge followed FC26 into two later game PIDs without
  reinjection. Its bounded `-4` launch retries succeeded on the third attempt,
  exposing the reader-ordering defect above while confirming the original
  one-off game-switch admission fix.

## [0.1.0-rc34] - 2026-08-08

### Fixed

- Retry a fail-closed game admission snapshot up to five times. RC33 treated
  the first result as permanent for that PID, which could make a one-off game
  switch require closing the title and reinjecting the payload.
- Replace the structural gate's single controller-information prologue lock
  with stronger cross-process evidence: a successful live DS4 ABI call in
  RemotePlay plus matching same-firmware `libScePad` fingerprints. Known
  manifests retain their exact checks.

### Changed

- Removed the retired game-ptrace installer and cleanup, injected UDP receiver,
  cave-based diagnostics, data-cache experiments, libpad dump/audit payloads,
  original Ghostcontrol USB controller sources, launchers, and binaries.
- Reduced the supported build surface to the automatic bridge, read-only
  status ELF, and cooperative stop ELF; all targets build with warnings as
  errors and function/data garbage collection.
- Added a complete GPL-3.0 license, explicit Ghostcontrol attribution, focused
  architecture/firmware documentation, and official DS4 revision details.

## [0.1.0-rc33] - 2026-08-08

### Fixed

- Fixed DS5-first/DS4-second multiplayer. Source pad indices are per user and
  therefore cannot identify the game's global P1/P2 slot; RC33 selects the
  unique disconnected DS4-facing game entry without taking over the connected
  DualSense.
- Made the bridge follow the validated global player slot when `libScePad`
  regenerates the upper bits of a controller handle. Live JoJo testing
  confirmed DS5 as P1 and the wireless DS4 as an independent P2.
- Non-selected controller calls no longer consume the translated-input lease,
  keeping the native DualSense entirely on Sony's path.

### Diagnostics

- Expanded automatic compatibility reports with a schema version, resolved
  symbol addresses, hashes and bounded prefixes, libpad object metadata,
  client-table/cache prefixes, selection counts, and import module ownership.
- The read-only runtime audit now scans pad imports in every loaded module and
  records full bounded client entries while logging only semantic changes.

### Compatibility

- 11.60 is live verified with both controller connection orders. The existing
  rest policy is unchanged: PoorDS4 quiesces and exits before sleep, then
  must be reinjected after wake.
- 12.40 retains its separately verified exact manifest and the bounded
  structural gate, but the current no-ptrace path remains hardware-unverified.

## [0.1.0-rc32] - 2026-08-08

### Fixed

- Replaced the game-ptrace installer with a continuously running, no-ptrace
  path. A write-free six-millisecond attach was enough to black-screen JoJo;
  RC32 never attaches to or pauses the game.
- Allocated a game-owned 32 KiB anonymous bridge mapping through kstuff remote
  VM calls and redirected only validated eboot pad-import slots. No eboot or
  `libScePad` code, game heap, game thread, or game socket is modified.
- Dynamically selected the game pad handle matching the source DS4 pad index,
  leaving every other controller handle on Sony's original path.
- Reduced the post-module launch grace from 30 seconds to 1.5 seconds after
  live JoJo menu, battle, and relaunch tests.
- Reconnect publication now masks short source loss with neutral connected
  frames and performs in-place reader rediscovery instead of removing hooks.
- Suspend/resume cleanup now quiesces owned imports without ptrace, target
  syscalls, or unmapping. This removes the unsafe post-wake cleanup path.
- Added ownership-proven recovery when Payload Manager is killed. Recovery
  restores stale imports without ptrace and retains the unreachable 32 KiB
  allocation until game exit; a live test showed synchronous remote unmap can
  stall in this forced-kill state.

### Compatibility

- Added dynamic pmap discovery. A unique candidate must translate a live user
  address to the same bytes as mdbg; firmware-family offsets are only proven
  fallbacks.
- Added a bounded structural firmware gate for unlisted firmware. It validates
  wrapper shapes, internal targets, executable mappings, the client-table
  layout, selected handle, controller-information ABI, and eboot import owners
  before any write.
- Forced structural mode passed live on 11.60, proving the unknown-firmware
  admission path independently of the exact manifest.
- Logout/login handle replacement, short Bluetooth reconnect, and a JoJo
  DS4-plus-DualSense P1/P2 session passed without reinstall or health errors.
- Rest/wake followed by closing JoJo passed on 11.60. RC32 intentionally exits
  after its pre-sleep quiesce; reinject the ELF after wake to resume translation.
- 11.60 remains exact and live verified. 12.40 retains a separate exact
  manifest but the current no-ptrace architecture still needs hardware.

### Diagnostics

- Added source/game firmware reports, client-table probes, libpad dumps,
  runtime audits, dynamic-pmap evidence, bridge health counters, and explicit
  lifecycle/stale-recovery logging.

## [0.1.0-rc31] — 2026-08-02

### Fixed

- Eliminated an intermittent one-call false disconnect in native games. The
  live Pragmata session showed an uninterrupted DS4 reader and loopback stream,
  but the game hooks could exhaust four seqlock retries while a 120-byte frame
  was being published and briefly fall through to Sony's disconnected backing
  handle.
- All five pad-data hooks now use a longer bounded wait and, on extreme
  contention, return a best-effort translated snapshot with `connected=1`
  instead of exposing the backing handle for one frame.
- While the translated receiver is active, the controller-information hook now
  always publishes connected standard-controller metadata and returns success,
  even if Sony's backing-handle call transiently reports an error.
- Added contention-fallback and controller-information-result-override counters
  to periodic health and cleanup logs.

### Compatibility

- The exact 11.60 and 12.40 manifests and the bounded unknown-firmware gate are
  unchanged. The appended RC31 argument layout preserves stale RC23–RC30 bridge
  inspection and cleanup.

## [0.1.0-rc30] — 2026-08-02

### Fixed

- Restored the automatic no-popup direct game bridge. RC29's distinct VDA
  controller was rejected because LoginMgr necessarily displayed the
  controller-user assignment screen.
- Source selection now uses public `scePadGetDeviceId` and
  `scePadGetDeviceInfo` metadata and accepts only known Sony DS4 products.
  Live 11.60 startup rejected the connected-first `054c:0ce6` DualSense and
  selected the Invite user's `054c:05c4` DS4. All unidentified-controller and
  sole-handle fallbacks were removed.
- The game bridge now selects the exact validated source user/index even when
  a native PS5 title suppresses `scePadIsDS4Connected`. This applies to the
  separate exact 11.60 and 12.40 manifests.
- Sustained wireless loss no longer removes and reinstalls live game hooks.
  RC30 feeds neutral connected packets, replaces only the source reader after
  Bluetooth republishes the DS4, and continues the same receiver in place.
- Transient loss never exposes a disconnected frame to the title.
- Cooperative stop after a successful reader startup now exits successfully
  even if no game was launched.

### Lifecycle safety

- Suspend/standby/shutdown and resume-gap exits abandon remote cleanup. The
  game receiver's local watchdog withdraws input and exits, while the source
  reader's owner watchdog closes its client independently.
- Target-death checks occur before status reads or ptrace cleanup, and failed
  stale-hook recovery stops further attach attempts for that PID.

### Firmware status

- 11.60 exact manifest retained; RC30 source discovery and cooperative stop
  were verified live.
- 12.40 retains its distinct exact six-function manifest and earlier direct
  gameplay evidence. RC30's new reconnect/rest behavior remains hardware-
  unverified on 12.40.
- Unlisted firmware remains eligible only through the bounded structural ABI
  gate; no `11.xx` or `12.xx` range is claimed.

## [0.1.0-rc29] — 2026-08-02

### Changed

- Replaced the automatic game's five-function detour with a distinct virtual
  DualSense device. Automatic mode no longer patches or leaves a receiver
  thread inside a game process.
- The virtual device is bound to the source DS4's exact user, including a
  temporary Invite user. Only a unique physical `DUALSHOCK4` mapped to that
  user may be removed from MBus; ambiguous topology fails closed.
- VDA creation is confirmed from `DEVICE_ADDED`, not from the API return code.
  Firmware 11.60 returns `0x803b0006` even while successfully creating the
  device, which caused earlier VDA experiments to reject a working path.
- Real DS4 frames are inserted at 120 Hz. Short source loss produces neutral,
  connected input for eight seconds and never replays held buttons.
- Added lifecycle event monitoring and a streaming-only resume-gap failsafe.
  The first live candidate exposed and fixed a false wake detection caused by
  counting the normal initialization interval.
- The status ELF now snapshots the VDA supervisor and source-reader watchdog
  without consulting stale direct-game-hook metadata.
- Automatic logs now include exact source/physical/virtual IDs, Invite user,
  input/output/error counters, sequence freshness, heartbeat, and raw lifecycle
  flag state. Repeated snapshots are no longer counted as fresh reader frames.
- Supervisor and read-only status snapshots are published with same-directory
  temp files plus atomic rename, so FTP/status readers cannot observe a file
  between truncation and completion.
- The automatic ELF discards unreferenced legacy game-hook and assignment-test
  sections at link time. It retains only the VDA bridge operations it calls.
- Trap-byte restore, page-protection restore, and `PT_DETACH` are now checked
  and logged for every ShellUI operation used by automatic mode. Detach retries
  are bounded, and a failed virtual bind attempts to restore the physical
  device during the same attach.

### 11.60 live evidence

- With FC26, a profile-1 DualSense, and a DS4 on an Invite user, RC29 created
  and bound a separate virtual DualSense, uniquely mapped and removed only the
  physical DS4, and retained both the profile-1 DualSense and Invite virtual
  device in the system controller table.
- The user confirmed that FC26 displayed the second controller. The queued
  Invite-profile popup remained visible and blocked final input confirmation;
  the current candidate adds a bounded Circle-plus-release pulse through the
  bound virtual controller. That exact dismissal behavior is not yet claimed
  as verified.
- A sustained live run recorded thousands of source and VDI frames with zero
  read failures, zero VDI failures, a healthy owner watchdog, and no lifecycle
  transition. Final post-popup input, rest/wake, and 12.40 tests remain pending
  and are not claimed by this candidate.

## [0.1.0-rc28] — 2026-08-01

### Fixed

- Automatic mode now retries DS4 discovery at a bounded five-second interval
  for as long as it remains active. Earlier builds slowed to 30-second scans
  after six failures, making a controller connected later appear unsupported.
- A DS4 absent for ten seconds now ends the current game session cleanly, stops
  the reader bound to the old handle, rescans every logged-in user and pad slot,
  and reinstalls into the still-running game when any supported DS4 appears.
  This covers replacement controllers and handle/slot changes, not only a
  reconnect that happens to reuse the original handle.
- RC27's eight-second neutral-connected grace remains first in the sequence, so
  ordinary short wireless reconnects are masked without tearing down the
  working reader. Rediscovery begins only after the sustained-loss threshold.

### Live verification

- With FC26 and a profile-1 DualSense already running, automatic mode waited
  without misidentifying it. Connecting a profile-2 DS4 later selected the
  correct second user and installed into the still-running FC26 process; the
  user confirmed the DS4 worked as player 2.
- A 6.086-second reconnect logged `prompt_exposed=0`, and the user confirmed no
  disconnected screen appeared. A separate sustained loss expired at 8.007
  seconds, requested rediscovery at 10.005 seconds, restored the game hooks,
  selected the DS4's new handle, and reinstalled into the same FC26 PID.

## [0.1.0-rc27] — 2026-08-01

### Fixed

- Transient DS4 loss is now masked by a monotonic eight-second
  neutral-connected grace period instead of a frame-counted window that lasted
  about 0.75 seconds at the observed 120 Hz bridge rate. A measured wireless
  reconnect took 4.529 seconds, so RC26 exposed `connected=0` and the game
  displayed its controller-disconnected screen despite recovering normally.
- Disconnect diagnostics now record the configured limit, elapsed milliseconds,
  frame count, and whether the disconnected state was ever exposed to the game.
  The supervisor state also records `source_disconnect_grace_ms`.

### RC26 live evidence carried forward

- Firmware 11.60 passed normal input, cooperative cleanup/reinstall, forced
  supervisor termination, complete exit of both detached watchdog threads,
  verified stale-hook recovery, full game close/relaunch, and a two-minute
  post-relaunch run with zero receiver timeouts or bridge failures.
- The no-rest controller-order capture found one valid game-user handle and a
  correctly identified DS4 source. True two-player behavior and the exact
  rest/wake/close regression remain separate manual tests.

## [0.1.0-rc26] — 2026-08-01

### Fixed

- The in-game receiver now exits after eight consecutive 250 ms receive
  timeouts. RC25 only disabled spoofing and left its detached injected thread
  alive; live testing proved that closing the game after rest/wake could still
  kernel panic the console in that state.
- The allocation path deliberately remains the already live-verified target
  `malloc` path. Current Ghostcontrol source documents remote `mmap` injection as
  less stable on retail hardware, so the speculative RC26 mapping change was
  removed before deployment.

### Validation

- All five payloads build warning-free. The PS5-target Clang analyzer, ELF ABI,
  SDK dependency, copied-stub relocation, and copied-stub indirect-call checks
  pass. Live RC26 rest/wake regression is pending console recovery.
- The supervisor now records the optional `SceShellCoreUtilAppFocus` pattern.
  This is diagnostic-only until live evidence establishes its exact transitions;
  it will help distinguish close/relaunch teardown from a vanished PID.

## [0.1.0-rc25] — 2026-08-01

### Fixed

- Lifecycle detection now watches both `SceSystemStateMgrInfo` and
  `SceSystemStateMgrStatus`. Suspend, standby, shutdown, and ShellUI's shutdown
  transition abandon cross-process cleanup before the target can freeze or
  disappear; the existing scheduling-gap fallback remains active.
- Game discovery waits for `libScePad`, libc, and libkernel to load before a
  five-second launch grace begins. Retry delays back off, and a failed verified
  stale-hook recovery is quarantined until that game exits instead of repeatedly
  attaching to a damaged or half-started process.
- The injected game receiver thread is detached immediately. Installation fails
  and rolls back if its thread ID cannot be read or detachment fails.
- A timed-out remote call must reach a proven ptrace stop before saved registers
  are restored. System processes are never killed; if the current game itself is
  already irrecoverably stuck, only that game may be terminated rather than
  risking a console-wide failure by detaching unknown register state.

### Diagnostics

- The supervisor snapshot records both lifecycle flag open/poll results, raw
  patterns, decoded state, shutdown bit, cleanup policy, and exact stop reason.

### Verified

- A live 11.60 Pragmata run opened and polled both lifecycle flags, installed
  only after all game modules were ready, detached the receiver thread, and
  transported 7,321 frames with zero receiver timeouts.
- Cooperative stop restored all hooks, stopped the source reader, closed both
  lifecycle flags, and left the console, game, and loader alive.
- Rest/wake, full game close/relaunch, transient controller loss,
  controller-ordering, and RC25 on 12.40 remain manual hardware tests; this
  changelog does not claim them yet.

### Rejected after hardware regression

- Manual 11.60 rest/wake followed by closing Pragmata reproduced the kernel
  panic. RC25 is not a release candidate; RC26 addresses the receiver-thread
  and allocation-lifetime hazards exposed by that test.

## [0.1.0-rc24] — 2026-08-01

### Fixed

- The injected `SceRemotePlay` source reader now checks its supervisor PID once
  per second and exits after three consecutive failed liveness checks. This
  complements the game receiver watchdog: force-killing PoorDS4 can no
  longer leave a permanent source-reader thread behind.
- Source-reader threads are detached after creation, and a reader closes a pad
  handle on exit when that run created the handle itself.

### Diagnostics

- The supervisor state now preserves the source reader PID and args address.
  `poords4-status.elf` reports the reader's owner PID, readiness, liveness
  misses, watchdog exits, pad-close ownership, and current connected state in
  addition to the game receiver counters.

### Verified

- Live 11.60 Pragmata testing completed exact-manifest install and input,
  cooperative six-hook cleanup, and clean reinstall into the same game PID.
- A targeted `SIGKILL` reproduced Payload Manager termination. Within seconds,
  the game receiver changed to `active=0` with one watchdog deactivation and
  the source reader reached `ready=2` with three owner misses and one watchdog
  exit. The console and game stayed alive.
- Starting RC24 again validated and restored the stale RC24 hooks, then
  installed a fresh bridge into the same game PID with zero receiver timeouts.
- Full game close/relaunch, rest/wake, controller-ordering, and 12.40 RC24
  regression tests remain pending and are not claimed by this release.

## [0.1.0-rc23] — 2026-08-01

### Fixed

- Rest-mode, shutdown, and Payload Manager termination are now lifecycle
  events. The supervisor watches `SceSystemStateMgrInfo`, handles catchable
  termination signals, and has a wall/monotonic resume-gap failsafe.
- A dead game PID is never queried for receiver status or passed through
  ptrace cleanup. Suspend/resume and dead-process paths send the local UDP stop
  and abandon remote cleanup; inactive hooks fail open to Sony's originals.
- The in-game receiver has a 250 ms socket timeout and withdraws spoofed input
  after two seconds without supervisor packets. This also protects a game if
  Payload Manager kills PoorDS4 without allowing signal cleanup.
- Ptrace detach now uses the PS5 SDK's `PT_DETACH(pid, 0, signal)` convention,
  records its result in every game report, and rejects an install whose final
  detach fails. A failed successful-install detach now rolls back all six hooks
  while the game is still stopped before retrying detach. A two-second launch
  grace avoids repeatedly tracing an eboot while its libraries are loading.
- Cleanup retries intermediate and final detach operations, restores the game
  detours even when receiver-stop confirmation is unavailable, and will not
  reinstall into the same live game after cleanup fails. This closes another
  path to the repeated `EEXIST`/stale-hook state seen in the 12.40 logs.
- Brief wireless drops receive up to 750 ms of neutral connected frames,
  preventing a transient Bluetooth loss from producing stuck buttons or an
  immediate in-game disconnect dialog.
- The source reader no longer treats any connected 12.40 pad as a DS4. Only the
  public DS4 predicate or the verified 11.60 identity table may distinguish a
  DS4 among multiple controllers.
- Game-side type-0 handles are enumerated across indices 0–7. A uniquely
  identified DS4 is selected first and a sole valid handle is the only
  unidentified fallback. Source indices are not reused across processes. This
  lets a recognized DS4 win when a DualSense connected first while ambiguous
  DS4-v2 layouts fail closed.
- RC22 stale args are read only through their original prefix; RC23's appended
  watchdog fields can no longer over-read an older remote allocation.

### Added

- Unlisted firmware may use a guarded structural ABI manifest when all five
  wrapper meanings, decoded shared internal targets, executable mappings, and
  the exact 20-byte controller-information prologue plus a live output-layout
  probe match. Any mismatch fails before hooks are installed.
- Every reader start archives a bounded six-function `libScePad` fingerprint
  as `/data/poords4/reports/source-fw-XXXXXXXX-pid-N.txt`, even when no
  native game is running.
- Status and health reports include the bridge layout marker, receiver timeout
  count, watchdog deactivations, packet count, and selected game pad index.

### Validation

- Clean warning-free PS5 builds completed for all five release ELFs, and the
  PS5-target static analyzer is clean on every modified C file. ELF inspection
  confirms x86-64 FreeBSD PIE output and no direct calls, RIP-relative data
  references, or relocations in the copied game-stub range.
- Hardware validation of RC23 rest-mode recovery and multi-controller ordering
  is still required; RC22 supplied the failure evidence this release addresses.

## [0.1.0-rc22] — 2026-08-01

### Fixed

- Game-hook cleanup now falls back to a one-time ptrace write when retail game
  processes reject the pre-attach `receiver_stop` write with `EPERM`. This
  fixes stop/restart leaving the five `libScePad` wrappers detoured.
- Automatic startup can recognize and recover a stale PoorDS4 bridge. It
  requires the exact five detour shapes, gateway-derived args, bridge magic,
  matching function addresses and remote-block ranges, firmware-specific saved
  wrapper bytes, and controller-information prologue before restoring anything.
- Cleanup failures now log the exact stage, wrapper index, errno, PID, and args
  address instead of returning an unexplained error.
- `scePadIsDS4Connected` now treats any positive return as connected; live
  11.60 returns a handle-derived value such as `0x030d0301`, not literal `1`.
- Game-local `scePadIsValidHandle` likewise requires a positive success value
  instead of over-constraining future exact firmware manifests to literal `1`.

### Verified

- Live 11.60 recovery restored an RC21 bridge left in Pragmata, then installed
  RC22 into the same process. A normal RC22 stop restored all hooks with
  `game bridge remove=0`; an immediate clean restart installed again and
  returned to `state=active` without stale recovery or a game restart.

## [0.1.0-rc21] — 2026-08-01

### Fixed

- Restored the proven asynchronous `scePadOpen` behavior used before RC17.
  The pad service may create a type-0 client before it associates the wireless
  controller; RC18-RC20 rejected and closed that valid handle while its
  connected and identity fields were still zero.
- A newly opened handle is retained only when it is the single successful
  candidate on an exact supported firmware. The reader then waits for a live,
  connected state and, on 11.60, verifies the populated Sony DS4 VID/PID before
  permitting any game hook.
- Immediate `scePadReadState` validation now runs on both exact manifests and
  process enumeration is recorded in the bounded log.

### Verified

- Live PS5 11.60 test with Pragmata: original Sony DS4 `054c:05c4` detected,
  exact game manifest accepted, all five read wrappers and controller-info
  hook installed, more than 9,000 frames transferred without read/write
  failures, a physical button event observed, and the game-side snapshot
  recorded 8,450 receiver packets and 13,651 hooked `scePadReadState` calls.

## [0.1.0-rc20] — 2026-08-01

### Added

- Exact native-game manifest for PS5 12.40 (`0x12400009`), derived from the
  supplied running-game archive: six export offsets and 256-byte hashes,
  exact wrapper bytes, decoded internal targets, and controller-information
  prologue.
- Source-independent, read-only game probing while the wireless controller is
  unavailable. A running game now produces an archived firmware report even
  when controller discovery fails.
- Unknown firmware reports now include decoded internal targets and relative
  offsets before failing closed.

### Fixed

- Removed the old generic diagnostic's invalid assumption that a mapped PS5
  SPRX begins with an in-memory ELF header; this caused the 12.40 archive's
  `error=module_bounds` result.
- On 12.40, a newly opened controller handle can be selected only after a live
  `scePadReadState` succeeds and reports `connected=1`. Empty probe handles are
  still rejected and closed.
- The 12.40 game handle is validated through `scePadIsValidHandle`; the private
  11.60 client-table layout is not guessed on another firmware.

## [0.1.0-rc19] — 2026-08-01

### Fixed

- Restored wireless DS4 discovery for DS4 v2 (`054c:09cc`) and Sony's DS4
  wireless adapter (`054c:0ba0`) on 11.60. The firmware's
  `scePadIsDS4Connected` implementation only recognizes the original
  `054c:05c4` device ID.
- Restored a conservative pre-RC17 compatibility fallback when exactly one
  pre-existing controller handle is available and the firmware API cannot
  identify it. Handles created by discovery probes are never accepted by this
  fallback.
- Native games now resolve their own type-0 pad at index 0. The source DS4's
  `SceRemotePlay` index is process-local and must not be reused in the game.
- Clean Windows builds once again use the PS5 compiler wrapper automatically.
- A newly opened source handle is closed if reader installation fails.

## [0.1.0-rc18] — 2026-08-01

### Fixed

- Unattended controller discovery uses a short initial retry window, then
  backs off to 30 seconds.
- The runtime log is bounded to a current 1 MiB file plus one rotated backup,
  preventing an asleep controller from growing `/data` without limit.

## [0.1.0-rc17] — 2026-08-01

### Added

- Wireless DS4 discovery across every logged-in user and pad index 0–7.
- Positive source identification through `scePadIsDS4Connected`, with the
  selected user and index carried into the native game's handle lookup.
- Per-firmware, per-game diagnostic archives under
  `/data/poords4/reports/`.

### Fixed

- A DualSense opened first no longer forces the bridge to use index 0.
- No-game polling no longer truncates the last useful firmware report.
- A fresh `SceRemotePlay` process can open a candidate handle when no existing
  handle is available, and closes newly opened non-DS4 candidates.
- The stop payload also shuts down pre-RC17 watchers that used
  `/data/poords4/`.

### Compatibility

- Native-game writes remain enabled only for the exact verified PS5 11.60
  manifest. Supplied 10.40 and 10.60 logs prove the API-resolved reader starts,
  but did not include a running game's required `libScePad` fingerprint.
