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
- Source rediscovery, handle ownership, snapshot interleavings and image recovery.
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
