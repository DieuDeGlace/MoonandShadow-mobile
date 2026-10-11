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

## Appearance and fade

Palette/desaturation now lives in [alpha-lookalike](https://github.com/DieuDeGlace/alpha-lookalike), a separate loader DLL. MoonandShadow applies no fullscreen color grade. Install alpha-lookalike only if wanted. ReShade remains untouched. The optional FinishWorldLighting export allows the palette to finish moon/shadow rendering before grading regardless of hook installation order.

SunWarmthPercent=3 retains the small warm sunlit contribution in the existing shadow receiver. FadeStartPercent=40 starts a smooth directional-shadow fade at 40% of Radius and reaches zero at Radius (48�120 blocks with Radius=120). This is a reference-inspired distance fade, not a change to shadow-map blur. The shadow map is now 4096 in the supplied preset; this fade adds no map or pass.

Lantern correction retains native illumination and suppresses added directional darkening near the player over the existing 12-block radius. No local-light shadow maps or occlusion tracing are added. Five moon/shadow tests pass; gameplay appearance and performance require live verification.

## 120-block shadow update

The default map is 4096�4096 with Radius=120. D32 shadow storage rises from 36 MiB (3072) to 64 MiB; the map has about 1.78 times as many texels. Higher resolution reduces shadow-map stair stepping while retaining the inexpensive hardware comparison filter. It does not eliminate voxel geometry steps or all screen aliasing. Larger caster coverage can cost more GPU time; FPS needs live verification.

The receiver no longer rejects faces pointing sideways/away from the directional light or fades their shadows based on face angle. All six voxel normals now receive occluder shadows; angle remains relevant to sunlight warmth and local lights. Grazing-face depth bias is bounded more tightly to avoid withdrawing shadows from block sides. Existing lantern protection and distance fade are retained.

Side shading correction: faces turned away from the directional light now have a consistent zero direct-light visibility, smoothly transitioning over a narrow grazing angle. Their own shadow-map samples no longer alternate between illuminated and shaded edge pixels. Sun-facing faces still receive ordinary cast shadows. Native ambient color, point-light/lantern protection and the distance fade remain; this is a visual approximation of the reference, not Omega renderer parity. No additional sampling, texture or rendering pass is added.

Shadow attachment correction: depth bias is now 0.008 world blocks divided by the actual light depth span, rather than a fixed normalized-depth bias. The former 0.0002 bias represented about 0.41 blocks with the 2048-block cloud-enabled depth span, causing detached contact shadows. Grazing-angle bias is also reduced and capped. Resolution/range are unchanged. Smaller bias can expose self-shadow acne; targeted tests pass but live tree/voxel-face verification is required.
