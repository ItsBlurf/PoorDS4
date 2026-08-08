# Attribution

PoorDS4 would not have been possible without
[Ghostcontrol](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher)
and its author, StonedModder. Ghostcontrol supplied the original project base,
controller research, PS5 process-injection groundwork, and the early pad bridge
work from which PoorDS4 was developed. The public repository intentionally
remains a GitHub fork so that this relationship is visible alongside this
notice.

PoorDS4 also depends on and gratefully acknowledges:

- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), maintained by John
  Törnblom and its contributors, for the PS5 payload toolchain, headers, loader
  ABI, kernel helpers, and mdbg interfaces;
- [kstuff-lite](https://github.com/EchoStretch/kstuff-lite), maintained by
  EchoStretch and based on the original kstuff research, for the remote-syscall
  ABI used to allocate the game-owned bridge mapping.

The current tree is deliberately limited to the wireless DualShock 4 bridge.
Ghostcontrol's USB controller implementations, launchers, research documents,
and compiled binaries are not redistributed here; their authorship remains in
the shared upstream Git history.

PoorDS4 is distributed under GPL-3.0-or-later. See [LICENSE](LICENSE).
