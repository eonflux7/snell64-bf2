---
id: render-21
title: Tonemap and grade
state: in progress
updated: 2026-11-23
next: 5 (checks; a fire)
---

# Render milestone 21: tonemap and grade

Design, tasks and checks of an open milestone; its order is in [PLAN.md](../PLAN.md#render),
the renderer it builds on in [render/](../render/README.md). On acceptance it is folded into
the subsystem's document there and this file is deleted. Format: [WRITING.md](../WRITING.md).

The frame grade (FSQuadDrawer pass 0, `RGrade_Apply`) after milestone 13 was PBR Neutral with no toe
and fixed constants: linear up to 0.76, then a hue-keeping shoulder. Mid tones read flat, and bright
saturated light (fire, flashes) kept its hue instead of running to yellow and white (milestone 7's
red-orange fire). The grade replaces it with a parametric curve and moves the look to console
variables (`Grade.cpp`, bound as `RaGradeLook` next to milestone 13's `RaGrade`). Cost: a few ALU
ops in the one full-screen pass, about what Neutral cost (contract 37). Drops: PBR Neutral, and the shader's
unused ACES, AgX, Reinhard and lift/gamma/gain paths (deleted with task 1).

## Tasks

### render-21.1 Curve, white balance, console variables

State: built.

The GT curve's toe and straight section
(Uchimura 2017): a power toe below 0.22 (`renderer.gradeToe`, 1.5), slope 1 to
0.22 + 0.78 × `renderer.gradeShoulder` (0.6: 0.69), then a rational shoulder, PBR Neutral's
form (slope 1 where it starts, white at 10× display white). GT's own exponential shoulder was
white by about 3× and lost the brightest gradations; the rational one keeps Neutral's headroom
(display 0.92 at 1.5×, 0.97 at 3×, 0.99 at 5×). Taken two ways and blended by
`renderer.gradeHighlightShift` (0.3; at 0.5 a bright sky drifted toward cyan): hue kept (the
brightest channel through the curve, the others scaled with it, desaturated toward white by
the shoulder's pull as Neutral does) and per channel (orange runs to yellow, then white).
White balance: a Bradford adaptation from the white of `renderer.gradeTemperature` (K, 6504
the identity; the Planckian locus offset onto D65) moved off the locus by `renderer.gradeTint`
(1 = 0.02 Duv), to D65; a 3x3 matrix from the CPU, the white's luminance kept.
`renderer.gradeContrast` (1.2) around 0.18 in log2, and `gradeSaturation` (1.15), before the
curve. `renderer.gradeCurve 0` is milestone 13's output exactly (Neutral, contrast 1.05), the
A/B until acceptance; thermals always take it. `tools.exposure` prints the settings (`[grade]`).
Contrast and toe raised by the owner's call (2026-11-23, "we need more overall contrast"): at
Muttrah City `sun` the ground's 5th / 50th / 95th percentile went from 63 / 110 / 189 (contrast
1, toe 1.25) to 45 / 107 / 212 (1.25, 1.5); toe 1.8 reached 38 but lost the rock's shadow detail.
The shadows' brightness is mostly the lighting's (sky light into shade, the haze), not the
grade's.

### render-21.2 Shadows, mid tones, highlights

State: built.

After the contrast, in scene linear: weights by the
pixel's luminance in stops from 0.18, shadows full below −4 EV and gone by −0.5, highlights from
+0.5 to full at +4, the mids between. Each range has a tint (`renderer.gradeShadowTint`,
`gradeMidTint`, `gradeHighlightTint`, `r/g/b`; the engine divides out its luminance, so a tint
moves hue, not brightness) and the shadows and highlights a saturation over the global one
(`gradeShadowSaturation`, `gradeHighlightSaturation`). All 1 by default: no change.
`tools.exposure` prints them (`[grade ranges]`).
Seen by an agent (2026-11-23): Kashan Desert `hb-owner`, sun at 7°, grey haze (40 km, 2 km):
shadows `0.85/1.0/1.2` and highlights `1.1/1.0/0.85` cool the dune's shaded side and warm the
sun's glow; mid grey unmoved.

### render-21.3 A 3D LUT

State: deferred, with per-level looks (D-27).

The grade baked to a 32³ table when a variable changes, one fetch in the
pass; per-level looks.

### render-21.4 Low light

State: built.

Night is how far the adapted luminance sits under the full-lift ceiling
(the luminance at which a full lift would reach `renderer.exposureMax`: log2 key + bias − max,
from the engine, whatever `exposureStrength`; the adapted value from milestone 13's state), 0 at the ceiling, 1 two stops under it,
0 without auto-exposure. With night the toe and the contrast relax to 1, so the toe never deepens what the
ceiling already holds dark (milestone 13's accepted night stays), and dark pixels (exposed
luminance under 0.05, fading out by 0.5; lights keep their colour) move toward the rods'
response (roughly 0.06 / 0.52 / 0.42 of linear sRGB) in a grey-blue, by
`renderer.gradeNight` (0.35) at full night. `tools.exposure` prints it (`[grade night]`).
Seen by an agent (2026-11-23): the Falklands' night layer (`w0`) terrain 29/32/33 against
milestone 13's 27/29/29, faintly grey-blue, the grass less saturated; Muttrah City `sun` the same
with `gradeNight` 1 and 0 (within frame noise).

### render-21.6 Partial lift

State: built (D-28).
Milestone 13's exposure lifted any scene darker than the key back to it in full, up to +4 stops;
now only `renderer.exposureStrength` (0.6) of that gap is lifted (a room 4 stops under the key
gets 2.4), while a scene brighter than the key is still darkened in full, so daylight views at
or above the key are unchanged. The night still reaches the ceiling (The Falklands' night layer
is 7.5 stops under the key; 0.6 lifts 4.5, clamped to 4). The grade's night (task 4) keeps
its threshold where a full lift would reach the ceiling (log2 key + bias − max, −5.25 EV), not
the partial lift's (−7.9 EV at 0.6, which left The Falklands, −8.7, under half a night).
Seen by an agent (2026-11-23): Muttrah City `sun`, `antisun`, `street`, `valley`, `market` meter
above the key, exposure the same at 1 and 0.6; Kashan Desert `hb-owner` at the dusk test setting
likewise. No interior view saved yet. `RaHdrPost` grows to `float4[5]` ([4].x the strength).

### render-21.5 Checks

State: planned.

A/B on Muttrah City `sun`, `street`; Kashan Desert `hb-owner` at dusk; a fire; the
Falklands' night layer (`w0`, its toe against milestone 13's accepted 25/255).
