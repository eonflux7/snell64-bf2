# Fixes worth upstreaming

Engine bugs found while running the x64 client on Linux/Proton (DXVK) with Project Reality and
vanilla BF2, most of them inherited from OpenBF2. The fixes are standalone commits so they can be
offered to kr4uzi/OpenBF2 or B2BF/OpenBF2; none has been submitted, and the owner decides what goes
([AGENTS.md](../../AGENTS.md#upstreaming)). Each entry says whether it was checked against the
retail binary.

Status: **open** = reported here, no fix on this branch; **fixed** = commit on `main` (before 2026-11-17 the branch was `pr/x64-local` of `eonflux7/OpenBF2`).

## Runtime and packaging (found on release v0.2.0-rc2)

### A. x64 under Wine/Proton: only terrain renders, then assert `BundledMeshTemplate.cpp:1115` (open, packaging)
`MetaFx::generate` builds the Ra permutation tables with `createEffectCompiler`, which loads
`d3dx9_24.dll`. Under Wine this resolves to Wine's builtin, whose `ID3DXEffectCompiler` reflection is
stubbed: the load succeeds, every lookup returns nothing, and all three meta-effects yield zero
permutations. Static meshes are invisible and bundled meshes assert.
- Fix: ship Microsoft's x64 `d3dx9_24.dll` (`Feb2005_d3dx9_24_x64.cab` in the June 2010 redist) and
  override it native (`WINEDLLOVERRIDES=d3dx9_24,d3dx9_25=n`). Verified: 190 / 1056 / 84
  permutations, no misses.
- Suggestions: document it; treat a meta-effect with zero permutations as fatal; have
  `requireD3dxRuntime()` probe `d3dx9_24` with one reflection call. The line-1115 assert also loses
  its message text.

### C. x64 under Wine needs the native x64 `d3dx9_25.dll` (open, packaging)
Wine's builtin returns `E_NOTIMPL` from `D3DXCreateEffect` and the process ends silently. Same fix
as A (`Apr2005_d3dx9_25_x64.cab`). `requireD3dxRuntime()` only checks `LoadLibrary`, so it passes on
Wine's stub.

### D. The x86 package has no rebuilt `aidll.dll` (open, packaging)
The retail `AIDLL.dll` is loaded instead and single player crashes during map load
(`aidll.dll+0x24B27`, read at 0x10), then exits with code 42.

### E. Fatal errors are invisible to GUI launches (open)
`BF2Log::displayErrorWindow` writes only to stderr and calls `exit(1)`; under Proton or any Windows
GUI launch the game just disappears. Suggest a message box or an always-on crash log.

### F. `vsdevcmd.cmd` breaks under `Program Files (x86)` (open, build)
`%VSDEVCMD_BAT%` expanded inside a parenthesised `if` block: the `)` in `(x86)` closes it early
(`\Microsoft was unexpected at this time.`). Fix: move the echo out of the block or use delayed
expansion.

## Engine fixes on this branch

### G. gameswf `as_value::string_concat` reads a freed object (fixed, `c0da0465`)
`drop_refs()` runs before `to_tu_string()`, the retail order. Retail's collector keeps the object
alive until the next sweep; with native reference counting the temporary `String` is freed first, so
`new String("abc") + ""` yields `"<object 0x…>"`. Fix: convert, then release. Not checked further
in the binary. Possibly related: a `Number` concatenated into a string gives `0.000000` where retail
gives `0`.

### H. x64 bots: pure virtual call / AV from stale AI `WeaponInfo` records (fixed, `dd8b056c`, `bb755a11`)
`GenericFireArm::enableCachedObject` gives a recycled firearm a new AI `WeaponObject` (retail does
the same; checked in `bf2_w32ded.exe` 0x583DA0) and the old one's records still hold its `IWeapon*`.
- `~WeaponObject` now calls a new courier slot, `IAIInformationCourier::weaponDestroyed` (appended
  at +0x3C), which releases every record on that weapon. `BF2_AI_WEAPON_TRACE=1` logs releases.
- `IPIArmamentReal::getWeapon` refreshes when a record is gone but the engine still lists the
  weapon; both `getWeapon` overrides guard index -1.
- Open: why retail does not crash here.

### I. x64: `MapContainer::isValidPosition` indexes island 0xFFFFFFFF (fixed, `255e6e25`)
`getIsland(-1)` is unchecked in retail too; on x86 it reads 8 bytes before the vector, on x64 about
64 GB past it. An out-of-range island now falls back to the any-island overload. Other unchecked
`getIsland` call sites in `AIPathfinding.cpp` share the hazard.

### J. Pure virtual calls leave no stack in the crash log (fixed, `40ad62cb`)
`_purecall` aborts without an SEH exception. A `_set_purecall_handler` now logs registers and frames,
then returns so the abort still happens.

### K. Console `if`/`elseIf` compares quoted literals with their quotes (fixed, `2427c2f7`)
`if v_arg2 == "meinsurgent"` was always false.

### L. The Ra shader cache is shared across mods (fixed, `ea06cc6c`)
Retail keys the `.cfx` cache to `mods/bf2` for every mod, so stale vanilla blobs beat a mod's older
`.fx` timestamps. Each mod now has its own cache.

### M. SquadManager console bindings never registered (fixed, `456e3c5f`)
Compiled in, but not registered in `ModuleGame::init`: scripts got "Unknown object or method".

### N. Generated Ra permutation `.cfx` files never go stale (fixed, `82e04f61`, `c2ccd4bc`)
Edits to `.fx`/`.fxh` did not invalidate the generated permutations, so shared-header changes mixed
old and new shader code. The cache is now keyed on a hash of the preprocessed source and defines.
`c2ccd4bc` fixes a null compiler entry the first commit introduced.

### O. Console scripts: no `var`, `const` or assignment (fixed, `06696832`, `0fbe662b`)
`OldConsole::handleCommand` had no handling for `var`, `const` or `v_x = value`; they fell through
to the binding lookup as unknown commands, so declared variables never existed and conditions
compared the literal name. Now as `bf2_w32ded.exe` 1.50 does (line handler 0x651EFE..0x652188 and
its strings): `var v_name [= value]` (one or three tokens, `v_` prefix, "Variable redefinition" in
the same scope) and `const c_name = value` (three tokens, `c_` prefix, "Constant redefinition").
Plain assignment `v_name = value` is not located in the binary yet; mods rely on it (PR's
`setLayerMode.tweak`). Also missing were `return` (ends the running script) and `command -> v_name`
(0x652735..0x6527EF: exactly one `v_` name after `->`, the command's output stored in it); PR's
kit files guard with `ObjectTemplate.active -> v_exists` / `return`, and without them a second
faction pass (night layers) created kits twice and `Kit::init` aborted on the duplicate item index.
Operands of `if`/`elseIf`/`var`/`const` keep a double-quoted literal with spaces as one token
(`if v_exists != "No geometry template"`, PR's geometry guards); split at the spaces, the guard
always returned and soldier and kit geometries (the 1P arms among them) were never created. The
retail tokenizer for these operands is not located yet. Seen in game (The Falklands night layer:
kits created once, soldier geometry and 1P arms present).
Also found: retail runs a level's `Init.con` with every argument empty (`bf2_w32ded.exe` 0x465D50;
`BF2.exe` 0x4ED7xx passes `BF2Editor` only in editor mode), as this engine does.

### Q. StateManager indexes its mirrors with vertex-texture stages (fixed, `6eb1a727`)
The D3DX effect state manager's `SetTexture` and `SetSamplerState` (`StateManager.cpp`) index the
texture mirror (16 stages) and the sampler mirror (8 stages) with the stage D3DX passes, unchecked.
An effect that samples a texture in a vertex shader gets `D3DVERTEXTEXTURESAMPLER0..3` (257..260)
there, and the write lands far past the blocks: the game crashed in `blockSet` on the first such
`BeginPass` (found when the renderer's particles moved their hemi map lookup to the vertex shader).
Now stages outside the mirror go straight to the device, as `SetTextureStageState` already does for
the states it does not mirror. Not checked against the retail binary; no retail or PR effect samples
in a vertex shader, so retail never reached it.

## Build

### P. Plugin DLLs as incremental Ninja targets (`dd686d2c`, `64e160f2`, `d310968a`)
RendDX9, TextureAtlasBuilder, SwiffPlayer and BF2Audio build as CMake targets from shared source
manifests, with a post-link ABI check. A one-file RendDX9 change goes from 2.5–3 min to about 8 s.
x86 not built or tested.

## Diagnostics (optional)
- `8bb1b7fb`: log the D3DX compiler message when an effect fails to compile.
- `803d2983`: `BF2_CONSOLE_TRACE=1` traces console script runs and `if` results.
- `16c5bd8c`: `BF2_FILE_MONITOR` / `BF2_DEVCMD_FILE` developer hooks.
