# MoonandShadow-mobile

See [mobile compatibility and reduced-intensity preset](MOBILE.md).

# MoonandShadow

A single CubeModLoader API 7.3 mod combining moon lighting, a visible moon disk, dimmer night clouds, and real-time sun/moon/cloud shadows. Made by DieuDeGlace with the aid of AI (OpenAI ChatGPT/Codex). MIT licensed; see THIRD_PARTY_NOTICES.md for SDK attribution. Source migrated from SteamCubeWorldMods.

## Build

Requires Windows x64, CMake 3.20+, Ninja, Visual Studio 2022 C++ Build Tools, Windows SDK and clang-cl. In an x64 Native Tools command prompt with those tools on PATH:

```bat
git clone https://github.com/DieuDeGlace/MoonandShadow.git
cd MoonandShadow
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_GPU_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Install

Close Steam Cube World, remove MoonLight.dll and SteamShadows.dll from Mods, and install build/MoonandShadow.dll instead. Do not load the old standalone DLLs alongside it. Existing Mods/MoonLight.ini and Mods/SteamShadows.ini settings are retained; examples are in config. F7 toggles shadows. WorldMotion is optional for animated caster coordination. ReShade is untouched.

Supports the inspected Steam executable (PE timestamp 0x5d93ab0d, image size 0x5b7000); Alpha and Omega executables are unsupported. Moon and sun share one shadow map. GPU tests check rendering resources, filtering, cloud casting and moon rendering; they do not guarantee FPS or visual correctness in every location.

## Shadow darkness

Shadows match the unmodified game's own dark side. Every shadowed surface (terrain and models) loses only the direct-sunlight part of the game's lighting, using the same formula as the native world vertex shader. With `StrengthPercent=100` (the default), a sunlit surface in shadow is exactly as dark as a block face turned away from the sun; ambient, back and lantern/point light are kept, so the darkness follows the game's own day/night lighting. Lower values remove only that share of the sunlight.

Shadows keep full strength while the sun or moon is more than about 17.5° above the horizon and fade out below that. Within about 4.6° of the horizon (roughly 17:42–18:18 and 05:42–06:18 game time) there are no shadows at all, so the switch between sun and moon happens while nothing is drawn, and shadows fade back in as the new light rises. MoonLight also turns the native light direction gradually towards the moon as it fades in and out, instead of jumping at 18:00 and 06:00.

## Appearance and fade

Palette/desaturation now lives in [alpha-lookalike](https://github.com/DieuDeGlace/alpha-lookalike), a separate loader DLL. MoonandShadow applies no fullscreen color grade. Install alpha-lookalike only if wanted. ReShade remains untouched. The optional FinishWorldLighting export allows the palette to finish moon/shadow rendering before grading regardless of hook installation order.

SunWarmthPercent=3 retains the small warm sunlit contribution in the existing shadow receiver. FadeStartPercent=40 starts a smooth shadow fade at 40% of Radius and reaches zero at Radius. With CameraFade=1 (default) that distance is measured from the camera, as in Omega: zooming out shortens how far shadows reach and, fully zoomed out, removes them around the player. CameraFade=0 restores the older ground-distance fade, which ignores zoom. The supplied preset (Resolution=3072, Radius=70) fades shadows between 28 and 70 blocks from the camera; Radius also sets how much ground the shadow map covers. Pixels past the fade skip their shadow-map samples, so zoomed-out views cost less GPU time.

Lantern correction retains native illumination and suppresses added directional darkening near the player over the existing 12-block radius. No local-light shadow maps or occlusion tracing are added. Five moon/shadow tests pass; gameplay appearance and performance require live verification.

## 120-block shadow update

The code default (no INI) is a 4096×4096 map with Radius=120; the supplied preset uses 3072 with Radius=70, which is slightly sharper per block (about 22 texels per block versus 17). D32 shadow storage rises from 36 MiB (3072) to 64 MiB; the map has about 1.78 times as many texels. Higher resolution reduces shadow-map stair stepping while retaining the inexpensive hardware comparison filter. It does not eliminate voxel geometry steps or all screen aliasing. Larger caster coverage can cost more GPU time; FPS needs live verification.

The receiver no longer rejects faces pointing sideways/away from the directional light or fades their shadows based on face angle. All six voxel normals now receive occluder shadows; angle remains relevant to sunlight warmth and local lights. Grazing-face depth bias is bounded more tightly to avoid withdrawing shadows from block sides. Existing lantern protection and distance fade are retained.

Side shading correction: faces turned away from the directional light now have a consistent zero direct-light visibility, smoothly transitioning over a narrow grazing angle. Their own shadow-map samples no longer alternate between illuminated and shaded edge pixels. Sun-facing faces still receive ordinary cast shadows. Native ambient color, point-light/lantern protection and the distance fade remain; this is a visual approximation of the reference, not Omega renderer parity. No additional sampling, texture or rendering pass is added.

Shadow attachment correction: depth bias is now 0.008 world blocks divided by the actual light depth span, rather than a fixed normalized-depth bias. The former 0.0002 bias represented about 0.41 blocks with the 2048-block cloud-enabled depth span, causing detached contact shadows. Grazing-angle bias is also reduced and capped. Resolution/range are unchanged. Smaller bias can expose self-shadow acne; targeted tests pass but live tree/voxel-face verification is required.

Receiver-plane anti-acne correction: shadow UV/depth derivatives estimate the comparison sampler footprint, replacing the undersized angle bias that exposed diagonal self-shadow stripes. The small world-unit minimum remains. PCF uses a wider footprint allowance. An actual GPU test now checks that an unoccluded plane under an angled light has no dark stripe pixels. No extra render pass, texture or shadow samples are added; in-game contact and performance still need verification.

## Cascaded shadow test branch

Use `test/cascaded-shadows` for the two-map experiment and pair it with WorldMotion from the same branch in SteamCubeWorldMods. `Cascades=1`, `NearResolution=2048`, and `NearRadius=32` are experimental defaults. Set `Cascades=0` before restarting for the single-map comparison. Main is unchanged. Keep stable TemporalAA installed; no SMAA or depth-reprojection build is required.

### Model contact-shadow test

The model pixel adapter uses a receiver-plane depth comparison at each of the four texels in its bilinear shadow footprint. The previous comparison used one depth reference for all four texels; on slanted model faces, some of those texels belonged to the same face but failed the comparison, creating repeating stripes. The new filter retains the small world-space bias so nearby hair and armor can still cast contact shadows. Near-map coverage is checked before selecting it; models outside its projected coverage use the far map.

The crisp preset replaces one hardware comparison lookup with one raw depth gather and four arithmetic comparisons, in the existing receiver pass. Map resolution, terrain filtering, caster count, shadow darkness, sunlight warmth, and local-light protection are retained. This adds arithmetic for model pixels, not another rendering pass or shadow texture; live GPU timing and visual verification are still required. The native-model pixel shader uses Shader Model 4.1 (Direct3D feature level 10.1 or higher). [Microsoft documents the gather ordering](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/gather4--sm4-1---asm-).

`ModelPlaneTest` sweeps both depth-slope directions, four subtexel phases, and the actual near/far map densities. It checks uniform visibility on a slanted face and continuous occlusion from a plane only 0.08 blocks away. The previous shader fails all 16 unoccluded cases; the revised filter passes all 32 cases. These controlled GPU checks establish the sampling correction, not final appearance in every live scene. `ModelLightingTest` checks ambient/local light preservation, warm sunlight, and close model occlusion. `NativeClipTest` compares the actual locally installed Steam vertex shader with the model receiver; shader bytecode is not distributed.


### World-space model lighting

Models use direct world-space position and inverse-transpose normals, following the shared caster/receiver transform structure inspected in Omega and the direct position path in Alpha. The prior camera-space adapter and its auxiliary buffers are removed. Shadows attenuate the native sunlight share while preserving ambient/back/local light, white flashes, and glow. Additive warm sunlight is retained with dual-source blending into the existing scene target. No additional map, rendering pass, or receiver geometry is introduced. Menu previews are excluded from the world shadow pass.

This aims for continuous hair/face contact shadows with a visible ambient-lit face. It approximates the lighting components inside Steam's existing pipeline; live appearance and performance must still be checked against the references. See PERFORMANCE.md for the current implementation and validation details.

Noon adjustment: vertical model faces receive consistent directional shade at grazing light angles, with a smooth transition back to ordinary shadow sampling as they turn toward the light. This retains native ambient/local illumination and normal shading on upward-facing surfaces. Fully shaded faces skip the shadow gather; terrain filtering and map sizes are unchanged. The 160-case GPU plane sweep checks phase stability across the transition; live reference comparison remains necessary.
