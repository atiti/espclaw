# Contributing To ESPClaw

Thanks for contributing. ESPClaw is an embedded-first project, so the bar is boring, inspectable, and reversible changes that keep the firmware stable on real boards.

## Before You Start

- Read the [README](README.md) for the current product shape.
- Check the [support matrix](docs/support-matrix.md) before proposing new board or channel work.
- Open an issue or discussion first for large product, architecture, or runtime changes.

## Development Setup

Host build and tests:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Simulator:

```bash
./build/espclaw_simulator --workspace /tmp/espclaw-dev --port 8080 --profile esp32s3
```

Firmware toolchain:

```bash
mkdir -p .deps
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git .deps/esp-idf-v5.5.2
IDF_TOOLS_PATH=$PWD/.deps/.espressif ./.deps/esp-idf-v5.5.2/install.sh esp32s3,esp32
source scripts/use-idf.sh
```

Firmware builds:

```bash
cd firmware
idf.py -B build-esp32s3 set-target esp32s3
idf.py -B build-esp32s3 build
idf.py -B build-esp32 set-target esp32
idf.py -B build-esp32 build
```

## Pull Request Expectations

- Keep changes scoped.
- Add or update tests for every code change.
- Run the relevant tests before opening a PR.
- Update user-facing docs in `docs/` when behavior changes.
- Update [CHANGELOG.md](CHANGELOG.md) for notable changes.
- Call out board-specific limitations and migration risks in the PR description.

## Commit Format

Use:

```text
<type>(<scope>): <summary>
```

Examples:

- `feat(runtime): add component manifests`
- `fix(telegram): reduce esp32cam camera DMA pressure`
- `ci(release): publish firmware artifacts on tags`

Allowed `type` values:

- `feat`
- `fix`
- `docs`
- `test`
- `chore`
- `refactor`
- `perf`
- `ci`

## Testing Guidance

Minimum expectation for most changes:

- host build
- `ctest`

Also run when relevant:

- `tests/simulator_api_test.sh`
- `tests/test_real_device_bench.py`
- firmware builds for `esp32` and `esp32s3`
- live-board validation when changing runtime, storage, OTA, camera, Telegram, or serial console flows

## Documentation Style

- Prefer operational docs over marketing prose.
- Keep examples copy-pastable.
- Be explicit about what is stable, experimental, or board-specific.

## Security

Do not open public issues with secrets, auth tokens, account cookies, or private device identifiers. Use the process in [SECURITY.md](SECURITY.md) instead.
