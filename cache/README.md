# Shader cache (optional)

Without these files the game still runs. It builds its shader cache while you play, so the
first races stutter until it has seen every shader. Leaving the game in a menu does not fill it:
shaders compile when something first draws with them. Play a few varied races (tracks, day and night,
weather, car classes, replays) and quit from the game's own menu, so the pipeline list is saved; the next
start prepares those pipelines while the game loads.

The three files here were recorded on a PS5 while playing and are copied next to `eboot.bin`
by `build.sh` when present:

- `grid.dxvk.bin` and `grid.dxvk.lut`: DXVK's shader cache
- `grid.pipelines`: the list of graphics pipelines to prepare while the game loads

They are generated from GRID's own shaders and only fit the GOG version of the game on this build. They are
published as a separate download next to the release, not inside this repository, and are
listed in `.gitignore`.

To make your own: play for a while, copy the three files from
`/data/homebrew/PPSA99779/data/` on the console into a folder, then run
`ps5/make_shipped_cache.sh <folder>` and copy the result back into this folder.
