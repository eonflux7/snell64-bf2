# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11, 15 and 16 accepted; 12 (clouds) paused | Milestone 13 (bloom, exposure): tasks 1–5 built, awaiting the owner; milestone 17 (far terrain): tasks 1–4 built (generator dropped, owner 2026-11-23); 5: Kunar, Kashan, Muttrah baked (heights), the bake's colour next; milestone 18 (atmosphere rework): tasks 1–3 built, 4 next | [render.md](lanes/render.md#13-hdr-bloom--auto-exposure), [17](lanes/render.md#17-far-terrain) |
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

## In progress: `render` milestone 18, atmosphere rework

One atmosphere with its aerosol from the weather (visibility, type, mixing height, the level's
altitude) in place of the ground-haze layer; weather per level. Design, as built and tasks in
[render.md](lanes/render.md#18-atmosphere-rework).

1. The model (aerosol, altitude from the bake, the sun against the reference air, tables on a
   worker, the camera's sky view by height band): built.
2. Aerial perspective from the model to 40 km (contract 27), the ground haze deleted: built.
3. Clouds through the same air (contract 28: near clouds by the atlas, the panorama's air from the
   model): built.
4. Weather per level: next. 5–6 as planned.

Open for the owner: above the mixing layer the aerosol falls at 1.2 km (the design's), so 1 km
over the layer's top it keeps 43 %: from a jet (Kunar Province `jet`) the ground 20 km off is
under an optical depth of about 2, washed to the horizon's blue, and at 5 km visibility the
surfaces' sun drops to 0.15 / 0.09 / 0.04 (Muttrah City, sun at 43°). A sharper top (a real
inversion) would thin both. It also hides the clouds aloft at 8 km (Kunar Province `horizon`, shot
`m18t3-horizon-8k`): even at 30° they lie under an optical depth of about 1.5.

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
  `renderer.farGroundScaled 1`, and `renderer.farGround 0` for the retail ring). Colour past the
  map is still the edge colour. Where it meets the map and the clouds behind the haze: Kunar
  Province `edge-low`, `edge-e`, `horizon`; it is in the depth prepass (contract 24), so the near
  clouds no longer draw over it. Elsewhere `renderer.farGround 2` forces it with placeholder heights
  past known ground. Look at with 2: Fallujah West `ft-2000` (no sea
  past the river's mouth), Muttrah City `ft-top` (the sea only by the bay), Kashan Desert
  `ft-1000-y0` (the map's colour at its edge with its detail layers' mean, the detail texture
  close to the camera). Procedural ground was built and dropped on the owner's look.

- Render milestone 13, tasks 1–5: on any map, A/B with `renderer.exposure 0`, `renderer.bloom 0`,
  `renderer.encodeSrgb 0` (all three: the old look). Muttrah City `street`, `sunup` (the sun's
  halo); The Falklands `gpm_cq 32` view `w0` (night: `renderer.exposureMax` 4 now, 4.5 matches
  retail's brightness). Bloom strength `renderer.bloomStrength` (0.04).

- One atmosphere (render milestone 18, tasks 1–2): the haze is the air's aerosol, from
  `renderer.weatherVisibility` (25 km default), `weatherAerosol` (0 continental, 1 maritime, 2 dust,
  3 smoke), `weatherMixing` (1500 m), `weatherAltitude` (from the bake). Kunar Province `horizon`
  at 100, 25, 8, 5 km (shots `m18-horizon-<n>k`), `ft-1500`, `jet` (above the haze layer);
  Muttrah City `sun`, `antisun` (bright warm haze toward the sun, bluer away). Task 3: the clouds
  fade in the same air (`m18t3-horizon-25k`, `-8k`; visibility is in metres). No A/B: the old
  haze is gone. A rebuild takes about 0.3 s after a change.

## Open issues

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.
