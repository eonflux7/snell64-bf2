# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11, 15, 16, 18 and 19 accepted; 12 (clouds) paused | Milestone 13 (bloom, exposure): tasks 1–5 built, awaiting the owner; milestone 17 (far terrain): tasks 1–4 built (generator dropped, owner 2026-11-23); 5: Kunar, Kashan, Muttrah baked (heights, land classes) | [render.md](lanes/render.md#13-hdr-bloom--auto-exposure), [17](lanes/render.md#17-far-terrain) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q, R fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Current: `render` milestone 13, HDR bloom and auto-exposure

Design and as built in [render.md](lanes/render.md#13-hdr-bloom--auto-exposure).

1. Dither after the tonemap, sRGB encode (`renderer.encodeSrgb`): built.
2. Scene luminance, a 64-bin histogram by gather, percentile metering: built.
3. Auto-exposure (`renderer.exposure*`, `tools.exposure`): built; key calibrated on Muttrah City.
4. Bloom (`renderer.bloom`, `bloomStrength`), replacing retail's glow while on: built.
5. Night: The Falklands' night layer on the +4-stop ceiling (owner to judge); night particles not
   yet looked at.
6. Compressed (Karis) resolve: dropped (owner, 2026-11-22: no aliasing seen at bright edges).
7. Judged by eye; GPU cost with the final GPU pass.

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

- Far ground (render milestone 17, tasks 2–5, [render.md](lanes/render.md#17-far-terrain)): drawn
  on levels with a bake: Muttrah City, Kashan Desert, Kunar Province (real heights; A/B
  `renderer.farGroundScaled 1`, and `renderer.farGround 0` for the retail ring). Where it meets the map and the clouds behind the haze: Kunar
  Province `edge-low`, `edge-e`, `horizon`; it is in the depth prepass (contract 24), so the near
  clouds no longer draw over it. Colour past the map from the bake's land classes (WorldCover) in the
  map's colours: Kunar Province `ft-1500`, `jet`, `top-12k`; Kashan Desert `ft-2000-y0`. Elsewhere `renderer.farGround 2` forces it with placeholder heights
  past known ground. Look at with 2: Fallujah West `ft-2000` (no sea
  past the river's mouth), Muttrah City `ft-top` (the sea only by the bay), Kashan Desert
  `ft-1000-y0` (the map's colour at its edge with its detail layers' mean, the detail texture
  close to the camera). Procedural ground was built and dropped on the owner's look.

- Render milestone 13, tasks 1–5: on any map, A/B with `renderer.exposure 0`, `renderer.bloom 0`,
  `renderer.encodeSrgb 0` (all three: the old look). Muttrah City `street`, `sunup` (the sun's
  halo); The Falklands `gpm_cq 32` view `w0` (night: `renderer.exposureMax` 4 now, 4.5 matches
  retail's brightness). Bloom strength `renderer.bloomStrength` (0.04).

## Open issues

- Sun moved off the level's own (`lightManager.sunDirection`): past the cascades the map keeps the
  baked noon sun term, so a dark disc round the camera and a bright map against the far ground.
  Proposed: the sun term owned at run time everywhere (render milestone 14,
  [render.md](lanes/render.md#14-gtao--lightmap-channel-ownership)); not yet planned by the owner.
- Clouds: a circular step at the near field's handoff (16 km), seen from altitude
  ([render.md](lanes/render.md#12-clouds), near clouds' open notes).

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.
