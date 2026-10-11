# Mobile test — 2026-10-10

Pixel 10 Pro, Android17, GameNative1.2.1, proton-11.0-2-arm64ec-1, System graphics, Wrapper-gamenative, DXVK2.7.1-1-gplasync-0, ALSA, 1280x720. Isolated test prefix; original Steam game/prefix unchanged.

MoonRulesTest: PASS (brightness limits, unchanged daytime/alpha, normalized moon direction, smooth dawn/dusk, night-only cloud dimming).
WorldMotion WindRulesTest: PASS (leaf targeting, motion bounds, temporal continuity, fade and chunk continuity).

Five-file installation (2 DLLs, 3 INIs): zero failures; original files backed up. Exported device INIs hash-match released preset. Phone logs confirm MoonLight attached, shadow resources4096/radius120 ready, WorldMotion GPU leaf animation ready. Motion counters reached clouds15986/foliage12880, marked leaf vertices528952 in this session. Entered copied TEST character; world, terrain, shadows and map render. Geo process starts automatically from internal Tools path in custom Cube launch. TerrainVariance mobile remains installed and CubeMegaMod WorldGen disabled.

Short-session validation only. Night transitions, every weather/biome and long-session stability not exhaustively tested. No frame-rate improvement measurement. Preset retains rendering features and resolution while reducing intensity/movement; fidelity percentage is not objectively measured.
