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
