<div align="center">

# Destroy All Humans! Big Willy Unleashed — Static Recompilation

**A work-in-progress native Windows port of the 2008 Wii release, built with ModernGekko and DolRecomp.**

[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4?style=flat-square)](#requirements)
[![Target](https://img.shields.io/badge/target-Nintendo%20Wii-E60012?style=flat-square)](#how-it-works)
[![Accuracy target](https://img.shields.io/badge/accuracy%20target-retail%2030%20FPS-734F96?style=flat-square)](#status)
[![Repository checks](https://img.shields.io/github/actions/workflow/status/Zippingrocks/Destroy-All-Humans-Big-Willy-Unleashed-2008-Static-Recompilation/repository-checks.yml?branch=main&style=flat-square&label=checks)](https://github.com/Zippingrocks/Destroy-All-Humans-Big-Willy-Unleashed-2008-Static-Recompilation/actions)

[Status](#status) · [Requirements](#requirements) · [Building](#building-from-source) · [Known issues](#known-issues) · [Contributing](#contributing)

</div>

This project statically translates the PowerPC code from the North American Wii release of *Destroy All Humans! Big Willy Unleashed* (`RDHE78`) into native x86-64 code. A modified ModernGekko runtime provides the Wii CPU, memory, graphics, audio and input behavior expected by the game.

The repository does **not** contain the game or its assets. You must supply files from your own legally obtained copy.

> [!IMPORTANT]
> This is active reverse-engineering work, not a finished PC release. The current build is useful for development and compatibility testing, but the complete campaign has not been validated.

## Status

| Area | Current state |
|---|---|
| Windows x86-64 | Native development build |
| Boot and front end | Boots through the original movies to the title screen and main menu |
| Gameplay | Crypto, Big Willy and saucer checkpoints have run without a crash in bounded tests |
| Input | Keyboard/mouse development profile; controller behavior remains active work |
| Movies | Original movies play; `Escape` requests the game's normal skip path |
| Graphics | Direct3D output works; maximum-detail and full-world modes are experimental |
| Commands | `giveallweapons`, `enter_bigwilly`, `list_levels` and `load level_NAME` development commands exist |
| Callbacks | All 20,907 named functions and 24,124 static code-pointer candidates have generated entry coverage |
| Timing | Retail 30 FPS is the gameplay target; maximum-detail gameplay is not locked to 30 FPS |

The callback audit exercised all 29,260 indirect branch taken paths against the original module and the candidate module. A live test observed 3,730 distinct indirect edges to 2,431 symbol-mapped targets with no unknown target. That is strong evidence for the audited paths, not proof that every campaign path is complete.

## Features

- Native x86-64 runner and game module produced from statically translated Wii code.
- Original menu, movies and game-world rendering.
- Keyboard and mouse development controls with a centered Wii pointer path.
- Scriptable runtime control for unattended boot, input, screenshots, memory reads and checkpoints.
- Crash logging, callback tracing, frame timing and repeatable gameplay probes.
- Safe injected guest-call contexts that preserve Wii CPU state and stack linkage.
- Host floating-point control synchronized with the emulated Wii FPSCR.
- Optional full-world draw distance and highest-model-detail development modes.

## Requirements

### Running a development build

- Windows 10 or Windows 11, 64-bit.
- A 64-bit x86 processor and a current Direct3D-capable GPU.
- The extracted files from a legally obtained North American Wii copy (`RDHE78`).
- The .NET Windows Desktop runtime used by the launcher.

### Building

- Visual Studio 2022 with **Desktop development with C++**.
- A current Windows SDK.
- CMake and Ninja.
- Python 3.11 or newer.
- PowerShell 5.1 or newer.

## Game files

Game executables, disc images, extracted assets, movies, saves, generated translation output and runtime captures are intentionally excluded from Git. Keep local inputs in the workspace paths expected by the scripts under `workstation/`.

The verified analysis input is the North American `RDHE78` release. Other regions are not interchangeable because code addresses, symbols and generated modules differ.

## Building from source

The current development workspace builds the runtime, prepares symbols, recompiles the game module, builds the launcher and refreshes the flagship package with:

```powershell
.\workstation\build.ps1
```

The root `BigWillyUnleashed.exe` is the local flagship launcher after packaging. Compiled binaries are never tracked in this repository.

This first public source snapshot preserves the build orchestration, runtime patches, configuration templates and validation tools. A clean-room bootstrap that fetches every pinned dependency and regenerates the translated module from a user-supplied game is still being consolidated; fresh clones should be considered developer source snapshots rather than one-command builds today.

## Controls and configuration

The current keyboard/mouse development profile uses:

| Action | Input |
|---|---|
| Move | `W` `A` `S` `D` |
| Look / Wii pointer | Mouse |
| Primary action / Wii A | Left mouse button |
| Secondary action / Wii B | Right mouse button |
| Enter or exit vehicle | `E` |
| Skip movie / back | `Escape` |

Bindings are still being refined for parity with the Wii Remote and Nunchuk. See `workstation/config/KeyboardMouse.ini` for the development profile.

## Known issues

- The complete campaign and all side missions have not received end-to-end validation.
- Maximum-detail, full-world gameplay is not locked to 30 FPS. One bounded test averaged roughly 15 FPS on the development machine.
- Audio, effects, visibility transitions, later missions, death/respawn and long sessions need broader testing.
- Mouse and vehicle controls remain development-oriented and need more retail-behavior comparison.
- The full-world mode deliberately changes retail draw-distance behavior and can create severe CPU/GPU and memory pressure.
- Public source bootstrapping and dependency restoration need more work before fresh clones are self-contained builds.

## Reporting bugs

Use the GitHub issue forms and include the exact commit, mission/location, reproduction steps, expected retail behavior, hardware details, and a reviewed log or screenshot when useful. Never upload a disc image, original executable, extracted asset, movie or save containing private information. Logs can contain local paths; inspect them before posting.

## How it works

DolRecomp translates the retail Wii executable's PowerPC machine code into C. That generated code is compiled into a native module. ModernGekko supplies the host runtime and emulates the Wii facilities that the translated code expects. Game-specific fixes are kept in runtime patches, host integration and narrowly scoped tooling where practical.

Runtime behavior is recovered from the original ELF/DOL, symbols, controlled traces, live callback targets and repeatable gameplay captures. A named or reachable function is not considered behaviorally correct solely because it compiles.

Pinned upstream revisions and their purpose are recorded in [`Repos/RECOMP_REFERENCE_REPOS.md`](Repos/RECOMP_REFERENCE_REPOS.md).

## Project layout

```text
.github/                       Repository checks and issue forms
Repos/                         Upstream revision and provenance notes
workstation/                   Build, package and status documentation
  config/                      Keyboard/mouse and graphics templates
  patches/                     Reproducible upstream runtime patches
  runtime-overrides/           Exact tested callback/runtime source overrides
  tools/                       Audit, automation and parity tools
```

Local game files, generated C, dependency checkouts, builds, packages, logs, captures, checkpoints and saves are excluded.

## Accuracy and validation

The project targets original 30 FPS gameplay behavior. Validation combines static entry coverage, low-level indirect-branch tests, live callback tracing, bounded play scenarios, screenshots, runtime counters and clean-exit checks. Current callback evidence is summarized in [`docs/CALLBACK_AUDIT.md`](docs/CALLBACK_AUDIT.md).

## Roadmap

- Complete stable retail-speed playthroughs of every mission and side activity.
- Hold 30 FPS at original visual settings before treating higher settings as release features.
- Compare controller, camera, vehicle, weapons, audio and effects behavior against the Wii release.
- Convert reached callback edges and gameplay sequences into unattended regression checks.
- Finish the fresh-clone bootstrap around user-supplied game data.
- Package a clean player-facing launcher without redistributing copyrighted assets.

## Contributing

Bug reports, retail-behavior evidence, documentation and focused fixes are welcome. Read [`CONTRIBUTING.md`](CONTRIBUTING.md) before opening a pull request. Changes that affect behavior should identify the retail evidence and provide a repeatable validation path.

## Legal

This is an unofficial community project. It is not affiliated with or endorsed by THQ, Locomotive Games, Nintendo, Sony, Microsoft, Pandemic Studios, Black Forest Games or the current owners of the *Destroy All Humans!* intellectual property.

*Destroy All Humans!* and related names, characters, artwork and assets belong to their respective owners. Contributors must not submit copyrighted game data, disc images, executables, extracted assets, movies or other material they do not have permission to distribute. Third-party components retain their original licenses.

## Credits

- Locomotive Games and THQ — original Wii game.
- Expansion Pak and ModernGekko contributors — Wii static recompilation runtime and template.
- DolRecomp and decomp-toolkit contributors — translation and extraction tooling.
- Dolphin contributors — Wii hardware and runtime research.
- Project contributors and testers — analysis, validation and compatibility work.
