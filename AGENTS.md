# AGENTS.md -- Water Physics

A native Motor Town plugin: one DLL loaded by
[Plugin Loader](https://github.com/qxZap/mt-plugin-loader). Source is `plugins/water_physics.cpp`.
`include/` is a copy of the loader's headers; refresh it from mt-plugin-loader when the
api version changes. The source of truth is `D:\MTLiveMap\mt_loader\plugins\water_physics.cpp`; copy changes here.

## Build and test

`build.bat` (MSVC 2022 x64) writes `out\`. Install to `MotorTown\Binaries\Win64\plugins\`
with the game closed. There is no test suite: the log `plugins\mt_plugin_<Name>.log` in that
folder is the check, so log what you need to see in game.

## Rules

- The loader owns the ProcessEvent hook and engine discovery. Use `include/mt_plugin_api.h`,
  and never hook a vtable from a plugin.
- `process_event` only on the game thread (`run_on_game_thread`).
- Find fields by reflection (`api->field_offset`) and code by signature (`find_sig`), never
  by address. A missing field or signature logs a line and leaves the game vanilla.
- Hardcoded offsets are the exception and need a `shortcut:` comment naming the limit.
- Mappings come from the loader: `[Mappings] Export=1` in `mt_loader.ini`. After a game
  update recheck every signature and hardcoded offset.
- A plugin rewriting game data affects the machine running the game: single player or the
  host. On someone else's server that server's rules apply.
- Settings live in `Binaries\Win64\plugins\<plugin>.ini`, through the loader's ini functions (`iniFile` moves an old file over). `[General] Disabled=1` makes `PluginInit` start nothing. After
  renaming one, move the old file on start so players keep their settings.

## This plugin

Vehicles float: lift at four points of the footprint in proportion to depth, and drag against speed, so a vehicle tips and settles instead of bobbing; it takes on water and sinks over SinkSeconds. Set the sea level once with Ctrl+F8 at the water's edge (nothing floats until then).

- Known issue (friend report): ~20 FPS lost even out of water; fix before a release.
