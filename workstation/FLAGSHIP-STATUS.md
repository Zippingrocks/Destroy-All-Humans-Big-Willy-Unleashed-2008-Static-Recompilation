# Flagship launch status — 2026-09-12, extended-world build

## September 20 graphics update

Ultra4K is now the default: 6x internal resolution, 8x SSAA, 16x anisotropic
filtering, linear texture filtering, true color, and no Wii copy blur. Root
`max_detail=1` keeps distance-selected meshes at their nearest/highest-detail
model. Extended distance and the full asset cache remain enabled. Original
texture artwork is used; no replacement texture pack is installed.

This supersedes the original-LOD description below. Effective graphics values
are logged after backend validation. Tested averages: 26.429 FPS in the saucer
scene and 20.272 FPS during Big Willy walking. 8K plus 8x SSAA averaged only
17.209 FPS in the saucer scene, so it remains an optional preset. Details,
backups, and switching commands: `notes/graphics-quality-20260920.md`.

## Extended world and commands

The root launcher now enables a host-RAM asset cache and extended visibility by default.
`bigwilly.conf` beside the EXE controls them independently: `preload_assets=0` disables
the cache; `full_world=0` restores original draw distance. Restart after changing either.

The cache successfully preloaded 20,967 files / 3,734,995,599 bytes. The test process
used approximately 4.9 GB working-set RAM. All extracted disc file data is resident;
the original Wii engine still instantiates sectors, entities, and mission content on
its own schedule. This does not merge separate maps or spawn every mission's actors.
Startup now includes this up-front read, so the older 7.762-second launch result below
does not describe this configuration.

The active world camera's far clip is 20,000 game units (observed original values:
500 in Midwest, 400 in the city). Distance-only object fading is disabled. Frustum
culling, original LODs, near clipping, and scripted visibility remain intact.
Midwest gameplay, travel to Island gameplay, and return to the city opening movie
passed with the cache enabled and a clean shutdown. An uncontrolled 35-second
Midwest sample averaged 25.004 FPS, minimum 16.789; this is not a locked 30 FPS mode.

Open the in-game console with backtick:

```text
enter_bigwilly
giveallweapons
list_levels
load level_MAP01_EastCoastCity
load level_MAP03_MidwestTown
load level_MAP04_Jungle
load level_MAP05_Island
```

`enter_bigwilly` uses native avatar switching and fills the mech's battery. Normal
damage and drain remain. `giveallweapons` unlocks and removes temporary locks on
the game's 14 weapons; it does not grant infinite ammunition. All 14 native inventory
flags were verified. Level travel uses native RequestLoadLevel. Commands require
active gameplay and reject during cutscenes; wait for travel to finish before another.

E requests the native contextual vehicle action, including Big Willy's dismount;
saucer landing still needs a valid landing location. Keyboard/mouse profiles move
the former E secondary action to R. Saucer turning now reads Crypto's pointer-camera
axis instead of Wii tilt. Forced mech entry, movement, and native exit to Crypto were
verified. Saucer steering feel and every landing context still need player feedback.

Evidence: `logs/bigwilly-entry-20260912-181854`, earlier command tests in
`logs/bigwilly-entry-20260912-180523` and `logs/bigwilly-entry-20260912-181011`.

Open `BigWillyUnleashed.exe` at the workspace root. It launches the packaged runtime and optimized native module in `dist/flagship`. Keep that folder, `workstation/game`, and `workstation/run-user` in place. This is one launch executable, not a standalone copy of all game data. The installed .NET 10 Windows Desktop runtime is required.

## Earlier measurements with original draw distance

- Idle dock gameplay: 45 samples, 29.956 FPS average, 29.920 minimum, 29.991 frame-counter FPS, clean exit. Normal game speed and 1920x1080 internal resolution.
- The full generated-code Clang build is 94,070,272 bytes, compared with 292,519,424 bytes for the original module. Strict floating-point settings are preserved. This optimized build targets AVX2 and was tested on the local Ryzen 9 7950X3D / RTX 3070 machine.
- Startup to the game window fell from about 33.3 seconds to 7.762 seconds in the measured warm launches. Assets are still fully verified; the duplicate hash pass was removed and SHA-256 uses Windows acceleration with a portable fallback.
- Controller release now actually removes overrides. The old implementation held all controls at zero. Initial registration also starts released. The user is visibly moving and firing in the corrected build; no input-driving controller is attached.
- The current game was opened using the same flagship executable with `--resume-gameplay`, restoring the verified dock checkpoint. Ordinary double-click launch still starts normally.

## Remaining validation

Combat can dip below 30 FPS: a 45-second passive player-controlled sample averaged 28.705 FPS with a 25.126 minimum; a later 60-second sample averaged 26.700 FPS with a 19.834 minimum. Do not advertise a locked 30 FPS throughout combat.

The complete opening movie-to-gameplay transition passed with the original native module and the corrected GPU token handling. The optimized module has passed gameplay checkpoint tests; a fresh complete movie pass on that module remains outstanding. Do not interrupt the user's running session to do it.

## Building and fallback

`workstation/build.ps1` builds the runtime, original game module, optimized module, launcher, and package. To rebuild only the optimized module, run `workstation/build-hot-module.cmd --all --jobs 8` after the original game build. Package only while the flagship game is closed.

`workstation/package.ps1` selects the optimized module by default. The untouched original module remains under `workstation/build/game-module`. A deliberate fallback can be packaged with `-GameModule build/game-module/gRDHE78_recomp.dll`.

The package avoids launching a build output while it is being linked. The launcher checks same-session existing instances, repairs an empty keyboard/mouse profile, clears experimental input/timing environment variables, and reports missing files. Hashes are in `workstation/logs/flagship-hashes.json`.

Launch logs: `workstation/logs/interactive-launch.log` and `workstation/logs/bigwilly.furonlog`. Live status: `workstation/interactive-automation/status.txt`. Performance evidence and limitations: `workstation/notes/gameplay-performance-20260912.md`. Historical test executables are not the supported entry point.
