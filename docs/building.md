# Build

The repository contains port source and an encrypted owner input bundle. Original game inputs, generated C, local device records, encryption keys, and decrypted app packages are excluded from Git.

## Local Windows host

1. Put the matching three owner-provided inputs in `binaries/` and the PC game data in `game/`.
2. Install Python, Zig, and `requirements-recompiler.txt` in your own environment.
3. Run `python -B tools/recompile.py` and `python -B tools/build_host_local.py`.
4. The native host is written to `recomp/build/`. Test logs and saves belong under `recomp/run/`.

`python -B tools/test_mmx.py` compares the portable MMX implementation with the real x86 CPU. Other recompiler differential tests are under `recomp/tests/`.

## Owner iOS workflow

The local key is a random base64 AES-256 key stored in ignored `secrets/build.key`. Configure the same value as the repository secret `NFS4_BUILD_KEY`, then use the manual `Build IPA` workflow. For your own copy of the game, run `python -B tools/ci_crypto.py bundle-inputs` to prepare a new encrypted input bundle and hash manifest with your own key.

The macOS job decrypts and checks the inputs, recompiles the game, builds an arm64 unsigned app, and uploads **encrypted** IPA and diagnostic artifacts. Generated source and plaintext game inputs are never uploaded as artifacts. Forks do not receive the owner's secret.

Use `tools/release-ipa.ps1 -Run <run-id>` on the owner's PC to download and decrypt a successful artifact into `dist/ipa/`. The script verifies the app version and build. It creates no public release. By default it archives every version in `dist/ipa/` and the owner archive `E:/ios-ports/ipa/NFS4`, then keeps only the newest NFS4 IPA in iCloud Drive / iOS 포트 IPA and the shared `latest` directory. Other apps are preserved and checked by SHA-256. Use `-SkipICloud` for a local-only download, or `-ArchiveRoot` / `-CloudTarget` for explicit owner paths. Keep the key private; it is not needed on the phone after decryption.

LiveContainer installation and data layout: [install.md](install.md).

## Car texture quality

The exact-build TGA loader at `004EF920` and its power-of-two helper at `004EEBA0` previously limited vehicle textures to 256 pixels. The port reconstructs 256px car/dash RGB at 1024px with separable cubic filtering, preserves alpha classes with nearest sampling, and lifts the internal limit for those uploads. The fixed 256px CPU mirror used for damage/reflections is downsampled separately; unknown existing texture allocations retain their original path. The driver LOD lookup is extended only after its byte signature matches. Higher-resolution replacement art is accepted up to 2048px. No game assets are bundled or modified on disk.

Run `python -B tools/test_car_texture.py` to verify reconstruction, categorical alpha preservation, source immutability, reuse and buffer guards. On the Windows test host, `NFS4_TRACE_TEXTURES=1` logs dimensions (bounded), and `NFS4_ORIGINAL_TEXTURES=1` selects the original path for comparisons. Device quality and loading cost still require an iPhone check.

## Performance and thermal policy

Cubic taps are reused; two bounded, byte-checked mirror caches avoid reconstructing unchanged damage/cockpit images. Equal TMU uploads and palettes retain their decoded GPU textures. The decoded texture cache uses a 64 MiB LRU budget (including mip levels); framebuffer copies have no mip chain. Triangle CPU timing is opt-in with `NFS4_PROFILE=1`.

The original native rendering resolution, 4x MSAA preference and 1024px vehicle quality are retained. The iOS game and presentation use the same frame cap: the selected 60/30fps normally, 30fps for Fair/Serious thermals or Low Power Mode, and 15fps at Critical. A 20-second Nominal recovery avoids rapid toggling. Inactive presentation pauses and the game cap drops to 5fps until reactivation. Power notifications cache state; the game thread reads an atomic cap. Repeated presentation of the same completed GPU frame is skipped.

Validation commands: `python -B tools/test_car_texture.py`, `python -B tools/test_texture_cache.py`, `python -B tools/test_power_policy.py`, and `python -B tools/bench_car_texture.py`. The benchmark reports Windows CPU times and pixel hashes; it does not measure iPhone temperature or battery life. Apple power notification guidance: https://developer.apple.com/documentation/xcode/responding-to-power-notifications

## Follow-up checklist audit

- Menus (including the rotating pause view) now cap both producer and display at 30fps; racing returns to the selected/power-limited cap. The existing exact-build menu observation is shared with touch controls.
- `grGet(0x0E)` is implemented. Unknown-value warnings latch once; they do not use an overflowing 8-bit postincrement. LFB diagnostics stop after eight messages for the entire process, independently of the 300-frame statistics reset.
- Release builds write flushed diagnostic files without duplicating routine messages to NSLog. Fatal/crash headlines still reach the console; `NFS4_CONSOLE_LOG=1` opts into verbose console output. `nfs4host.log` and `nfs4host.segment.log` rotate at 4 MiB each; `nfs4host.prev.log` preserves the preceding launch. Collect all three when diagnosing a long session.
- Frame statistics include average presented frame time every 300 frames; per-triangle CPU timing remains opt-in. `tools/run_host.py` no longer forces file-access tracing; opt in with `NFS4_TRACE_FILES=1`.
- The multimedia timer waits indefinitely on an event with no registered timers and waits until the nearest future deadline otherwise. Timer creation/cancellation wakes it. The AudioUnit mix path is callback-driven; its finite sample loops and mutex do not implement a polling thread. This is a source audit, not proof that every guest code path is free of spin loops.

`python -B tools/test_idle_policy.py` verifies deadline rounding, menu caps, bounded log rotation and previous-launch preservation. The cache regression also verifies that 1000 identical unknown queries emit one warning. Native upscaling remains capped at 3x, memoryless MSAA and three in-flight frames retain the existing semaphore backpressure.
