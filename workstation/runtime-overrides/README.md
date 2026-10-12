# Runtime overrides

These files are exact copies from the callback-audited development tree. Apply each file at the same repository-relative path after checking out the pinned ModernGekko dependency revisions in `Repos/RECOMP_REFERENCE_REPOS.md`.

The override set preserves:

- Runtime automation commands, including the game-owned movie-skip request.
- The persistent guest-call context and safe callback injection boundaries.
- Save-state exclusion while an injected call is active.
- Host floating-point control restoration through the module state-loaded callback.
- The retail-parity cleanup that removes experimental timing, LOD, visibility,
  camera, interaction and asset-preload behavior.
- Fast ordinary startup without the optional full asset-manifest hash; pinned
  releases and tooling can still request complete asset verification.

The earlier patch series under `workstation/patches/` must be applied first. Some patches and overrides touch the same files; the override is the tested final source for this snapshot. Review all changes before applying them to a different upstream revision.
