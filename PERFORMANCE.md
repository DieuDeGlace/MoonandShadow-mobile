# GPU performance measurements

Made with AI assistance. These measurements retain the existing visual settings.

The mod samples its GPU pass once every 60 executions using a four-slot timestamp-query ring. Results are polled with `D3D11_ASYNC_GETDATA_DONOTFLUSH`; unfinished samples are skipped without waiting. Disjoint timestamps are rejected. Logs report averages of ten completed samples in milliseconds.

MoonandShadow writes `Mods/SteamShadows.log`: first is shadow-map generation; second is receiver application. TemporalAA writes `Mods/TemporalAA.log`: first is input copy and setup; second is resolve and output-to-history copy.

These measure GPU execution of the named passes, not total frame time, CPU capture overhead, or FPS. Sparse timestamps still have a small instrumentation cost. Compare the same location, camera, resolution, and settings after loading has settled. Test both enabled, shadows only (F9 off), TAA only (F7 off), and both disabled. Preserve ReShade settings throughout.

## Receiver local lights

Receiver parameters compact active native point lights in their original order. The vertex shader iterates only active slots instead of testing ten slots on every vertex. Position, radius, color, attenuation, and accumulation order are preserved; lantern protection is unchanged. No shadow quality setting changes. GPU tests compare compacted and original pixels across six normals, with sparse local lights and lantern combinations. Expected savings depend on mesh complexity and active-light count; in-game timing is still needed.
