# Mobile test — 2026-10-10

Pixel 10 Pro, Android17, GameNative1.2.1, proton-11.0-2-arm64ec-1, System graphics, Wrapper-gamenative, DXVK2.7.1-1-gplasync-0, ALSA, 1280x720. Isolated test prefix; original Steam game/prefix unchanged.

MoonRulesTest: PASS (brightness limits, unchanged daytime/alpha, normalized moon direction, smooth dawn/dusk, night-only cloud dimming).
WorldMotion WindRulesTest: PASS (leaf targeting, motion bounds, temporal continuity, fade and chunk continuity).

Five-file installation (2 DLLs, 3 INIs): zero failures; original files backed up. Exported device INIs hash-match released preset. Phone logs confirm MoonLight attached, shadow resources4096/radius120 ready, WorldMotion GPU leaf animation ready. Motion counters reached clouds15986/foliage12880, marked leaf vertices528952 in this session. Entered copied TEST character; world, terrain, shadows and map render. Geo process starts automatically from internal Tools path in custom Cube launch. TerrainVariance mobile remains installed and CubeMegaMod WorldGen disabled.

Short-session validation only. Night transitions, every weather/biome and long-session stability not exhaustively tested. No frame-rate improvement measurement. Preset retains rendering features and resolution while reducing intensity/movement; fidelity percentage is not objectively measured.

## Updated upstream shadows — e6463d (2026-10-10)
Merged upstream through e6463d: world-unit contact bias, consistent away-facing side shade, and updated shadow reception/filtering. Mobile executable guard retained. Existing Mobile75 settings retained (4096 resolution, radius120, strength45; wind preset unchanged).
FilteringTest passed on Windows D3D11 WARP: all six block faces, consistent caster boundaries, native point lights, crisp filtering and resource release. Installed DLL checksum matched build on Pixel10 Pro; renderer initialized and copied character entered Groron Hills. This is a short launch/world test, not long-session or frame-rate validation. Previous phone DLL backed up before replacement.
`n2026-10-11: upstream 4fcc0bb + 60fabd3 merged; mobile guard and preset retained. Updated FilteringTest PASS, including zero dark pixels on an unoccluded angled plane. Phone disconnected; this update not device-tested or installed.
