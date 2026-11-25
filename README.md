# Race Driver GRID on PS5

Race Driver GRID (PC, GOG) running natively on a jailbroken PS5: a static recompilation of the game's
executable to C, with [mihawk-99's PS5 Vulkan stack](https://github.com/mihawk-99/PS5_Vulkan) and DXVK
for graphics. It is built on [XITRIX's DiRT 2 recompilation](https://github.com/XITRIX/Dirt-2-Recomp) and
follows the DiRT 2 and DiRT 3 PS5 ports step for step; GRID uses the same engine (Direct3D 9 only).

**You bring your own game files. Nothing from the game is in this repository.** The build
recompiles your `GRID.exe` (unpacked with UPX) on your own computer and packages the result as a PS5 title.

## Status

Playable on a console at 1440p: front end, profile, races. What works:

- PlayStation pad, with PlayStation button glyphs in the menus (Cross, Circle, Square, Triangle, L1/R1/L2/R2,
  Options, touch pad, L3/R3) and rumble.
- Resolution: the game caps its list at 1280 wide; the port raises the cap, so 1080p, 1440p and 4K can be chosen.
- A **Statistics** row in the graphics menu (it replaces *Aspect Ratio*): on-screen fps, frame time, draw calls.
- Changing graphics settings does not ask to restart the game (a title cannot relaunch itself); changes that
  need it apply the next time the title starts.
- Name entry (profile, team name) opens an on-screen keyboard driven by the pad.
- Shader cache: it builds while you play (see [cache/README.md](cache/README.md)), so the first races stutter.

Known gaps and the findings behind the port are in [NOTES.md](NOTES.md). If the game stops with
`FATAL call to unknown target 0x...`, see *Troubleshooting*.

## What you need

- A PS5 with homebrew enabled and a way to launch titles from `/data/homebrew` (tested with
  ShadowMountPlus), and FTP access to the console.
- A computer with **Docker**, about **60 GB** free disk space and 16 GB of memory.
- **Race Driver GRID**, PC (GOG): the folder that holds `GRID.exe`. The exe is UPX-packed; the build
  unpacks it (needs `upx` on the computer, `brew install upx`) or use a `grid_game.exe` you unpacked
  yourself. The unpacked exe must have SHA-256 `954e9dca446b7822c969063f79f4f2212f7defc8756c212508479271d2a286ec`,
  the file the recompiler was fitted to (the build checks it).
- The **DiRT 3 recompilation source tree** (the one the DiRT 3 PS5 port builds from) next to this folder as
  `../dirt3-recomp-src`, or set `DIRT3_SRC`. This repository holds only the changes for GRID
  (`ps5/patches/grid`) and for the PS5 (`ps5/patches/dirt3`, `ps5/overlay`), applied to a copy of that tree.

## Build

```bash
GAME_DIR="/path/to/Race Driver GRID" ./build.sh
```

Or copy the game folder to `gamedata/GRID/` and run `./build.sh`.

The toolchain is shared with the DiRT 2 and DiRT 3 PS5 ports: by default the build uses that port's Docker
volume (`dirt2ps5-build-data`), so a machine that already built DiRT 2 or 3 skips the 1 to 2 hour
toolchain step. GRID builds in its own folders inside the volume (`tree-grid`, `game-grid`).
On a new machine the first run builds the toolchain.

The result is `dist/PPSA99779/`. Steps can also be run alone: `./build.sh toolchain`,
`./build.sh lift`, `./build.sh title`. By default `dist/` holds only the PS5 files; `COPY_GAME=1` also copies the game folder into it.

## Install on the console

1. Copy `dist/PPSA99779` to `/data/homebrew/PPSA99779` with FTP.
2. Copy (once; skip it if the console already has it) your game folder, any folder name, into
   `/data/homebrew/PPSA99779/`, for example `/data/homebrew/PPSA99779/GRID/`. It must contain the **unpacked**
   executable as `grid_game.exe` (the build unpacks `GRID.exe` into `work/grid_game.exe`; copy that file
   into the game folder on the console), next to the original files. `COPY_GAME=1 ./build.sh` does both for you in `dist/`.
3. Let ShadowMountPlus rescan, then start **Race Driver GRID Recompiled** from the home screen.

Everything the port writes goes in `/data/homebrew/PPSA99779/data/`, beside the game folder (which is
never touched): `boot.txt` (start-up steps, written first), `grid.log` (the log), your profile and
settings. If the game does not start, `boot.txt` says why (for example "game folder not found").

Optional switch files, empty files or one-line files in the `data` folder:

| File | Effect |
|---|---|
| `novsync` | turn vertical sync off |
| `fpscap` | frame cap, the number inside the file (for example `30`) |
| `output` | output height: `1080`, `1440` or `2160` |
| `verbose` | keep the whole log (by default `grid.log` holds errors, crash reports and the game folder line only) |
| `bench` | log extra frame-time detail |
| `profile` | write a CPU profile to the log |

## How it is built

`build.sh` runs a Fedora container: it reuses (or builds) the PS5 toolchain from pinned
revisions (`tools/pins.env`), copies the DiRT 3 source tree and applies the GRID changes and the PS5 port
(`ps5/patches/grid`, `ps5/patches/dirt3`, `ps5/overlay`), recompiles your executable, builds SDL3, OpenAL Soft,
FFmpeg and DXVK 3.1.1 (`ps5/patches/dxvk`) for the console and links everything with RADV into
a signed title.

## Troubleshooting

- `boot.txt` lists the start-up steps; the last line is where it stopped.
- `grid.log` holds errors only. Create an empty file named `verbose` in `data/` for the whole log.
- `FATAL call to unknown target 0x00XXXXXX`: a function the recompiler did not find. Add the address
  (one hex address per line) to `cfgGRID/entries.txt` in the source tree used by the build (or to
  `ps5/patches/grid`) and run `./build.sh` again; the recompilation is cached and only redone when its inputs change.
- Stutter in the first races: the shader cache is still filling (see [cache/README.md](cache/README.md)).

See [NOTICE.md](NOTICE.md) for licences and credits.
