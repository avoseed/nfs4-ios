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

## PC에서 받기 서버

휴대폰 전송은 `E:/ios-ports/PC에서-받기-서버.cmd` 공용 서버 하나만 사용합니다. 기존 프로젝트 실행기(.cmd)는 폐기 안내용입니다. `tools/serve-games.py` 복사본은 PC 안 시험에만 사용하며 `--port 18765 --host 127.0.0.1` 이외의 실행 인수를 거부합니다. 등록·목록 변경은 작업 폴더의 `docs/공용폴더-반영요청.md`로 요청하고, 공용 설정 반영과 서버 재시작은 사용자가 합니다. PC 전용 시험 복사본은 `127.0.0.1:18765`로만 사용합니다.
