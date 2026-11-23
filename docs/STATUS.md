# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11, 13, 15, 16, 18 and 19 accepted; 12 (clouds) paused | Milestone 17 (far terrain): tasks 1–5 built and seen by the owner (fine for now); 6, bakes for more levels | [render.md](lanes/render.md#17-far-terrain) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q, R fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Current: `render` milestone 17, far terrain

Design and as built in [render.md](lanes/render.md#17-far-terrain). Tasks 1–5 built and seen by the
owner (2026-11-23: fine for now); Kunar Province, Kashan Desert and Muttrah City baked. Next: 6, bakes
for the other levels; 7, judged from a jet, cost with the final GPU pass.

## Paused: `render` milestone 12, clouds

Tasks 1–7 and 8's day sky done, 6 judged by the owner (2026-11-22; GPU time with the final GPU pass)
([render.md](lanes/render.md#12-clouds)). Open: 8's night layers (stars from a catalog with the moon, the
sky's rotation; the Yale Bright Star Catalogue has no stated licence, so it waits on the owner: a
catalog read from outside the tree, or another source); 9 (low scud, perhaps).

Reference views (`view.save` by the owner; milestone 11's shots `m11acc-<view>`, earlier
`m10-<view>` at 1600x900): Muttrah City and Fallujah West `sun`, `antisun`, `ridges`, `valley`,
`street`; Saaremaa the same with `water` for `street`; test_bootcamp (`gpm_cq 16`) `gloss`,
`gloss2`, `glass`. A view facing the sun has yaw = the survey's sun azimuth + 180°.

## Awaiting the owner's look

Built and checked by an agent; an entry goes when the owner has seen it.

- Clouds (milestone 12, contracts 32–33): no step at the 16 km handoff and no far radial edge
  (one air integral, a cross-fade band); edge albedo and the eroded light bake. Kashan Desert,
  views `hb-owner`, `hb-owner-back`, sun `lightManager.sunDirection 0.7/-0.07/0.71`.
- Clouds' ambient along the view (contract 34): shaded sides take the sky behind them. The dusk
  flatness was Kashan's haze (render.md, 12, near clouds); compare with `renderer.weatherVisibility
  60000` and `renderer.weatherMixing 1500` (the level's: 20000, 3500).
- The sky under the deck (contract 35): gaps between clouds now take the clouds' shadow on the
  haze below the deck, so they no longer glow clear past veiled clouds. Same views.

## Open issues

- Sun moved off the level's own (`lightManager.sunDirection`): past the cascades the map keeps the
  baked noon sun term, so a dark disc round the camera and a bright map against the far ground.
  Proposed: the sun term owned at run time everywhere (render milestone 14,
  [render.md](lanes/render.md#14-gtao--lightmap-channel-ownership)); not yet planned by the owner.
- Far clouds at dusk shade flat under Kashan's dust haze (visibility 20 km, mixing 3500 m): the
  air's in-scatter, as modelled; whether that haze is right for dusk (a deep dust layer at
  sunset) is the owner's call ([render.md](lanes/render.md#12-clouds), near clouds' notes).

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.
