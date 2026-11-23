# Renderer: HDR post (bloom, auto-exposure)

Part of [the renderer as built](README.md).

## 13. HDR bloom + auto-exposure

Accepted by the owner, 2026-11-23, by eye; no GPU timing (owner). Task 6 dropped. Everything runs in
`HdrPost.cpp` (effect `Shaders/RaHdrPost`, contract 19), called by the post-production composite
(`postProductionFinish`) on the resolved single-sample float scene just before the final copy
(FSQuadDrawer pass 0, the grade), after tinnitus, flashbang and gas mask; skipped when TV draws
straight to the back buffer. Thermals (fog colour with no red) skip all of it: exposure 1, no bloom,
the 2.2 encode, as before. Under water it all applies. Every pass is a full-screen pixel pass into
a target of its own, unrolled by hand (no shader loops: they came out wrong under DXVK).

1. **Dither and the encode.** The grade adds triangular noise of one 8-bit step (two interleaved
   gradient noise values, offset each frame) after the encode. The encode is the sRGB curve (the
   curve the samplers decode colour textures with), darker than retail's 2.2 power below 0.0031
   linear; thermals keep the 2.2 power. Tonemap and encode are now separate (`RGrade_Apply`, `RGrade_Encode`).
2. **Scene luminance.** The bloom's down chain runs (to its fourth level, 1/16 of the scene) even
   with the bloom off; that level, bilinear, down to 64 x 32 log2 luminances with a centre weight
   (1 at the centre to 0.25 at the corners). The 64-bin histogram over −14 to +10 EV is a gather,
   not the planned point scatter (no vertex buffer, no float blending): 16 x 64 partial sums (each
   texel half a meter row, four bins in rgba), then 16 x 1 (`A32B32G32R32F`). Metered value: the
   mean EV of the bins between the 50th and 95th percentile of the weight.
3. **Auto-exposure.** A 1 x 1 `A32B32G32R32F` ping-pong: adapted EV, exposure (multiplier),
   metered EV, 1 once written. The adapted EV moves toward the metered one by 1 − e^(−dt·rate),
   3/s into light and 1/s into dark, times `renderer.exposureSpeed`; it snaps on the first frame
   after a device reset and after a frame gap over 0.5 s (a level load). Exposure in stops =
   log2(key / adapted) + `renderer.exposureBias`, clamped to `renderer.exposureMin`/`Max`
   (−4, +4); under the key only `renderer.exposureStrength` of the gap is lifted since milestone 21
   ([21](../milestones/render-21-grade.md), task 6). The key is 0.42, calibrated, not 0.18: Muttrah City's reference views meter −1.0 to
   −1.3 EV, and 0.42 (−1.25 EV) shows them at milestone 11's accepted look (0 stops). The ceiling
   is what keeps night dark (task 5). `renderer.exposureBias` adds stops over it: −0.3 since
   milestone 21 (owner, 2026-11-23: Muttrah City's day read washed out at 0).
   Particle emissives read the state per vertex (`RaExposureParticle`, `RaExposureState`) and
   divide by the exposure, so an emissive tint reaches the tonemap at its authored value, as
   retail showed it, by day and by night; one frame late (the state is written at the frame's end).
4. **Bloom.** Six half-size `A16B16G16R16F` levels from the scene (level 0 half its size); down
   with Jimenez's 13-tap filter, level 0 Karis-averaged (each of its five boxes weighted by
   1 / (1 + luminance)) and every scene sample's luminance capped at 100; up with a 3x3 tent of
   one smaller-level texel, added (blend ONE, ONE) into the level above; the grade takes level 0's
   tent at full size, divided by the level count, and mixes it in before the exposure:
   lerp(scene, bloom, `renderer.bloomStrength` 0.04). Outside thermals retail's glow is off: `postProductionSetGlowEnabled` keeps the owner's glow byte 0,
   so neither the scene glow (`GlowMaterial`) nor the `RSMTGlow` queue nor their composite runs,
   as with retail's glow off; the night-vision distortion skips its glow part. Thermals, which
   skip the HDR post, keep retail's glow until their rewrite. Facing the sun the
   disc gets a small halo, nothing tuned per source.
5. **Night.** The Falklands' night layer (`gpm_cq 32`, view `w0`) meters −9 EV and sits on the
   ceiling: at +4 stops its terrain draws at about 25/255 (2.2 encode at 0 stops: 8/255; retail
   about 30/255, reached at +4.5, where the scene starts to read as an overcast dusk). The toe is
   not changed. Accepted at +4.
6. **Compressed (Karis) resolve: dropped** (owner, 2026-11-22). The owner sees no aliasing at
   bright edges with auto-exposure and bloom on. It would have meant every scene pixel shader
   writing compressed colour through an engine-owned register flag, an expansion pass at the
   resolve, and every pass blending before the resolve (decals, particles, water, first-person
   transparents, additive light passes) blending compressed values.
7. **Judged by eye** and accepted (owner, 2026-11-23); no GPU timing.

`tools.exposure` ([tools.md](../lanes/tools.md#exposure)) dumps the state, histogram, meter and bloom.
On acceptance the A/B switches `renderer.exposure`, `renderer.encodeSrgb` and `renderer.bloom`
were deleted (bloom always on, owner 2026-11-23); `bloomStrength` and the `exposure*` limits stay.
