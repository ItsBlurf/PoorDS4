# Testing

## Host regression suite

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

The suite extracts named production functions and compiles them with explicit
Sony API and memory/transport models. It covers:

- State and queued reads, sample bounds, native passthrough and disconnected sources.
- Empty-read continuity without duplicate events.
- Connection metadata and source generations.
- Import/NID matching, ordinary relocations and SDK symbol extents.
- Controller attribution, high client indexes and source-user ordering.
- Anchored table discovery before clients open, first information queries,
  recreated handles, duplicate-user clients and native index reuse.
- Four independent production reader feeds through queued and state acquisition,
  and distinct per-player packets through all five game input callbacks.
- Source rediscovery, handle ownership, snapshot interleavings and image recovery.
- Delayed metadata after a successful open, bounded probe lifetime, pending-probe
  shutdown and close-failure handling, plus bounded V1/V2 discovery status reads.
- Trigger request/query guards and concurrent per-player cache publication.
- Existing feedback routes and per-controller output isolation.

`tests/checks/` contains the compiled controls. `tests/fixtures/client-layout.json`
is a small, readable table derived from a reported layout: its users are sample
identifiers, padding is cleared, and its source provenance is retained.

The optional `check_trigger_effect_readback.py` accepts `--reference` pointing
to a separately obtained KytyPS5 `controller.cpp` for a differential comparison.
The default suite requires no downloaded game files, private logs or console.

## Native validation

For a changed build, verify source connection and packet progress before testing
application callbacks. Check representative state-read and queued-read consumers,
application handoff, normal shutdown/reinjection and native controller isolation.
Keep the ELF/source hash and relevant output with the result.

SDK compilation and final ELF inspection establish build/ABI properties.
Host models establish the tested code contracts. Console logs establish the
observed source and game paths. Physical button response, controller join order
and motor delivery require their own field evidence.

Changes with those software/runtime checks complete can still need broader
gameplay testing. Report that remaining scope alongside the test result.

The current 11.60 release line has user-confirmed Eden input and Isaac
Repentance gameplay with one DS4 and with a DS4 plus DualSense. A saved
two-player Isaac run was also compared with the game's parser and live player
assignments; its reconnect screen cleared after starting a new one-player run.
That saved-session behavior is not evidence of a controller bridge regression.
Several physical DS4s, additional join orders and wider firmware combinations
remain separate field checks.

Mixed DS4/DualSense gameplay has also been checked with either controller as
player one, reconnecting either device, and reopening a saved co-op run. Both
players retained independent input. These checks used one DS4 and one DualSense.

The latest table-locator correction is covered by longer lookup loops, both
record strides, extended registers, native-first ordering, high client indexes,
invalid records and ambiguous-table rejection. Captured failing instructions
select the correct table in host and PS5 controls; the previous locator selects
a synchronization record instead. The copied reader and game callbacks are
unchanged from the earlier gameplay-tested build. Live Isaac reads also accept
connected DS4 input on the console. Gameplay in the affected tester setup still
needs a retest.
