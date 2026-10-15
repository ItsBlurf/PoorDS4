# Contributing

Bug reports and focused compatibility fixes are welcome. Start with the
[usage guide](docs/USAGE.md), [build instructions](docs/BUILD.md) and
[source guide](docs/ARCHITECTURE.md).

## Changes

Describe the observable problem and the behavior after the fix. Prefer shared
controller/API invariants over title-specific exceptions. Preserve native
DualSense input, PS4 passthrough, controller ownership and normal shutdown.

Add a regression control that fails for the demonstrated cause and passes after
the repair. Use current production functions where possible, and keep modeled
Sony behavior explicit. Run the host suite and build all three payloads.
Changes to copied callbacks also need final machine-code/ELF inspection and
appropriate console evidence.

## Reports

Provide the firmware/build, title version, controller models and connection
order. Include fresh diagnostics with private account/network information
removed. A transfer receipt, startup log, controller packet and actual gameplay
observation prove different stages; describe which one was checked.

## Licensing

Keep existing notices and attribution. Identify external source/version and
license when proposing code reuse. Contributions are distributed under the
project's [GPL-3.0-or-later license](LICENSE).
