# Development input fixtures

The normal bridge uses real console controller input. Host regression tests
use explicit memory and Sony API models; see [testing](TESTING.md).

A legacy runtime development switch reads
`/data/poords4/simulated_pads.txt`. Keep this file absent during ordinary use
and physical compatibility testing. Existing channels can be selected with
`slots=0,2`, `slots=all`, or `slots=none`. Per-slot numeric keys include `btn0`,
`lx0`, `ly0`, `rx0`, `ry0`, `l2_0` and `r2_0`, with the final digit selecting
the slot. Sticks/triggers use byte values; buttons use a mask.

The parser is a legacy development interface, so use simple valid entries and
replace the file as a complete update. These formats and host models do not
establish physical controller enumeration, independent players or rumble.
Native DualSense channels retain their own path and a valid physical source is
still required for normal bridge admission.
