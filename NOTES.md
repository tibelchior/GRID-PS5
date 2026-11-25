# Race Driver GRID on PS5: status notes

Approach: same as DiRT 2/3 (static recompile of the retail exe, DXVK d3d9 + PS5 Vulkan stack).
The recompiler is the DiRT 3 tree (`../dirt3-recomp-src`); the GRID changes live in `../grid-recomp-src` (the working copy used for development) and are
published as `ps5/patches/grid/0001-grid-recomp.patch`, regenerated with `tools/make_grid_patch.sh`. Always regenerate it after changing that tree.
The PS5 packaging follows `../DiRT3-PS5`. `work/` (unpacked exe, first analysis) is ignored by git.

## Done (first spike)
- `GRID.exe` (GOG, SHA-256 40350b08...) is UPX-packed. Unpacked with `upx -d` ->
  `work/GRID_unpacked.exe` (13.9 MB, SHA-256 954e9dca...). Use this as the lift input.
- Unmodified DiRT 3 recompiler lifts it without crashing (53 s), but the analysis is poor
  (see `work/first-lift-analysis.md`).

## Findings: GRID vs DiRT 2 (same toolchain, linker 8.0 / VS2005 for both)
| | DiRT 2 | GRID |
|---|---|---|
| Build date | Nov 2009 | Oct 2009 |
| .text size | 14.5 MB | 8.5 MB |
| `.reloc` section | yes, 313,745 entries | **stripped** (fixed base 0x400000) |
| Jump tables detected | 786 | **0** (likely depends on relocs; unconfirmed) |
| Code coverage | 91.5% | **55.9%** (6 MB "unclaimed" in .text) |
| Functions | 160k (33k by calls, 36k by relocs) | 85k (1.5k by calls, 0 by relocs) |
| "Dead" functions | 91k | 83k (similar, so gap-scan noise is normal, not the problem) |
| Extra section | - | `.secu` (executable, 0.29 MB): code-obfuscation section, NOT active protection (see below) |
| Graphics | D3D9 + D3D11 (dxgi, d3dcompiler_42) | **D3D9 only** (d3dx9_41) |
| Online / GFWL | xlive, winsock | none (dbxlive32, ws2_32, iphlpapi only) |
| Other imports | identical set: dsound, ddraw, npclient, dbxlive32, wintrust, crypt32, nvapi, openal32, binkw32, vorbisfile, dinput8, xinput1_3 | same, minus winmm/winsock/xlive/d3d11 |

Takeaway: the import surface is a subset of DiRT 2's, so the existing shims should cover it
(plus d3dx9_41). `.secu` checked: plain unencrypted x86 (no decryption, no anti-debug). 1180 functions in `.text` start with
`jmp` into it; the bodies were moved there, padded with int3, and call normal game functions. One stub at
0x125B400 is a spinlock/TLS-chain hook, only needed if something calls it. The recompiler already treats
`.secu` as code (7436 functions lifted), so it needs no special handling.
The only real blocker is the missing `.reloc`, which starves function discovery and
(probably) jump-table detection. Everything else is smaller than DiRT 2.

## Recompiler fix (done): `../grid-recomp-src`
Copy of `dirt3-recomp-src` (tools only) with pseudo-relocations for images that have no `.reloc`:
- `pe_image`: no reloc dir -> aligned image pointers in non-exec sections become relocs; `has_reloc()` accepts any slot
  (fixes jump-table detection, which required a reloc on every table slot).
- `discovery`: imm32/disp32 fields of decoded instructions that hold code pointers also seed functions
  (set `RECOMP_NO_CODEFIELDS=1` to disable). Seeds are rejected if they start with 00 00 or 0xCC, or decode to invalid bytes.

| | before | after | DiRT 2 |
|---|---|---|---|
| Jump tables / entries | 0 / 0 | 511 / 3993 | 786 / 6616 |
| Unreferenced functions | 83,511 | 15,276 | 91,460 |
| Functions reaching undecodable bytes | 0 (but 40% unseen) | 0 | 8 |
| Live noreturn functions | - | 1,064 | 1,357 |

The earlier "55.9% code coverage" was a reporting artifact: the report spans the gap between `.text` and `.secu`
(.rdata/.data/.rsrc, 6.1 MB), counted as unclaimed. Real code coverage is about 99.9%.
Reproduce: `recomp lift work/GRID_unpacked.exe --out <dir> --entries <empty> --hooks <empty> --include-dead` (30 s).

## macOS bring-up (grid-recomp-src, builds with `cmake --preset macos-arm64`, then `-DCMAKE_EXE_LINKER_FLAGS=-L<deps>/lib`)
Builds and links. Run (use an ABSOLUTE game path: a relative `--game` breaks the C:\Dirt3 mapping):
`build/macos-arm64/dirt3 --game /Users/tbelchior/PS5-DEV/ps5-ports/GRID-PS5/gamedata/GRID`
Game folder = symlinks to the GOG files + `grid_game.exe` (the unpacked exe). Runtime now loads `grid_game.exe`.
Changes vs DiRT 3: guarded the DiRT 3 xlive hooks (`f_00401070/80`), d3dx9_43 -> d3dx9_41, new `shims/misc/grid_stubs.cpp`
(CryptoAPI, ddraw, toolhelp, GetAdaptersInfo, WSARecv, D3DXGetShaderOutputSemantics), user32 GetMessage/PostMessage/timers/etc.
Reached so far: guest entry, 12+ worker threads, DXVK D3D9 device on Metal, 1280x720 window, first Present, runs ~11 s.
**Crash ~1 s after first Present: FIXED (root cause was a shim, not discovery).** Chain, found with lldb watchpoints:
- GRID runs an anti-cheat scanner job (f_0080D7A0 / f_007F96B0 / f_007F9760): EnumWindows + GetWindowTextA/GetClassNameA and
  EnumProcesses + GetModuleBaseNameA (API names stored Caesar+1, e.g. "FovnXjoepxt"), each name checked with shlwapi StrStrIA
  against an encrypted blacklist (first entry "Cheat Engine"; some entries are empty strings).
- Our StrStrIA shim matched an empty needle against any text (Windows returns NULL). So every window "matched", the scanner set its
  quit byte ([obj+0x84], obj = state+0x2414), the race-state step (f_00644D20) posted the internal Exit message (vtable 0xCD83DC),
  the main loop f_005CC530 quit ("[SYSTEM]: Exit Message Received"), and the state job ran its cleanup slot (f_00654250), deleting
  half-initialised objects -> null read at guest 0x10 in f_0049FEB0.
- Fix: `shims/misc/misc.cpp` StrStrIA returns 0 for null arguments or an empty needle.
- Re-lifting with `RECOMP_NO_CODEFIELDS=1` gave the identical crash, so code-field seeding was not the cause.
**Lifter bug found next (fixed in tools/recomp/codegen/fn_lifter.cpp):** when discovery splits a function at a jcc (e.g. f_004293BB
starts with `jz` after `test esi,esi`), the caller tail-calls the split part, which reads flags from `c->eflags` ("flags live on
entry"), but the caller never packed them. Liveness only looked at in-function successors. Now direct exits (fallthrough/jump/
tailcall/jcc) to another function count all arithmetic flags as live, and `emit_exit_to` packs `c->eflags` before leaving.
Symptom was a null InterlockedDecrement(esi+4) on thread 308 right after the anti-cheat fix.
**Result after both fixes (2026-11-19):** no crash in 45 s runs, ~300-420 fps, reaches the legal screen and then the front-end
profile ("enter name") screen. Capture frames with `DIRT3_SCREENSHOT_DIR=<dir> DIRT3_SCREENSHOT_EVERY=1500` (BMP back buffer).
**New issue:** heavy rendering corruption: horizontal streaks, blocks in the wrong place, image shifted/cropped left. Not yet
investigated (suspect render-target/surface copy (StretchRect/UpdateSurface) or texture format/pitch handling in the d3d9 bridge).
Debug aids: `GRID_STACK=1` (guest code-address scan on every SetEvent), `DIRT3_LOG_WAITS=1` (now also lists the handles of
WaitForMultipleObjects), `DIRT3_LOG_MSGS=1`, `DIRT3_LOG_FILES=1`, `RECOMP_TRACE=1` (huge). lldb: launch, not attach;
`process handle SIGSEGV -n false -p true -s false`; guest memory = host 0x300000000 + guest address; registers via
`((X86*)$x0)->ecx` in an `f_XXXXXXXX(X86* c)` frame; source-line breakpoints on `/* ADDR:` comments in generated/src.
Also seen: "Could Not Get Primary Adapter Handle" (nvapi, harmless so far), NPClient.dll absent (fine).

## Resolution (1440p works)
GRID caps selectable resolution via `<res mem="270" maxWidth="1280"/>` in `system/hardware_settings_restrictions.xml`, so the game
fell back to 1280x720 whatever the config said. `shims/frontend/menu_patch.cpp` now serves an overlay copy with maxWidth=7680
(game folder untouched). Set `<resolution width/height/oldWidth/oldHeight>` in the saved `hardware_settings_config.xml` (or pick it
in the graphics menu on first run); verified `CreateDevice 2560x1440`. Render modes offered = 0.5x..2x of the display.
For PS5 pick the default per output (1080p/1440p/4K) when the stack is wired up.

## Lifter: missing functions after the first minute (fixed)
Symptom: `FATAL call to unknown target 0x00862890 / 0x004E9BA0` (a function never lifted). Cause: without `.reloc` every jump-table slot
counts as relocated, so a table could run one slot too far into the next function whose first bytes (`81 EC 8C 00` = 0x008CEC81)
look like a code pointer; discovery then erased that function as "table data". Fixes in `tools/recomp/analysis/discovery.cpp`:
a slot listed in `--entries` ends a table, and a function that is a direct call target is never erased as table data.
`cfgGRID/entries.txt` holds `00862890`. Debug: `RECOMP_DEBUG_TABLE_ERASE=1` lists functions erased as table data (16 left, none called directly).
Runtime misses still appear in `~/Library/Application Support/Dirt3Recomp/recomp_discovered.txt`; add them to the entries file and re-lift.
Result: 100 s run, 15,000 presents, ~210 fps at 2560x1440, no crash.

## PS5 build (working on the console)
`./build.sh` builds `dist/PPSA99779/` (eboot.bin 140 MB + sce_sys; the game folder is copied only with `COPY_GAME=1`).
Source = `../grid-recomp-src` + `ps5/patches/dirt3/0001-ps5-port.patch` + `ps5/overlay`. Install: copy to /data/homebrew/PPSA99779,
game folder as `GRID/` inside it with the unpacked exe as `grid_game.exe`. Data/logs in `<title>/data`: `boot.txt`, `grid.log`
(errors only unless an empty file `verbose` is there), `output` (1080/1440/2160), saves and settings under `C/Users/Player/Documents/Codemasters/GRID`.
Shader cache files are `grid.dxvk.bin/.lut` and `grid.pipelines` (play a few varied races, quit from the game's own menu).

### Console features (all verified on the console)
- PlayStation button glyphs: `shims/frontend/glyph_patch.cpp` (GRID's icons are 32x32 DXT5 without mip, 1024 bytes).
- Graphics menu: the Aspect Ratio row is now "Statistics" Off/On (`ps5/overlay/.../grid_stats_row.cpp`: language text, options xml);
  the "restart needed" decision in states.bin (`pc_check_restart`) always answers no, since a title cannot relaunch itself.
- Name entry: IBTextEntry's update (0x0049F870, in `cfgGRID/hooks.txt`) is hooked in `shims/input/dinput8.cpp`; it opens the
  console's on-screen keyboard and sends the answer as WM_CHAR (8 = backspace, 13 = return, 27 = escape). GRID takes typed text only
  through WM_CHAR (window proc 0x00A9C730 -> 0x00A9C290). `MapVirtualKeyA`/`ToUnicode` are real (US layout), buffered DirectInput
  keyboard events work.
- Naming: settings in `Codemasters/GRID`, log `grid.log`, cache `grid.*`.

## Next steps
1. Fix the rendering corruption on the front-end screens, then get past the profile screen (input) to the main menu.
2. Play more on the console and fix what `grid.log` reports (add `unknown target 0x...` addresses to `cfgGRID/entries.txt`).
