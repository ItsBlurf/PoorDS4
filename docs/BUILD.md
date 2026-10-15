# Building PoorDS4

## Requirements

- ps5-payload-sdk **v0.43**.
- LLVM/Clang and LLD compatible with that SDK; current Linux and Windows builds use **23.1.3**.
- GNU Make.
- Python 3.10 or newer and GCC/Clang for host tests.

Download the SDK from its [official release](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.43).
Keep the SDK outside the repository. The SDK's `toolchain/prospero.mk` selects
the PS5 compiler and linker on Linux/WSL.

## Linux or WSL

```sh
export PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk
make -C payload
make -C payload status stop
```

For distributable binaries:

```sh
make -C payload release
```

The release target removes debug information from all three payloads.
Outputs are written to `payload/` and ignored by Git.
The main payload is `PoorDS4.elf`; its runtime reports identify the release
version declared in `payload/version.h` (currently **0.2.0**).

## Native Windows

Native Windows requires a working PS5 SDK adapter providing `ps5-clang.cmd`
and `llvm-strip` on PATH. It must target `x86_64-sie-ps5`, use the SDK sysroot
and linker script, and supply emulated TLS and the PS5 CRT. The upstream SDK's
shell scripts cannot be run directly by `cmd.exe`; use WSL for an upstream-only
setup.

```powershell
$env:PS5_PAYLOAD_SDK = 'C:\ps5-payload-sdk'
make -C payload release
```

## Host checks

These checks compile production controller code with modeled Sony/transport
calls. They do not require the PS5 SDK or a console.

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

Select a host compiler with `POORDS4_HOST_CC`. Keep host compilers separate from
the PS5 compiler used for payloads. See [testing](TESTING.md) for coverage and
[architecture](ARCHITECTURE.md) for the source layout.
