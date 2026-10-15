# GPU performance measurements

Made with AI assistance. These measurements retain the existing visual settings.

The mod samples its GPU pass once every 60 executions using a four-slot timestamp-query ring. Results are polled with `D3D11_ASYNC_GETDATA_DONOTFLUSH`; unfinished samples are skipped without waiting. Disjoint timestamps are rejected. Logs report averages of ten completed samples in milliseconds.

MoonandShadow writes `Mods/SteamShadows.log`: first is shadow-map generation; second is receiver application. TemporalAA writes `Mods/TemporalAA.log`: first is input copy and setup; second is resolve and output-to-history copy.

These measure GPU execution of the named passes, not total frame time, CPU capture overhead, or FPS. Sparse timestamps still have a small instrumentation cost. Compare the same location, camera, resolution, and settings after loading has settled. Test both enabled, shadows only (F9 off), TAA only (F7 off), and both disabled. Preserve ReShade settings throughout.

## Current receiver implementation

The test branch retains the 4096 map at radius 120 and a 2048 near map at radius 32. Both are texel-snapped independently. The outer fifth of the near region blends into the far map. Geometry is replayed once for receiving; only that overlap samples both maps. Loaded terrain is conservatively culled in light space for the near caster pass. The near map adds about 16 MiB of depth storage. Terrain filtering, configured darkness, sunlight warmth, and lantern protection remain in place.

Models now carry world position directly from their world transform, shared with the caster, and use inverse-transpose normals for mirrored/nonuniform transforms. The receiver preserves Steam's native clip-matrix arithmetic and depth offset. Camera inverse reconstruction, original native-shader replay, its six auxiliary constant buffers, and its repeated resource/shader bindings have been removed. Model/terrain shaders and blend states change only at boundaries. Replay vertex/index/layout/constant caching is retained. Only active local lights are evaluated, in their original order.

Model shadows attenuate the native directional-light share instead of multiplying ambient/back/point lighting by the full terrain shadow strength. Native material alpha, ambient/back colors, local-light colors, white flashes, and glow protection are included. This is a lighting-component approximation within the existing Steam receiver, not a replacement of the complete native color/fog pipeline. Dual-source blending supplies sunlight warmth and the RGB attenuation factor to the same existing render target; no texture or pass is added. See [Microsoft's output-merger documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage).

Grazing model faces now receive consistent directional shade when their normal has a light-facing cosine at or below 0.15. Sampling smoothly returns between 0.15 and 0.30; upward-facing surfaces retain normal occluder/contact shadows. This stabilizes the thin light-space projections of vertical faces around noon. It is a deliberate angular lighting rule, not a claim that all depth-map limitations are solved. Ambient/back/local illumination is preserved, and fully shaded model faces skip the gather. Terrain is unaffected.

The crisp model filter performs one raw depth gather per selected map, with receiver-plane depth comparisons at the four texels. Terrain retains hardware comparison filtering. The small model bias preserves close hair/armor occluders. Tall receivers outside the projected near map use the far map. Rotating menu previews lack an established native terrain origin and are excluded from the world shadow pass.

The matching WorldMotion test branch shares one animation timestamp between native leaves/clouds and shadow sampling. Stable TemporalAA remains installed. This is an independent implementation informed by Alpha/Omega behavior; proprietary shader code is not bundled or published. Actual model appearance, foliage motion, and frame cost still require live verification.

## Validation

Nine tests cover moon rules/rendering, resource setup, terrain filtering, cloud casting, cascade filtering, model lighting/contact shadows, inclined model depth planes, and native model clip positions. ModelPlaneTest sweeps real near/far map density and subtexel phase, retaining 0.08-block contact occlusion. Model lighting checks retain ambient-only illumination, local lights, and additive warm sunlight.

The model-plane sweep includes five face orientations above, inside, and below the grazing transition: 160 near/far, orientation, occluder-gap, slope-sign, and subtexel-phase cases. Unoccluded surfaces follow a uniform continuous angular exposure; occluders retain directional occlusion. This specifically tests that changing subtexel phase does not recreate bands around the noon shading rule.

NativeClipTest reads the supported Steam executable's shader locally at runtime, without including it in the repository. It compares 4,608 clip components across 24 mirrored/nonuniform poses and native depth offsets, and checks transformed normals. The verified fixture matches clip positions bit for bit. Users without that exact executable receive a skipped test (exit 77); an executable path can be supplied as its first argument. These tests validate specific rendering invariants, not Omega visual parity or measured FPS.

## Overhead reductions

These changes do not alter the image. The 3072/2048 maps, filtering, camera/dusk fades, mesh-replay placement, TAA and WorldMotion are unchanged.

- Replay constants use a dynamic buffer with `WRITE_DISCARD` instead of `UpdateSubresource`. The receiver and caster passes upload once per draw. Updating a default-usage buffer the GPU is still reading makes the driver copy it and order each draw.
- Receivers whose world bounds lie wholly beyond the shadow radius are not replayed. The test uses ground distance, plus height when CameraFade is on. The pixel shader already returns "unchanged" for fully faded pixels.
- Model casters are culled against each map's light-space bounds. Terrain now uses this test for the far map too, not only the near map.
- In the combined mod, one Map/Unmap/Update hook layer (MoonLight's) mirrors native constants into one shared cache. Previously two layers each stored every write. Buffers are classified with `GetType` instead of `QueryInterface`.
- Replayed draws go straight past MoonLight's per-draw hook, which never applies to this pass's own constants. MoonLight computes its moon state once per game-clock value instead of per draw. The supported-executable check is cached.
- `SteamShadows.log` reports captured/receiver/model-caster counts. Every minute it also logs the capture hook's CPU time per frame, sampled every 60th frame.

## Inline terrain shadows, 2026-10-15

The receiver pass replayed every visible draw a second time to darken shadowed areas. Terrain chunks are its largest part: they have the most vertices and cover most of the screen.

Terrain now gets its shadow inside the native draw:

- **Shader copies.** `InlineTerrain.h` re-expresses Steam's world vertex shader (`0x436fb0`) and the two world pixel shaders used for terrain (`0x438810` shine, `0x43b500` plain) instruction by instruction.
- **Shadow term in place.** The pixel shader applies the receiver's shadow term directly: native × transmission + sun warmth. That is what the receiver's blend did to the stored native colour.
- **Draw-time swap.** The game's shaders are recognised by their DXBC checksum when they are created. While the previous frame's shadow maps are valid, the copies are swapped in for native terrain draws, and the receiver pass skips those draws.
- **No per-draw uploads.** Native constants stay in their own slots (b0–b9). The shadow parameters use b12, and the floating-origin shift uses b13.
- **Fallbacks.** Models keep the receiver pass, so their self-shadows stay current-frame. Terrain drawn with any other shader also stays on the receiver pass, as does everything when `InlineTerrain=0` is set in `SteamShadows.ini`.
- **Trade-off.** Terrain samples the shadow maps one frame later, so a moving caster's ground shadow lags by one frame.

Validation, on the RX 6600 against the installed executable's own shaders:

- `NativeEquivalenceTest`:
  - Vertex shader: 98,304 random vertices, native UBYTE4 and WorldMotion float layouts, random native constants. Positions and depth are exact; the largest colour difference is 7 ULP.
  - Both pixel shaders: 0 ULP over 12 × 65,536 pixels each.
- `InlineTerrainTest`: native draw plus the real receiver pass versus the real inline draw function, with a shadow map that shades about 19,000 pixels, point lights, and a shifted floating origin. The maximum difference is 1/255, which is 8-bit rounding. Overlapping coplanar faces were left out; the receiver pass blends such faces twice, while the inline draw shades them once.

The live frame cost still needs `SteamShadows.log`: "inline terrain=N" in the frame line, and "Receiver work" for the remaining receivers.
