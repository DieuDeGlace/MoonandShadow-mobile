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

### Static models (same day)

Models that are not moving now get the same in-draw shadow. A model counts as not moving when its mesh, draw range and absolute transform match the previous frame: buildings, props, and NPC parts standing still. Static models use the model shadow filter (`MODEL_PLANE_FILTER`, ps_4_1) and the receiver's model parameters. Moving models keep the receiver pass, so their self-shadows stay current-frame. `InlineModels=0` keeps every model on the receiver pass.

`InlineTerrainTest` now also checks a rotated and scaled static model, with both pixel-shader variants and a shifted origin. The maximum difference is 1/255. The shadow frame log reports "inline static models=N".

## Instanced model caster submissions

The caster phase groups repeated model geometry separately for each map after the existing conservative bounds test. The key includes vertex/index buffers, input layout, strides, offsets, index format, topology, draw range, base vertex and indexed/nonindexed mode. Each instance retains its current frame's full world transform, including mirrored, rotated and nonuniform poses. Only the depth-only caster shader changes its source of world matrices. No material, light, receiver or animation data is reused from an earlier frame by this change.

A fixed 256 KiB dynamic typed-buffer SRV holds at most 4,096 transforms. One discard upload is made per map when repeated groups exist; each repeated group gets one compact submission and one Params upload. The shader reads its explicit table base from an otherwise unused caster field, with zero StartInstanceLocation. Existing geometry bindings are reused. The ordinary path handles singletons, disabled batching, oversized captures and failed resource/upload checks. State changes stay within the existing isolated shadow context. Lilies retain their current pad-caster exclusion; flower casters remain eligible. Loaded terrain, WorldMotion mesh selection, and cloud casting are unchanged.

The typed-buffer load is supported in Shader Model 4 vertex shaders, as documented by [Microsoft](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-load). This adds four transform-buffer loads per caster vertex while removing repeated CPU submissions and per-object constant uploads. It can help scenes with repeated small meshes; a vertex-bound scene or one with mostly unique terrain may gain little. Map-before-receiver ordering was already present in both the inspected Alpha and Steam implementations. No native-world pass is deferred or rerun by this change.

`CasterBatchTest` runs on the hardware device and WARP. It compares every D32 map bit over three changing frames and both map scales, using native byte and float meshes, indexed and nonindexed paths, nonzero draw/vertex/index offsets, negative base vertices, mirrored/nonuniform/sheared transforms, overlapping surfaces, fresh vertex data, singletons and the existing pad/terrain exclusions. Its 255 eligible objects become 5 submissions with identical depth. A deliberately failed transform upload produces the original 255 submissions and the same map. An oversized capture also falls back. Existing model planes, lighting, native clip/equivalence, inline receiver, clouds, cascades and moon checks remain passing (13 total).

`SteamShadows.log` reports `Model caster submissions (both maps): N objects -> M draws` at startup and every 600 shadow frames. Those counts are scene-dependent. Existing sparse GPU timing still separates map generation from receiver replay; inline shading costs remain inside native scene draws and are not captured by that receiver timer. Live total-frame CPU/GPU timing and appearance need a same-location comparison after installation. No live FPS or millisecond improvement is claimed by these fixtures. Restart with `BatchCasters=0` to compare while preserving every visual setting.

An optional isolated benchmark is available as `build/CasterBatchTest.exe hardware benchmark`. It repeats the 255-small-mesh fixture over 96 maps per mode at 512 pixels, alternating ordinary and batched submissions twice. On the local RX 6600, the measured GPU interval was approximately 0.084 ms/map ordinary and 0.018–0.019 ms/map batched. CPU submission was 0.179–0.199 ms ordinary and 0.026–0.116 ms batched (including the first batch run's driver warmup). This tiny synthetic scene does not model the full game's terrain, map sizes or CPU load and must not be extrapolated to a live-frame saving. The benchmark waits for its queries after each run; production timing retains its nonblocking polling.

## Cached terrain culling and inline state overhead

Cached terrain caster draws select their animated mesh directly and do not carry the visible-draw terrain replay tag. They were consequently culled using the model box (`-255..255` on each local axis) rather than the existing terrain box. The caster cull now explicitly selects the conservative terrain box: `-1..65` horizontally and `0..255` vertically, transformed and projected onto each map. This keeps the complete chunk height, including tall off-camera casters, and more than the supported WorldMotion maximum XY sway of 0.35 blocks. Mesh selection, caster shaders, receivers and light matrices are unchanged. `TightTerrainBounds=0` restores the old caster bounds; diagnostics report cached terrain candidate and submitted counts across both maps every 600 shadow frames.

`TerrainBoundsTest` and its WARP counterpart compare every D32 depth bit for native and wind-expanded geometry, both map scales and four light directions, including grazing illumination. All 16 cases per device match exactly. The overhead-light fixture reduces 81 cached terrain submissions to 16 far-map draws and 4 near-map draws; angled cases reduce the same 81 candidates to 41–46 far draws and 17–22 near draws. These fixture counts are not predictions for every live view.

Inline shader identities are now cached with two retained interfaces, preventing recycled COM addresses from being misclassified. The draw path also saves/binds/restores only VS b13 and PS b12: reflection of all five compiled inline variants proves the unused companion slots are absent. Their existing contents remain untouched. The expanded inline test covers all four sentinel bindings, failed inline uploads, both pixel shader variants, origin shifts, zero/two/ten lights and terrain/rotated/mirrored/nonuniform/sheared model poses. It runs on hardware and WARP, with final native-plus-receiver versus inline pixels within the existing 2/255 limit (1/255 observed locally). Sixteen total CTest checks pass.

The HLSL is unchanged from the preceding batching build. Lighting-loop experiments were rejected after isolated GPU measurements regressed; no register-pressure or lighting-formula change is shipped. A private before/after stream-output audit compares 1,572,864 vertices and all 31 inline outputs exactly (0 ULP, including clip/depth). A narrow shader-tag microbenchmark measured about 18.95 ns for a direct tag read versus 0.52 ns for a retained-cache hit. This verifies a small lookup saving, not the total hook/frame cost. Overall in-game frame-time improvement still requires a fixed-view comparison; existing shadow/TAA pass timers omit inline GPU work and cannot account for the entire F7 frame-time difference.
