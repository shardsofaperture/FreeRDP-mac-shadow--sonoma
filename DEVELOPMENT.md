# Development and release workflow

Mac Shadow RDP 1.0.0 is the published production configuration. Its signed
bundle uses fixed 256000 B/s graphics pacing, 50 ms publication aggregation,
ordinary/F bitmap scheduling, system-default `SO_SNDBUF`, and no adaptive
probing, burst, or coverage scheduler. The installed listener is
`127.0.0.1:3390` behind SSH. See the [architecture guide](docs/mac-shadow-architecture.md)
and [engineering history](docs/mac-shadow-engineering-history.md).

## Production build and package

Build and sign the app on the target Mac:

```zsh
python3 scripts/build-macos-shadow-app.py
```

The builder configures Release, builds `freerdp-shadow-cli`, packages
`dist/FreeRDP Shadow 1.0.0.app`, and verifies nested signatures. Installation
uses the same generated app:

```zsh
./scripts/install-macos-shadow-menu.sh
```

The production signing identity is
`the project Apple Development signing identity configured in Keychain` and the bundle identifier
is `io.freerdp.shadow.sonoma.menu`. Never use ad-hoc signing or modify the
bundle after signing. Build provenance and the member SHA-256 manifest are
written beside the generated app. The app uses macOS SMAppService Launch at
Login and starts its child server on `127.0.0.1:3390` only.

## Clean Release build

Use an isolated build directory when validating a change. This builds and
signs a temporary package without replacing the installed application or the
retained bundle in `dist/`:

```zsh
build_dir=/private/tmp/mac-shadow-1.0.0-release
python3 scripts/build-macos-shadow-app.py \
  --build-dir "$build_dir" \
  --output "/private/tmp/FreeRDP Shadow 1.0.0.app"
```

The target Mac needs the Apple signing identity, Xcode command-line tools,
CMake, Ninja, and dependencies selected by
`packaging/macos-shadow-menu/production-cache.cmake`.

## Regression tests

Configure a separate Release/json-c test build, build the relevant targets,
and run the regression filter:

```zsh
test_dir=/private/tmp/mac-shadow-1.0.0-tests
cmake -S . -B "$test_dir" -G Ninja \
  -C packaging/macos-shadow-menu/production-cache.cmake \
  -DBUILD_TESTING=ON -DWITH_JSONC_REQUIRED=ON
cmake --build "$test_dir" --target \
  TestSynch TestWinPRUtils TestFreeRDPCodec TestShadowBitmap \
  TestShadowPacer TestShadowDiagnosticM TestShadowPublicationPacing \
  TestShadowLatencyWorkload TestShadowAggregationWorkload \
  TestShadowCoverage TestShadowSocketCap TestMacShadowPublication \
  TestMacShadowClipboard -j 6
ctest --test-dir "$test_dir" --output-on-failure \
  -R '^TestShadow(Bitmap|Pacer|DiagnosticM|PublicationPacing|LatencyWorkload|AggregationWorkload|Coverage|SocketCap)$|^TestMacShadow(Publication|Clipboard)$|^TestFreeRDPRegion$|^TestFreeRDPCodec(Color|Copy|Interleaved|Planar)$|^Test(SynchEvent|SynchCritical|SynchThread|MessageQueue|MessagePipe)$'
```

The custom tests cover publication and aggregation, bitmap reconstruction,
pacing, socket behavior, clipboard, and Mac input/publication paths. Do not
count skipped or unbuilt tests as passes. See [the architecture guide](docs/mac-shadow-architecture.md)
for code locations and [the release record](docs/mac-shadow-1.0.0.md) for
hardware acceptance and validation limits.

## Recovery

Use a separate worktree at the desired recovery tag; build and sign with the
configured identity before installing. The 0.2.0 source release and 0.1.9
Recovery bundle are retained for recovery. See the [engineering history](docs/mac-shadow-engineering-history.md)
for the selected rate and retained conclusions.

## Privacy scan

Before publishing repository changes, run `python3 scripts/check-privacy.py --base origin/master`. It scans added and changed content for private infrastructure data. A full-history scrub requires separate history validation.
