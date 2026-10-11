# Contributing

Thank you for helping improve the *Big Willy Unleashed* static recompilation.

## Before opening a change

- Keep original game executables, disc images, extracted assets, movies and saves out of the repository.
- Search open issues and describe the retail behavior you are trying to recover.
- Keep generated translation changes separate from hand-written runtime changes when possible.
- Do not present inferred symbol names or behavior as original developer information.

## Behavior changes

A gameplay, rendering, timing, input or runtime change should include:

1. A concrete trigger and reproduction path.
2. The observed recompilation behavior.
3. Evidence for the Wii retail behavior.
4. The smallest responsible code change.
5. A repeatable validation path and nearby regression coverage.

Useful evidence includes original code disassembly, symbol-backed call paths, live callback traces, controlled screenshots, timing captures and bounded gameplay scenarios. State what the evidence proves and what remains untested.

## Pull requests

- Use a focused title and explain the resulting behavior.
- Mention the tested game ID and exact scenario.
- Include relevant build or test commands and results.
- Review logs for local paths or private data before attaching them.
- Avoid unrelated formatting or generated-file churn.

By contributing, you confirm that you have the right to submit the material and that it contains no copyrighted game data you lack permission to redistribute.
