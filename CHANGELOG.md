# Changelog

## Unreleased

- Established retail parity as the baseline: original frame timing, draw distance, LOD, camera, vehicle interaction and streaming behavior.
- Removed the 60 FPS menu patch, full-world rendering, forced maximum LOD, PC camera remap, forced `E` interactions and whole-disc RAM preloading.
- Retained the developer console and explicit development commands.
- Avoided hashing the entire extracted asset tree on ordinary launches unless a release explicitly pins an asset-manifest hash.
- Restored the flagship graphics profile to the native 1x render target with no forced MSAA, SSAA, anisotropy or texture filtering.
- Established the public source repository and legal boundary around user-supplied game data.
- Documented the current native Windows boot, menu and bounded gameplay status.
- Added callback coverage and live indirect-target validation documentation.
- Preserved build, package, runtime patch, configuration and validation tooling.
- Fixed injected movie-skip guest-call lifetime, stack-linkage preservation and safe-point handling.
- Restored host floating-point control state from the emulated Wii FPSCR.
