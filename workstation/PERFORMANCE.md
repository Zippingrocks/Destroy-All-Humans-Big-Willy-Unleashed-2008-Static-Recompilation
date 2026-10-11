# Performance verification

## September 20 Ultra graphics

The default is now Ultra4K with 8x SSAA, 16x anisotropic filtering, maximum
distance-selected mesh detail, and extended visibility. Controlled tests averaged
26.429 FPS in the Midwest saucer scene and 20.272 FPS while Big Willy walked.
Both exited cleanly. Extreme8K averaged 17.209 FPS in the saucer scene and is
optional. These supersede earlier measurements for the default graphics profile;
this is not a locked 30 FPS build. See `notes/graphics-quality-20260920.md`.

## Extended-world build, September 12

The flagship now defaults to full asset preloading and extended visibility. A
controlled 30-sample Midwest saucer idle run averaged 26.238 FPS (minimum 24.897,
frame-counter rate 26.261). Evidence: `logs/gameplay-perf-20260912-182153`.
The runtime became running after 13.046 seconds and resumed checkpoint rendering
after 18.046 seconds, including the 3.73 GB RAM preload. These are runtime test
timings, not a cold filesystem or full flagship-launch measurement.

Set `full_world=0` in root `bigwilly.conf` to restore original visibility; keep
`preload_assets=1` to retain the cache. Both require restart. Loading disc bytes
ahead of time does not eliminate the engine's world construction or shader work.
The earlier 30 FPS dock results below are scene-specific, with original distance.

The matching original-distance run (`logs/gameplay-perf-20260912-182306`) averaged
25.343 FPS, minimum 22.980, counter rate 25.366. Both runs exited cleanly. This small
single-run difference is not evidence that extending distance improves performance.

Current September 12 gameplay and startup measurements are in
`notes/gameplay-performance-20260912.md`. The optimized flagship holds the
nominal 30 FPS rate in the verified idle dock test. Player-controlled combat
still has drops; it is not yet a locked 30 FPS build throughout gameplay.

## Historical frontend verification — 2026-09-09

Release runtime rebuilt and tested with the existing 249-chunk native game DLL. Direct3D, normal emulation speed, no CPU overclock, local run-user profile; audio muted during measurement. No savestate was loaded.

The actual New Game / Load Game / Options / Multiplayer screen was visually verified before and after measurement. A Wii pointer override visibly moved the cursor, verifying input response. 120 status samples over 59.594 seconds reported mean 59.9233 FPS (minimum 59.7384, maximum 60.0328), mean 59.9239 VPS, and mean speed 1.00085. Frame-counter delta divided by elapsed wall time independently gave 59.8047 FPS. No crash occurred during the session or measurement.

Evidence: `logs/fps-shell-final/benchmark.json`, `automation/stage.png`, `automation/menu-final.png`, and `stderr.txt`. Reproduce sampling with `python workstation/measure-menu.py <run-directory> --seconds 60` after visually identifying the main menu.

The test stopped unpaused and exited cleanly. Shutdown reported 895,477,470 native dispatches, 1,004,622,106 hook fallback instructions, and zero failed code-integrity checks across 210 verifications. Dispatches and hook instructions use different units and are not a coverage percentage.

## Implementation

- Match dcbf/dcbst + addi register,32 + bdnz cache-line loops before batching. Preserve invalidation and bound each batch by remaining cycles and 256 lines. Other cases retain the scalar path.
- Recognize SelectThread's idle loop at 0x803654F0 after checking its original instruction.
- Identify Game::spGameMode at 0x8053A640 using ELF-derived vtables: CGameModeNull 0x803FD4F8, CDAHGameModeShell 0x80442244. The shell object was also read from live guest memory.
- At Pcpl::TimeSync::_updateTimer (0x802CBC78), patch the main clock at 0x8047B668 from 33.3667 ms to 16.68335 ms; halve its maximum accumulation and set WiiFrameSync::suFlipRetraceInterval (0x80539E3C) from two retraces to one. Restore original values when leaving the frontend.
- At the verified game timer polling site 0x80022440, only yield when no simulation turn/render is due. Poll at most 1 kHz while advancing hardware events and allowing interrupts. Retain the deadline across interrupt returns so frequent IRQs cannot indefinitely postpone the game.

This is a Dolphin-derived static-recompilation runtime with fallback support. Full gameplay, gameplay timing restoration, audio, and savestate reload are not validated. Nominal 60 FPS here means the Wii's approximately 59.94 Hz cadence, not an exact 60.000 Hz clock. Earlier menu savestates froze or failed on reload; use cold boots. Stop unpaused for clean shutdown.
