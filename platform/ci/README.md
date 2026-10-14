# Validation and packages

The GitHub Actions **Build** workflow runs automatically for pull requests and
published releases (including prereleases). Ordinary branch/tag pushes and merge
queues do not start it. Updating a branch with an open PR still runs its PR checks.
An explicit manual dispatch is available for testing and packaging without a
release. All Windows binaries are cross-compiled on Ubuntu; no job uses a Windows
runner or Windows container.
The obsolete Windows-hosted AppVeyor configuration has been removed; GitHub
Actions is the maintained validation and packaging pipeline.

## Mandatory test gate

The first job builds NetBlood with GCC 13 and LTO, then runs the complete legacy
Blood 1.21 demo corpus. It requires the identities of all **144** checked-in demos,
144 passes, and **zero skips**. A crash, timeout, missing asset, duplicate result,
desync, or compiler exclusion fails the job. It then runs the standalone network
regressions with ASan/UBSan, URI registration checks, and pipeline integrity tests.
Headless demo validation disables frame limiting so a missing display refresh
rate cannot stall the event loop; simulation and checksum checks are unchanged.

Only after that succeeds do the integration shards run:

- Network transport, prediction, content verification and map downloads, version
  negotiation, names and passwords, server rules, votes, teams, chat, lives,
  cinematics, scoreboard, intermission, URI/Discord joins, and STUN.
- Vulkan backend and validation-layer checks, Polymost/Vulkan comparisons,
  rendering fixtures and room-over-room maps, renderer switching and lifecycle,
  operation without OpenGL, ambient occlusion, and save previews.
- Reproducible shader compilation against the checked-in SPIR-V.
- Windows URI registration and Discord named-pipe tests, cross-compiled and run
  under Wine on Linux in a disposable prefix. Wine's explicit registry-class
  mirror checks dispatch, not native Windows' per-user association lookup.

Polymer is not exercised. Every packaging job depends on **all** the regression
shards. There are no allow-failure or missing-data skip paths. Test logs and JSON
reports are retained for 14 days, including on failure. The exact executable from
the first job is used by integration tests.

`platform/ci/validate.py --help` lists individual shards. For example:

```sh
python3 platform/ci/validate.py --suite demos --binary ./nblood \
  --data-dir /path/to/blood121 --output /tmp/netblood-demo-results
python3 platform/ci/validate.py --suite unit --output /tmp/netblood-unit-results
python3 platform/ci/validate.py --suite render-ror --binary ./nblood \
  --data-dir /path/to/blood121 --output /tmp/netblood-ror-results
```

Output directories must be new. Graphical shards create their own Xvfb display
and use Mesa lavapipe, avoiding a dependency on the runner's physical GPU. Install
the packages listed in `.github/actions/validation-environment/action.yml` to run
them locally. The shader compiler is separately pinned:

```sh
bash platform/ci/validate-shaders.sh /tmp/netblood-shader-tools \
  --output /tmp/netblood-shader-results
```

### Game data

`fetch-test-assets.py` retrieves the same Blood 1.21 disc used for the development
environment from Internet Archive. It verifies the full disc SHA256, builds a
checksum-pinned extractor, and verifies all 50 extracted test assets against the
checked-in manifest. The demo fixture archive itself remains under
`tests/demo-validation/` and is exercised in full by the demo runner.

```sh
python3 platform/ci/fetch-test-assets.py \
  --output-dir /path/to/blood121 --cache-dir /path/to/download-cache
```

CI caches the extracted data and checks its hashes on every use. A repository
variable `NETBLOOD_TEST_DISC_URL` may point to a mirror of the **same disc image**;
the expected checksums cannot be overridden. Download failures fail validation.
No credentials are needed for the default source.

Retail game files are excluded from packages and test-report artifacts. Some
runtime tests copy game files into their evidence directories, so never upload
those directories wholesale. The workflow uses an explicit log/JSON allowlist.

## Package matrix

Every successful build produces these 13 artifacts, with the source revision
recorded inside each package. Users must supply their Blood game data separately.

| Artifact | Target and baseline | Build host |
| --- | --- | --- |
| `netblood-windows-x86_64.zip` | Windows 64-bit | Ubuntu, MinGW GCC |
| `netblood-windows-i686.zip` | Windows 32-bit | Ubuntu, MinGW GCC |
| `netblood-windows-xp-i686-nosse2.zip` | Windows XP 32-bit, Pentium III, no SSE2 | Ubuntu, MinGW GCC/MSVCRT |
| `netblood-windows-arm64.zip` | Windows 11 ARM64 | Ubuntu, pinned LLVM-MinGW/UCRT |
| `netblood-debian12-amd64.deb` | Debian 12 amd64, x86-64 baseline | Debian 12 container |
| `netblood-debian12-i386.deb` | Debian 12 i386, SSE2 | Debian 12 container |
| `netblood-debian12-i386-nosse2.deb` | Debian 12 i386, i686/x87, no SSE2 | Debian 12 container |
| `netblood-debian12-armhf.deb` | Debian 12 armv7, VFPv3-D16 hard float | Debian 12 container/QEMU |
| `netblood-debian12-arm64.deb` | Debian 12 arm64, ARMv8-A | Debian 12 container/QEMU |
| `netblood-linux-x86_64.AppImage` | Linux x86-64, glibc 2.36 or later | Debian 12 container |
| `netblood-macos-universal.zip` | macOS 11+, Intel and Apple Silicon | macOS 14 |
| `netblood-freebsd-amd64.tar.xz` | FreeBSD 14.5+, amd64 | FreeBSD VM on Ubuntu |
| `netblood-netbsd-i386-nosse2.tar.xz` | NetBSD 10.1, i386/Pentium III, no SSE2 | NetBSD amd64 VM, i386 cross compiler |

The no-SSE2 packages provide software rendering and Polymost; Vulkan is disabled
for these legacy CPU targets. XP networking is IPv4-only because XP cannot create
the dual-stack sockets used on newer systems. Unsupported IPv6 joins show an
error. The macOS package provides software, Polymost OpenGL, and native Metal
rendering. Metal uses Apple's system framework and requires a Metal-capable GPU;
it does not use or bundle MoltenVK. Other modern targets retain Vulkan support when an
appropriate system Vulkan runtime and GPU driver are installed.

Windows dependencies are built from pinned source archives for each target.
The XP build disables the newer allocator and optimized dependency routines,
sets the Windows 5.1 subsystem, and checks PE imports and instruction usage.
Its audit rejects SSE2 and newer vector instructions throughout the executable.
The only newer scalar instructions allowed are libstdc++'s identified RDRAND and
RDSEED entropy helpers, after verifying their CPUID feature checks and conditional
selection. The package records those checks before stripping audit symbols.
The Windows 11 ARM64 package contains native ARM64 code, not an x86 emulator.

Debian packages declare their runtime dependencies. Debian **12** is intentional:
Debian 13's i386 CPU baseline requires SSE2. The no-SSE2 package also checks its
startup under QEMU's Pentium III CPU model. AppImage bundles its application
libraries; graphics drivers and the Vulkan runtime come from the host. Its URI
and Discord handlers refer to the persistent AppImage file, not a temporary mount.

The macOS executable and bundled libraries are checked for both architectures.
`USE_METAL=1` is the default for macOS SDL2 builds; `USE_METAL=0` disables it.
Metal is disabled automatically on other platforms, SDL1, and software-only builds.
Select **Metal** in the startup window or graphics menu, or use `setrendermode 6`
in the game console. The selection is saved between launches.
Its Objective-C++ backend uses ARC independently of the existing Cocoa sources.
Before packaging, the macOS job compiles the Metal shader with Apple's compiler
and builds the backend tests against the same SDL2 dependency as the game. The
tests exercise rendering when the runner exposes a usable Metal device/display.
Hosted runners without one record an explicit runtime skip in the uploaded Metal
report; shader/backend compilation still has to pass. On a Mac with a GPU, run
`tests/metal/backend-test.sh --require-device --output test-results/metal` to
require the rendering checks as well. A hosted-runner skip does not establish
visual parity or GPU runtime compatibility.
The app is ad-hoc signed; notarization and a Developer ID are not configured.
FreeBSD packages include runtime installation instructions. NetBSD uses official
10.1 i386 release sets and a NetBSD cross toolchain, rebuilding its multimedia
libraries without SSE2. It audits the executable and bundled SDL2 instructions,
checks the ELF32 machine type, and runs the packaged executable through NetBSD's
32-bit compatibility support.

## Building packages locally

The package scripts are also usable outside Actions. Set source metadata when
building from an exported checkout:

```sh
export NETBLOOD_REVISION="$(git rev-parse HEAD)"
export SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
```

Windows (Linux host prerequisites are in the workflow):

```sh
python3 platform/ci/windows-build.py --target windows-xp-i686-nosse2 \
  --work-dir /tmp/netblood-windows --output dist --jobs 2
```

Debian/AppImage (install Docker Buildx and QEMU/binfmt for foreign architectures):

```sh
docker buildx build --platform linux/arm/v7 --file platform/ci/linux.Dockerfile \
  --build-arg PACKAGE_KIND=deb --build-arg CPU_BASELINE=standard \
  --build-arg NETBLOOD_REVISION --build-arg SOURCE_DATE_EPOCH \
  --build-arg BUILD_JOBS=2 --target artifact --output type=local,dest=dist .
```

Use `linux/386` and `CPU_BASELINE=nosse2` for old x86; use `linux/amd64` and
`PACKAGE_KIND=appimage` for AppImage. A managed environment's extra HTTPS CA can
be passed with `--secret id=proxy_ca,src=/path/to/combined-ca-bundle.pem`.

Run `bash platform/ci/macos.sh` on macOS with Xcode, CMake, GNU Make, pkg-config,
and Python. Run `bsd-freebsd.sh` or `bsd-netbsd.sh` inside the corresponding VM
with the prerequisite packages listed in the workflow. These scripts fail when
used on the wrong host rather than producing mislabeled binaries.

## Publication

Publish a release in GitHub to start validation and packaging for its tag. The
release exists before CI finishes; its download assets appear only after every
test and package gate succeeds. Draft releases do not trigger builds. CI never
creates releases or tags automatically. Release events created using another
workflow's `GITHUB_TOKEN` do not trigger Actions; publish through the GitHub UI,
a personal token, or a GitHub App token instead.

PR and manual runs provide downloadable Actions artifacts and cannot publish.
Only a successful **published-release** run calls the reusable Publish workflow,
after the complete-package gate. Write permission is limited to that release job.
It checks out the exact tested commit and downloads artifacts from the caller's
exact workflow run ID; it never selects an unrelated successful build.

Both workflows verify that all 13 expected packages exist. Publication also
checks that the original release remains published and that its tag still points
to the tested commit. A failed/cancelled validation or package job prevents asset
upload. `SHA256SUMS` covers the complete set. Uploads never replace existing assets;
a rerun encountering the same filenames fails instead of silently overwriting.

The build recipes and smoke checks run in CI on their named targets. Local syntax
checks or a successful Linux build do not establish that macOS, BSD, ARM, or XP
runtime compatibility has been tested; inspect the corresponding job reports.
