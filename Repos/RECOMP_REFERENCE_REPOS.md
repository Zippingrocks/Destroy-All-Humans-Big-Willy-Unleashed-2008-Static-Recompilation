# Recompilation dependencies and references

The project uses local checkouts during development. Those checkouts are not vendored into this repository; this file records their provenance so the public source can be reconstructed deliberately.

| Project | Upstream | Pinned revision | Purpose |
|---|---|---|---|
| ModernGekko-Template | https://github.com/ExpansionPak/ModernGekko-Template | `eedda2b02dde3aefc02796d859f0033b916aad03` | Project and build template |
| ModernGekko | https://github.com/ExpansionPak/ModernGekko | `5417826c31187d4dadf8588c7aa25bf107782936` | Host runtime |
| RecompCore | https://github.com/ExpansionPak/RecompCore | `55c7b023fa0f4eba1cf3fdbbb25b1c5ec468d5ac` | CPU/runtime backend used by the pinned ModernGekko checkout |
| DolRecomp | https://github.com/encounter/dol-recomp | `40637c4683bd2820ac5b23607ee344720beb26df` | PowerPC-to-C translation tool recorded by the workspace lock file |
| decomp-toolkit | https://github.com/encounter/decomp-toolkit | `e4219e7644fb7b96d920d5bc3d1d950f5569dcaf` | Wii executable extraction and analysis |

The native module that produced the validated callback build used the DolRecomp revision pinned by the ModernGekko dependency graph. Before updating any dependency, record the old and new commit, rebuild the callback/ABI tests, and exercise boot, movie skip, menu, Crypto, Big Willy and saucer scenarios.

Local modifications are preserved under `workstation/patches/` and in the source history. A patch name describes its purpose; it is not evidence that every runtime path affected by the patch has been validated.
