# Project Reality compatibility

The `compat` lane: what PR 1.9's content, scripts and launcher need from the engine, so the x64
client can host and play PR offline. This lists engine-side contracts only. PR's own scripts,
shaders and Python shims are closed and are not part of this repository.

## What PR adds on top of retail BF2 1.50

- A patched 32-bit `PRBF2.exe` that loads `rhooks.dll`, a closed hook DLL that patches the retail
  executable at fixed addresses. It cannot load into OpenBF2. Its client features (HUD variables,
  FCS, bleeding, thermal fog, the ability bar and more) have to be provided by the engine instead.
- A Python **2.7.12** `dice_py.dll` with extra built-in modules. Most of PR's game scripts ship only
  as 2.7 bytecode.
- `_realitymemory`, a 32-bit Python extension that patches retail memory. Its game-logic side is
  engine source here, from realitymod's OpenBF2 PR #1 (merged as `c4a3a850`).

## Done

| Need | Engine side | Commits |
|---|---|---|
| Python 2.7 | CPython 2.7.12 as `dice_py.dll` for x64 (UCRT patches), engine compiled against 2.7 headers (`BF2_PYTHON27_SRC`), `pylib-2.7.12.zip` on `sys.path` | `d933fc0e`, `430b2eaa`, `a3540325` |
| PR's built-in Python modules | `_socket`, `select`, `_sqlite3`, `_ctypes`, `socket.acceptEx`, `_realitycore` compiled into `dice_py` | `0262df02`, `3b4e89c3`, `a168936c`, `f15d9ac9`, `30c8a76d` |
| HUD variables registered by rhooks | `BF2_HUD_AUTOVAR=1` registers unknown HUD variables on first use, aliasing names that exist with another type | `c7d66445`, `6739b5fd` |
| rhooks' HUD event channel | Game events 9–12 with mod subtypes are logged once and dropped under `BF2_HUD_AUTOVAR` instead of asserting | `cae8b9c5` |
| Launcher handshake | `BF2_CMDLINE_FILE` redirects the menu's command-line file, as `PRBF2.exe` does | `5d5e7d2b` |
| Squad names over rcon | `SquadManager.listSquads` / `listSquadMembers` console rows | `8242d255` |
| Layer-specific level init (night, skirmish, vehicles, coop) | `BF2_LEVEL_INIT_ARGS=1` runs the level `Init.con` with `v_arg1` = `host`/`client`, `v_arg7` = game mode, `v_arg8` = layer, which PR's `setLayerMode.tweak` reads; retail passes them empty. Needs the console's `var`/`const`/`return`/`->` and quoted operands ([fix.md](fix.md) O). Seen in game: The Falklands `gpm_cq` 32 loads its night layer (sun 0.07, night kits, 1P arms) | `2c5de840` |

Engine bugs that PR exposed are `fix` commits and are listed in
[fix.md](fix.md): gameswf `string_concat` (Issue G), console quoted
literals (K), SquadManager console registration (M), per-mod shader cache (L), console `var`/`const` (O).

## Missing

Parked (owner, 2026-11-19) while the renderer roadmap runs; offline play works without these.

- rhooks' client features beyond dummy HUD variables: FCS/rangefinder, bleeding and pain effects,
  thermal-imaging signalling, crouch toggle, ability bar, spawn block, black screen.
  Spawn readiness is currently forced from Python.
- The rhooks HUD event channel is dropped rather than handled.
- Joining servers needs PR's online login and anti-cheat; out of scope while the fork stays offline.
