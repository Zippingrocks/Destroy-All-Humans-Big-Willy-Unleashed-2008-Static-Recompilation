# Callback audit — October 11, 2026

This audit checked the North American `RDHE78` Wii executable and the generated native game module.

## Static inventory

| Measurement | Result |
|---|---:|
| Named original functions | 20,907 |
| Missing generated entries | 0 |
| Data-section code-pointer candidates | 24,124 |
| Candidates without generated coverage | 0 |
| Indirect branch instructions | 29,260 |
| Linked indirect calls | 6,980 |

Data-section values are candidates because an aligned executable address can also be ordinary data. The inventory does not claim that every candidate is registered or reached at runtime.

## Low-level validation

All 29,260 indirect branch instructions were exercised through their taken paths against the original and candidate DLLs. The harness validates branch-target masking, link-register behavior and relevant CR, CTR and GPR preservation. It does not execute every function body.

A separate floating-point callback test exposed a real defect: state restoration updated guest summary flags but did not restore the host rounding and denormal controls from the Wii FPSCR. The corrected callback passed all eight tested rounding/denormal combinations.

## Live validation

A clean bounded run reached the menu and exercised movie skipping, Crypto, Big Willy and the saucer. It observed:

- 3,730 distinct indirect CTR edges.
- 2,431 symbol-mapped targets.
- Zero unmapped live targets.
- Zero failed self-modifying-code verification events at shutdown.
- Clean process exit.

The scenario ran `giveallweapons`, entered Big Willy and drove 30-second input intervals in the three tested control modes. Runtime callback tracing and frequent state reads impose overhead, so those samples are correctness probes rather than performance benchmarks.

## Injected guest-call correction

Movie skipping and game commands now use a persistent guest-call context. It preserves the complete emulated CPU state and the caller's 64-byte stack-linkage area, survives runtime pause/resume, and recognizes a return only when both the program counter and stack pointer match. Movie teardown is requested only at observed safe boundaries in `Game::Run` or `VideoPlayer::Update`, and game-command injection cannot overlap movie-skip injection.

Save-state creation is blocked while either injected operation is active. On restoration, the module's state-loaded callback reestablishes host floating-point controls.

## Limits

This proves entry coverage and the tested branch/callback semantics. It does not prove that every function body, dynamically constructed pointer, mission script, multiplayer path or complete campaign sequence behaves like retail. Future playthroughs should retain callback tracing for newly reached paths and convert reproducible divergences into focused regression cases.
