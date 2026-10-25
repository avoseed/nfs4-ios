# NFS4 iOS port

Personal arm64 static recompilation project with a Windows development host, Metal presentation, audio, touch controls, and gamepad support.

Current validation: native PC menu, track loading, analog acceleration, pause, and 1360x768 race output. The unsigned arm64 iOS package 0.1.0 build 1 passed the [macOS workflow](https://github.com/avoseed/nfs4-ios/actions/runs/40205720470) and local package validation. An entire race and physical iOS execution have not yet been verified.

The repository publishes port source. Original inputs and CI app packages use owner-controlled authenticated encryption. Game assets, raw inputs, generated C, keys, and local records are excluded from Git.

- Version: `APP_VERSION`; build number: Actions run number.
- Bundle ID: `io.github.avoseed.nfs4`.
- Data folder: `NFS4`, with `game/`, `save/`, and `mods/`.
- [Build instructions](docs/building.md)
- [LiveContainer installation](docs/install.md)
- [Control evidence](docs/controls.md)
- [Static import support](docs/imports.md)

This is an early development build. Building or running requires the matching owner-provided files.
