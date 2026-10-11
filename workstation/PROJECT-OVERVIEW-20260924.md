# Project audit — September 24, 2026

## Position

Big Willy Unleashed RDHE78 boots and supports real interactive gameplay through
a Windows x64 launcher, native recompiled game DLL, and Dolphin-derived runtime.
This is a playable development build, not a finished PC port or a locked-30-FPS
release. The latest completed performance work and packaged binaries are from
September 20. This audit inspected files, configuration, source, hashes and saved
evidence; it did not launch the game or run new gameplay benchmarks.

All three flagship hashes match `logs/flagship-hashes.json`. No dahbwu process
was found during the audit. The latest Furon session ended September 20 at
12:27:35 with exit code 0. Its final status file is stopped; old FPS fields in
that file are not a live measurement. The newest packaged crash dump found is
September 12, not September 20.

## What the build is

- Launch: root `BigWillyUnleashed.exe`, a .NET 10 Windows desktop launcher.
- Runtime: `dist/flagship/dahbwu.exe`, C++ ModernGekko/Dolphin integration.
- Game code: `dist/flagship/gRDHE78_recomp.dll`, 94,070,272 bytes, optimized
  Clang-generated native code targeting AVX2 and strict floating point.
- Content: `workstation/game`, extracted Wii assets; user configuration and
  saves live beneath `workstation/run-user`.
- Source: `github repos/ModernGekko-Template/lib/ModernGekko`, including nested
  Dolphin and recompiler dependencies. Local patches exist at several levels.

One EXE is the supported entry point, but it is not a self-contained game file.
The runtime, DLL, assets, Sys files and .NET runtime remain required. Hardware
validation is on the local Ryzen 9 7950X3D / RTX 3070 / 64 GB machine.

The symbol-rich 7,049,192-byte `input/dah_rls.elf` was the key reverse-engineering
input. Earlier verification matched its executable sections against main.dol and
exported 20,907 named functions. The build contains 249 generated C chunks.
This is static recompilation with runtime fallback and Wii subsystem support,
not a complete hand-reconstructed source decompilation. A host PDB, if present
in compiler output, is not an original Wii game PDB.

## Working systems and boundaries

- Cold startup and the actual main menu work. Historical menu testing sustained
  nominal 59.94 FPS; gameplay uses the original approximately 29.97 FPS timing.
- Crypto movement, aiming/firing, Big Willy entry and movement, and saucer
  gameplay have been exercised. The user has confirmed interactive control.
- Opening-movie transition deadlock was traced to a GPU token acknowledgment
  race and corrected without pretending unfinished GPU work was complete.
  A complete movie-to-gameplay pass succeeded with the original module.
- The optimized module and Ultra build have passed checkpoint gameplay tests.
  A fresh uninterrupted opening-movie-to-gameplay pass on the exact current
  package is still not documented. Campaign-wide completion is not established.
- E calls native vehicle interactions, with verified mech dismount. Saucer
  landing retains native location rules. Every context and steering direction
  has not received comprehensive testing.
- Escape requests native movie/cutscene skip; it does not guarantee that every
  scene or loading state is immediately skippable.
- F1 save states and File/View desktop menus exist. Several gameplay checkpoints
  reload successfully. Original menu-state experiments had hangs; ordinary
  mission-save progression needs broader regression coverage.
- The new vector splash is built into the launcher and visually checked. It
  hands off when the game window exists, not when the whole world has finished
  loading. The inset client-edge border was removed. File/View and the title
  bar intentionally remain in windowed mode; F11 toggles fullscreen.

## Controls and console

Current keyboard profile: WASD movement; mouse pointer aiming/camera; left click
Wii B/fire; right click Wii A/contextual mental action; Space jump/jetpack;
Left Shift Nunchuk Z; arrows Wii D-pad/weapon selection; Q Wii 1; R Wii 2;
Backspace Wii minus; Home Wii HOME. E, Escape, F1, F11 and backtick have native
host handling. Camera movement still derives from Wii pointer behavior; this
is not yet a fully redesigned conventional PC relative-mouse camera.

Backtick console commands:

```text
enter_bigwilly
giveallweapons
list_levels
load level_MAP01_EastCoastCity
load level_MAP03_MidwestTown
load level_MAP04_Jungle
load level_MAP05_Island
```

Mech entry uses native switching and fills its battery; normal damage/drain
remain. Weapon grants unlock 14 weapons and clear their temporary locks; native
inventory flags were checked. This does not grant infinite ammunition. Travel
uses native RequestLoadLevel. Stateful commands require gameplay and reject
during cutscenes to prevent overlapping transitions. Midwest/Island gameplay
and return to the city movie are recorded; equal mission coverage across all
four maps is not established. Full Xbox-style controls remain unfinished.

## Graphics: actual current settings

| Setting | Audited value |
|---|---|
| Launcher backend | Direct3D, explicitly passed on launch |
| Frontend resolution label | 3840x2160 / Ultra4K |
| Internal EFB scale | 6x; 640x528 scaled to 3840x3168 before presentation |
| Anti-aliasing | 8x SSAA |
| Anisotropic filtering | 16x |
| Texture filtering | Forced linear |
| Color/copy filter | True color, Wii copy blur disabled |
| Model selection | Highest-detail distance-selected CMesh path |
| World camera far clip | 20,000 game units |
| Distance fading | Disabled for the hooked object-fade path |
| Asset cache | All 20,967 extracted files, 3,734,995,599 bytes |
| Texture artwork | Original Wii assets; HD replacements disabled |
| Shader mode | Asynchronous ubershaders, shader cache enabled |
| Wait for shaders before starting | **False in current live profile** |

The tested preset files specify shader waiting True, but the current profile
contains False. This audit records the difference without overwriting it. The
frontend config says Vulkan, but the launcher's explicit D3D argument wins.

"Full world" means disc-file bytes cached in host RAM plus extended visibility
for the active map. The engine still constructs sectors/entities and applies
mission visibility; it does not instantiate all maps or actors simultaneously.
Frustum culling and texture mipmaps remain. Highest-detail enforcement applies
to the identified distance-selector path, not a proven override of every
possible engine detail system. Original textures and geometry remain the asset
detail ceiling. This is an aggressive Ultra preset, not literally every numeric
setting at its maximum. Extreme8K is available but much slower.

## Performance evidence

These are short, scene-specific FPS samples, not per-frame percentile data.

| Test | Mean FPS | Minimum sampled FPS | Evidence |
|---|---:|---:|---|
| Historical main menu, ~60 seconds | 59.923 | 59.738 | logs/fps-shell-final |
| Original-distance idle dock, 45 samples | 29.956 | 29.920 | logs/gameplay-perf-20260912-170630 |
| Player combat, 60 seconds, older settings | 26.700 | 19.834 | human-play-20260912-171603 |
| Ultra4K saucer, 30 samples | 26.429 | 25.353 | logs/gameplay-perf-20260920-122128 |
| Ultra4K Big Willy walking, 30 samples | 20.272 | 17.174 | logs/gameplay-perf-20260920-122241 |
| Extreme8K saucer, 30 samples | 17.209 | 15.019 | logs/gameplay-perf-20260920-122402 |

The three September 20 tests exited with code 0. At 8K the GPU reached 100%
usage and about 7 GB VRAM, demonstrating a GPU limit in that setup. Earlier
lower-resolution CPU sampling points to generated-code execution, floating-point
helpers and runtime dispatch overhead. This does not identify one universally
dominant bottleneck. Ultra scene tests cannot establish whole-game stability.

At 20.272 FPS, mean throughput corresponds to roughly 49.3 ms/frame; 30 FPS
requires 33.3 ms. That scene needs roughly one-third less frame time, or 48%
more throughput. Reaching 30 only in quiet scenes is not the remaining goal.

## Loading, crashes and diagnostics

Duplicate asset verification was removed and SHA-256 accelerated with Windows
BCrypt while retaining verification. Earlier warm startup improved from about
33.3 to 7.8 seconds before adding preload/Ultra settings. September 20 test
checkpoints rendered after roughly 18–22 seconds; those are not cold-disk or
full opening-movie timings. One September 20 launch spent about 186 seconds
verifying files; its cause was not established. That session was force-closed
on request, yielding -1, which is not evidence of a spontaneous game crash.

Furon logs capture stdout/stderr, startup events, effective graphics settings
and exit status. The current launcher truncates the Furon file on each new
session; a complete archived session history is not guaranteed. Crash dumps
are separate. FAST_STATS disables expensive dispatch counters, so zero counts
in a normal shutdown log do not prove zero fallback execution.

Automation commands operate on the game rather than the desktop: pad input,
bounded frame inputs, release, screenshots, state load/save, memory probes,
native commands and stop. The browser controller includes a 120-sample FPS
history. Benchmark/observation scripts, CPU sampling, ELF disassembly and symbol
lookup tools exist. Per-frame CPU/GPU timing and automatic dip-context captures
are not yet implemented; ordinary crash logs cannot diagnose every stutter.

Useful checkpoints: performance-baseline.sav (Crypto dock), charged-bigwilly.sav,
city-combat-performance.sav and midwest-saucer.sav, under interactive-automation.
The earlier bigwilly-performance.sav has partial battery and is unsuitable for
long mech tests without accounting for automatic exit.

## Source preservation and reproducibility

The root Git repository is on an unborn master branch: no commits and no tracked
files were returned. Its .gitignore excludes github repos and workstation/tools,
which include major source modifications, the launcher and diagnostics. Nested
repositories do have upstream history; ModernGekko has local modifications.
That does not provide a versioned baseline of our complete current project.
Patch files and local executable/config backups exist, but seven patch files
must not be assumed to capture every subsequent change. No remote backup or
fresh-machine rebuild was verified by this audit.

The supported build orchestrator is workstation/build.ps1. It establishes the
R: workspace alias, builds runtime/module/launcher and packages. Individual
incremental scripts can fail if R: is absent. Nested Git metadata has emitted
bad-HEAD/submodule errors during otherwise successful builds. Original MSVC and
experimental module outputs are retained; the shipped module is the all-Clang
strict-FP build, not the discarded fast-math/fallback or ThinLTO experiments.

README.md and parts of PERFORMANCE.md contain historical statements that are
now stale (gameplay not reached, old controls, old startup times). Use this dated
audit and the cited evidence for current status. Build and game behavior were
not changed during this overview.

## Recommended next work

1. Preserve a complete source/config/tool baseline with recoverable history,
   appropriate asset exclusions, pinned dependency revisions and off-machine backup.
2. Add rolling per-frame CPU/GPU timing and automatic dip capture, with archived
   session logs. Diagnose loading and shutdown stalls separately from crashes.
3. Run repeatable A/B scene tests for resolution, SSAA, distance and model detail;
   optimize measured CPU and GPU costs without changing game timing semantics.
4. Validate cold boot, full opening cutscene, Crypto walking, combat, mech entry/
   exit, saucer landing, all-map travel, saving/reloading and clean shutdown on
   the exact current package; then extend duration and mission coverage.
5. Finish input polish and a proper gamepad profile. Keep Ultra and performance
   settings explicit rather than describing one as a universal maximum.

The foundational boot/playability milestone is achieved. The central unmet
milestone is consistently paced 30 FPS through cutscenes, gameplay and combat,
with comprehensive stability and reproducible project recovery.
