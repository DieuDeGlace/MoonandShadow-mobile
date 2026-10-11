# SteamShadows

Made with the aid of AI (OpenAI ChatGPT/Codex), by DieuDeGlace.

D3D11 sun shadows with a 100-block default radius, loaded terrain casters and cached cloud casters. MoonLight provides nighttime shadow direction. Scene bindings are released before state restoration to allow swap-chain resizing. Clouds must have been observed before their cached models can cast.

See [root build instructions](../README.md#build-on-windows-x64). Build only this mod with `cmake --build build --target SteamShadows` from the repository root. Output: `build/SteamShadows.dll`. Configuration: `Mods/SteamShadows.ini`. Close the game to install or remove hooks; restart after binary/config changes.

## Crisp shadow edges

`SoftShadows=0` is the new default: one hardware-filtered comparison sample instead of the old nine-tap blur. A narrow antialiased edge is retained. `SoftShadows=1` restores the previous softer nine-tap filter. Settings are read at startup. The supplied configuration now uses a 3072 shadow map and a 100-block radius; the code fallback when no configuration exists remains 2048. No additional render pass is introduced. Texture sampling work is reduced, but this is not a measured overall FPS guarantee.

The graphics fixture measured an edge transition of 8 pixels versus 20 with the previous filter at the same map size. Shadow coverage and scene-target release checks passed, along with the other nine tests. The fixture deliberately uses a low-resolution map to expose filter width; these pixel counts do not describe normal gameplay. In-game sharpness, aliasing and performance still depend on camera, light angle and scene complexity.

The 3072 preset improves shadow texel density by 50% over 2048 at the same radius. Its D32 map uses 36 MiB instead of 16 MiB, and has 2.25 times as many texels; caster fill work may increase. It retains single-sample hardware comparison filtering and texel snapping. This is a quality/performance compromise, not a measured FPS guarantee. Restore `Resolution=2048` if frame times worsen. SMAA/TAA are not added by this setting. Restart after changing resolution.

Local light handling: captured native point lights reduce directional shadow darkening using native vertex attenuation. An active player lantern adds a warm 12-block falloff within the added shadows when native local lighting is absent. This is an approximate fallback, not an exact restoration of native lantern quality/radius. It adds no pass, texture, GPU readback, or local-light shadow maps. Lights do not gain independent wall occlusion from this change; GPU cost is not measured in game.

The supplied daytime preset uses StrengthPercent=60 (previously 48) as an initial darker match toward vanilla shade. This opacity adjustment adds no rendering work. Exact visual parity is not established; native shading depends on surface, sunlight and time. MoonLight's night shadow strength is configured separately and remains 20%. Restart to apply.

Sun/moon shadow angles now follow the world clock rather than the fixed native lighting vector. Directional strength fades near the horizon at dusk/dawn; moon rise is at 18:00 and set at 06:00. The moon disk shares this clock-driven trajectory. No additional shadow map or pass is added. Native daytime material lighting is unchanged. Live visual verification remains required.

Moving-leaf compatibility: both shadow receivers and casters resolve WorldMotion's published animated terrain mesh at replay time. This removes a static/moving mesh mismatch when motion runs after shadow capture. D3D11 tests cover both hook orders, motion toggle-off and preservation of non-terrain draws. No added rendering pass or GPU readback. In-game visual confirmation remains necessary.

Replay optimization: an internal pass-scoped cache skips unchanged vertex/index/layout/topology bindings and identical constant uploads. It resets when shadow context ownership begins and ends; draws outside that pass bind explicitly. Resolution, radius, shading, geometry, motion, light handling and all shaders are unchanged. GPU pixel comparisons cover all six normals, entity bias and lantern lighting; a repeated-bind test verifies redundant calls are eliminated. In-game FPS and GPU timing gains have not been measured.
