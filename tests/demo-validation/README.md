# DOS Blood 1.21 regression demos

This ports NotBlood's deterministic demo validation into NBlood. The 144
recordings and expected results come from NotBlood commit
`4aa93cb12194a8864ff467a139cc2ca38595a1ff`; see [SOURCE.md](SOURCE.md) for fixture
provenance. The baseline values are unchanged.

## Run the suite

Build NBlood using the repository's normal Linux dependencies, then run:

```sh
make -j4 blood
python3 tests/demo-validation/run.py --data-dir /path/to/blood-1.21
```

The data directory must contain the original Blood 1.21 files listed in the
root README. They are not included with the test fixtures. In the prepared
cloud environment, first source `/workspace/netblood-env/activate.sh` and use
`--data-dir /workspace/netblood-game-data/blood121`.

The runner uses SDL's dummy video/audio drivers and an isolated 640x480
software-rendering configuration. Each invocation creates a fresh temporary
directory containing `console.log`, `nblood.log`, and `result.json`; its path
is printed at startup. It does not read or overwrite your normal game settings.
It requires a zero exit status, the final success message, and distinct
per-demo pass records matching the summary. A timeout or incomplete run fails.
Use `--timeout SECONDS`, `--binary PATH`, or `--output-dir DIRECTORY` to change
those defaults. `--archive PATH` selects an alternate validation fixture archive.
CI additionally passes `--require-all`: every recording from `TEST000.DEM`
through `TEST143.DEM` must have a distinct pass record, and the summary must
report exactly 144 passes and zero skips. Use GCC 14 or earlier with LTO enabled
for that strict gate. A compiler exclusion fails CI instead of reducing coverage.

To invoke the built-in mode directly from the checkout, with a usable display:

```sh
./nblood -validate -usecwd -j=/path/to/blood-1.21 -g=tests/demo-validation/validation.pk3
```

Use `-j=/absolute/path` and `-g=/absolute/path` for absolute paths: the legacy
option parser otherwise treats a leading slash as an option. The validation
mode selects `TEST.INI`, suppresses autoload/setup/intro screens, and cannot
be combined with explicit map selection, recording, playback, or multiplayer.

## What is checked

`source/blood/src/demo_validate.h` holds the expected input count, final random
seed, player coordinates, player health, and auto-aim setting for every demo.
`CDemo::LoadDemoInfo()` verifies suite completeness and demo headers, while
`CDemo::Playback()` feeds recorded inputs through the normal game simulation
at accelerated speed and checks the final state. A mismatch reports the demo
and expected/actual values, then exits unsuccessfully. Successful completion
prints `1.21 Validation Successful! (N passed, M skipped)`.

Each case starts with cleared sprite collision state so that DOS playback's
stale state from the previous demo cannot affect the next one. Sprite allocation
and gameplay logic within each recording retain their normal behavior. Checks
use the upstream checkpoint, just before processing the final recorded input.

The upstream compiler exclusions are retained and reported explicitly:

- Without link-time optimization: `TEST038.DEM`.
- With GCC 15 or later: `TEST085.DEM`, `TEST096.DEM`, and `TEST098.DEM`.

Missing or unknown demos and malformed input data fail validation. Compiler
exclusions are separate from passes; the complete manifest still has 144
entries. New regressions should be investigated without changing baselines or
adding exclusions merely to make the run pass.

These checks cover deterministic game behavior. They do not compare rendered
images, measure performance, or validate physical audio/video devices. CI
needs separately supplied Blood 1.21 assets before it can run this suite.
