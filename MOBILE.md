# MoonandShadow mobile

Derived from https://github.com/DieuDeGlace/MoonandShadow with original Git history, attribution and license retained. Separate repository because GitHub does not support same-owner forks. Original PC repository unchanged.

Windows x64 mod for Steam Cube World through GameNative; not a native Android library. NativeScene keeps the executable timestamp guard 0x5d93ab0d and requires the tested phone SizeOfImage 0x58a000. Unknown builds are rejected. Rendering algorithm unchanged.

## Mobile75 preset

This preset reduces intensity and movement to approximately 75% of the PC settings while retaining the rendering features. Visual fidelity has no objective percentage; this is not a measured 75% performance/quality claim.

| Setting | PC | Mobile75 |
|---|---:|---:|
| Sun shadow strength | 60 | 45 |
| Moon shadow strength | 60 | 45 |
| Tree sway amplitude | 16 | 12 |
| Wind speed | 100 | 75 |
| Cloud drift distance | 64 | 48 |
| Cloud speed | 125 | 94 |

Preserved: shadow resolution4096, radius120, cloud shadows enabled, original soft-shadow setting0, moon visibility/brightness, night cloud brightness, tree animation range96, sun warmth and fade settings. Slower wind and weaker shadows do not inherently reduce rendering cost. Tree/cloud settings require WorldMotion-mobile from SteamCubeWorldMods-mobile, a separate mod.

## Install

Close the game. Back up Mods/MoonandShadow.dll, WorldMotion.dll and the three existing INIs. Put the mobile DLL in Mods/MoonandShadow.dll; keep one copy only. Copy config/mobile75/SteamShadows.ini and MoonLight.ini to Mods. For reduced tree/cloud wind, also install the mobile WorldMotion DLL and WorldMotion.ini. Restart the game. A release includes these components for convenience, with WorldMotion provenance linked above.

Game files, saves, Steam components, credentials and Microsoft runtime installers are not included. Tested runtime configuration and validation results are recorded in TEST-MOBILE.md.

## Build

Run build-mobile.cmd in a Windows x64 Visual Studio developer shell with clang-cl and Windows SDK libraries on PATH. CMake remains available as documented upstream. The script builds the DLL and runs the moon-rule CPU test; device rendering tests are separate.

## Rendering update and mobile cleanup (2026-10-11)
Upstream a7861b3 includes formatted source, cached replay bindings/uploads, inactive-light compaction preserving native order, and nonblocking GPU pass timing. MobileBuild.h names the narrow executable identity; NativeScene caches immutable validation but refreshes the game pointer on every query. Mobile75 settings retained. Tests cover pixel equivalence, moving foliage, light compaction, native shader arithmetic and lantern preservation. Paired with updated TemporalAA-mobile/alpha-lookalike-mobile in the actual Steam folder. No measured FPS-improvement claim.

## Coordinated rendering update (2026-10-12)
Merged upstream test/cascaded-shadows at 5084188, TemporalAA test/taa-edge-stability at 8f3c561 (code 44b12e0), and WorldMotion test/cascaded-shadows at 59c8a89. Palette has no newer source update and its installed companion stays unchanged.

Shadows retain the phone strength 45, full far 4096/radius 120, cloud and moon settings. Upstream now enables a near 2048/radius 32 map by default, even with older INIs; it adds roughly 16 MiB depth storage. Cascades=0 in [Shadows] disables the near map. The new model receiver preserves ambient and local illumination, fixes close contact/self-shadow comparisons, and stabilizes grazing model faces. WorldMotion shares animation time across native rendering and shadow capture. TAA aligns native backgrounds with world jitter and improves edge history rejection without adding samples or fullscreen passes. Phone wind settings and all INIs were preserved byte for byte during installation.

Built Windows x64 DLLs with clang-cl /O2. All nine shadow tests passed, including160 model-plane cases, native shader clip equivalence (4608 values, zero ULP discrepancy on the local PC fixture), ambient/point/lantern preservation, cascade filtering and state restoration. TemporalTest passed 720p/1080p/square cases; TemporalStabilityTest static-edge variance changed from 67.8996 to 14.7805 on WARP, with moving silhouette/uniform-color checks passing. This synthetic variance reduction is not an in-game percentage claim. Wind CPU/GPU tests passed including frame-time synchronization.

Installed into the actual GameNative Steam folder with a rollback installer; three DLLs byte-compared successfully. Pixel 10 Pro / Android 17 / GameNative 1.2.1 loaded into Groron Hills; HUD and terrain rendered. Logs confirm model receiver/cascades active and TAA resolves. User reported: "Looks steadier; speed feels similar." No matched gameplay FPS benchmark or long-session test was performed. Wine's shader compiler reports a potential-uninitialized warning for modelPlaneVisibility; all return paths are defined, compilation completes, and rendering tests/device initialization succeed. This warning is retained in diagnostics rather than suppressed.

Rollback: close GameNative and restore all three backed-up DLLs together. Keep the existing INIs. The normal Steam-folder launcher was restored after the one-time installer. Geo/.NET, saves, Steam auth, driver/DXVK/Proton and other installed mods were not replaced.

For all rendering tests: build-mobile.cmd --gpu followed by an optional quoted path to your supported cubeworld.exe. NativeClipTest skips with exit 77 if the exact private fixture is absent; no game shader binaries are distributed.
