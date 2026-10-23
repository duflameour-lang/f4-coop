# f4-coop

Co-op multiplayer for Fallout 4 (pre-next-gen **1.10.163.0**, F4SE 0.6.23).
Host-authoritative state sync: each player runs their own game; a relay
server holds the world truth; other players appear as driven actors.

## Status

Research prototype in active development. Built and tested so far:
F4SE plugin skeleton, UDP transport, engine-level puppet spawn
(`PlaceAtMe`) and network-driven movement via `MoveRefrToPosition`
on the game thread, host relay with roles from `f4coop.ini`,
one puppet per remote player, save-load lifecycle gates.

## License and provenance

This project is distributed under **GPL-3.0-or-later** (see `LICENSE`),
with the **Modding and Linking exceptions** (see `EXCEPTIONS`) inherited
from the project whose code it incorporates:

- `vendor/f4mp/` — `common/`, `server/`, `bot/` from
  [F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer)
  (GPL-3.0-or-later + Modding and Linking exceptions), vendored for a
  port to the pre-next-gen 1.10.163 runtime. Upstream targets next-gen
  1.11.240; do not merge blind - offsets and layouts differ.
- `vendor/enet-1.3.18/` — [ENet](https://github.com/lsalzman/enet)
  1.3.18 (MIT), vendored, SHA-256 of the release tarball:
  `28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd`.

Original f4-coop plugin code (`src/`, `tools/`) is (C) 2026 the f4-coop
authors and is distributed under GPL-3.0-or-later with the same
exceptions.

## Build

- `build.bat` — the F4SE plugin (`f4-coop.dll`) and the legacy
  echo tools against `..\f4se-0.6.23` headers.
- `build-rig.bat` — the game-free test rig: `build\F4MPServer.exe`
  (standalone relay) and `build\F4MPBot.exe` (bot player) from the
  vendored sources.

Run the rig: `build\F4MPServer.exe --loopback-only`, then
`build\F4MPBot.exe --server 127.0.0.1:7779 --name Bot`.