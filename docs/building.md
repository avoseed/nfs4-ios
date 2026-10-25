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
