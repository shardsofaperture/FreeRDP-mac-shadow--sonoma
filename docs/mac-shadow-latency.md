# macOS shadow production build

Build without installing or launching:

```zsh
python3 scripts/build-macos-shadow-app.py
```

Output: `dist/FreeRDP Shadow.app`. The build uses `Release`, warnings enabled,
the Mac shadow subsystem and existing server audio channels. Testing, profiling,
sanitizers, and benchmark targets are disabled in the production build. The
bundle contains the menu controller, server, required non-system libraries, and
licenses. Libraries use bundle-relative paths. Both executables, nested libraries, and
the bundle are certificate-signed with the required Apple Development identity,
preserving the menu application's existing identifier.

The installer builds and copies this same app to `~/Applications`, then performs
its existing login-item registration and launch. Building alone does neither.

## Build 0.1.8 release

Build 0.1.8 is hardware-accepted with Microsoft Remote Desktop for Mac, Android/aRDP,
and Microsoft RDP 5.2 on Windows 98. It retains the accepted capture scheduler,
latency behavior, audio, resolution policy, security fallback, and input mappings.

The Win98-only clipboard profile uses the historical RDP 5.2 framing dialect: every
clipboard PDU has a trailing zero DWORD that is outside its declared `dataLen`.
This trailer is required for bidirectional Win98 clipboard operation and must remain
on the wire. The receiver suppresses the associated parser warning only after it
recognizes exactly that zero DWORD on the selected Win98 connection; it does not
relax parsing for other clients or unexpected bytes. Modern Mac and Android clients
keep standard framing. INFO logs record the selected client profile and legacy
clipboard selection; high-volume per-key and per-PDU diagnostics are DEBUG-only.

## Build 0.1.7 channel, teardown, and signing revision

The app's short version and bundle version are both `0.1.7`. Its dropdown begins with
**Mac Shadow RDP — Build 0.1.7**, generated from the bundle metadata.

The Mac shadow backend does not consume client microphone data. The generic shadow channel setup
previously created the AUDIN context and opened its worker anyway; that worker later failed its
virtual-channel query. AUDIN now remains absent unless a subsystem supplies an explicit audio-input
callback. Mac system-audio output remains on the independent RDPSND path.

Static channel IDs are negotiated per connection rather than having permanent meanings. In the
observed aFreeRDP channel order, 1006 is the clipboard channel. The server receive path previously
called any negotiated channel without an open application handler "unknown". It now quietly and
diagnostically identifies a joined-but-unopened channel by name, while retaining warnings for truly
unknown or unjoined channel IDs.

The two rejected Android connection attempts are the client's TLS/NLA probing before it retries
with Standard RDP Security. The `/sec:rdp` listener behavior is intentionally unchanged for Windows
98 RDP 5.2 compatibility. The shadow layer no longer adds a redundant error when a rejected probe
ends before activation.

An orderly socket EOF could inherit `EAGAIN` from an earlier BIO operation and was consequently
reported as `ERRCONNECT_CONNECT_TRANSPORT_FAILED`. Orderly EOF is now treated as a normal peer
close, while real read/TLS/network errors retain their error and last-error paths. A genuine
nonblocking `EAGAIN` is treated as no data even if an OpenSSL BIO omits its retry flag.

The roughly ten-second first audio start occurs inside macOS's process-tap/aggregate-device Core
Audio initialization. It was already isolated on the serial audio startup queue; 0.1.7 adds elapsed
time diagnostics without introducing a wait or moving any audio work onto activation, capture,
input, publication, or graphics threads.

Production packaging requires the exact identity
`Apple Development: shardsofaperture (H7V72A5WH6)`. The packager fails before building if that
identity is unavailable, signs the menu executable, server, every nested library, and final bundle,
then rejects any artifact that is ad-hoc, lacks that authority, or has no certificate team. The bundle identifier
remains `io.freerdp.shadow.sonoma.menu`, and signed contents are not modified afterward.

## Build 0.1.5 input and negotiated-packet revision

The app's short version and bundle version are both `0.1.5`. Its dropdown begins
with **Mac Shadow RDP — Build 0.1.5**, generated from the bundle metadata.

Android mouse input now emits one Quartz event for each real RDP button transition. Duplicate
button downs and releases are ignored, move-only packets no longer post a second null mouse event,
and right/middle dragging retains the correct Quartz button. Any held buttons are released when the
last client disconnects or the server stops, preventing state from leaking into a reconnect.

The bounded newest-state scheduler now derives its tile dimensions from each client's negotiated
`MultifragMaxRequestSize`. Every bitmap is sent as a single-rectangle update, with conservative
space reserved for bitmap and fast-path framing, followed by an exact encoded-size check. A change
to the negotiated limit rebuilds the per-client tile state. This keeps 16-bit interleaved and 32-bit
planar output under client-specific limits without hard-coding one client packet size.

The output-buffer failure reported after a bitmap update occurs when FreeRDP's transport flush
returns an I/O error, including a peer disconnect. Under congestion, the scheduler checks the
transport before each tile, submits at most one additional bounded tile when a write first blocks,
then drains or waits while continuing to prioritize input. It therefore does not create an
application graphics FIFO behind FreeRDP's output buffer.

## Build 0.1.4 bounded 32-bit motion revision

The bounded newest-frame bitmap scheduler now covers both 16-bit interleaved and 32-bit planar
sessions. Previously, a 1920x1080 32-bit full-screen update used the standard monolithic encoder:
hundreds of tiles were encoded and fragmented before the client loop could process input or observe
a newer framebuffer, while the buffered transport retained already serialized animation. The
32-bit path now replaces unsent tile state whenever a newer published framebuffer arrives and
submits at most eight operations, 16 KiB, or eight milliseconds between input checks. It stops
submitting immediately when transport output is blocked. Auxiliary message processing is also
bounded per pass and stale queued audio blocks are discarded after congestion.

Display capture now starts before system-audio initialization is queued on a separate serial
queue, so a slow Core Audio tap cannot delay the first video frame or input. Generic and phone
clients use their requested RDP framebuffer dimensions without synchronously changing the
physical Mac display. Physical resolution switching and the missing-size 1024x768 default are
reserved for clients whose hostname identifies the VAIO profile.

The production preset now forces `WITH_VERBOSE_WINPR_ASSERT=OFF`, all debug
instrumentation, profiling, sanitizers, and benchmark/fuzzer options off even
when reusing an older CMake cache. The packager checks the cache and every
compiler command before building: `Release`, `-O3`, `-DNDEBUG`, and no verbose
assertion, debug, sanitizer, or profiling flags. The native menu controller
also uses `-O3 -DNDEBUG -DNS_BLOCK_ASSERTIONS=1`. Normal INFO/WARN/ERROR logging
and failure diagnostics remain available. The audio test tone and comparison
switches remain absent; no server architecture or session behavior changed in
this packaging revision.

Verification for this revision: 384 production compiler commands checked;
bundled `/buildconfig` reports verbose assertions OFF; packaged FreeRDP libraries
have no calls to the verbose assertion handler; all 12 correctness tests pass in
a separate Release build with verbose assertions OFF; an isolated AppKit menu
check using the bundled version metadata confirms the exact dropdown title;
plist, deep signature, and removed-runtime-feature scans pass.

To repeat correctness checks with the production optimization/assertion policy:

```zsh
cmake -S . -B build-macos-shadow-release-checks -G Ninja \
  -C packaging/macos-shadow-menu/production-cache.cmake -DBUILD_TESTING=ON
cmake --build build-macos-shadow-release-checks \
  --target TestSynch TestWinPRUtils TestFreeRDPCodec TestShadowBitmap TestMacShadowPublication TestMacShadowClipboard -j 6
ctest --test-dir build-macos-shadow-release-checks --output-on-failure \
  -R '^TestShadowBitmap$|^TestMacShadow(Publication|Clipboard)$|^TestFreeRDPRegion$|^TestFreeRDPCodec(Color|Copy|Interleaved|Planar)$|^Test(SynchEvent|SynchCritical|SynchThread|MessageQueue|MessagePipe)$'
```

## Runtime behavior

- Input is serviced before graphics. Capture continues into a private latest
  framebuffer while the publication worker waits for consumers of its immutable
  snapshot. No captured-frame FIFO accumulates.
- Eligible Mac 16-bit and 32-bit bitmap sessions automatically use sparse tiles,
  newest-image replacement, bounded submissions, and output backpressure. The 16-bit path uses
  RGB565/interleaved compression and the 32-bit path uses planar compression.
  Verified screen copies require negotiated ScrBlt and fast-path output;
  otherwise tiles use classic bitmap compression.
- Other depths, graphics pipelines, surface commands, lobby views, and shared
  subrectangles use the standard encoder. Negotiated update limits that cannot safely hold even
  the minimum bitmap tile also use the standard encoder.
  Cache allocation/conversion failure switches to that encoder and forces a
  complete refresh, including damage still pending in the discarded cache.
- The Mac server advertises 16-bit fallback for indexed-color and 24-bit requests
  before capability exchange. Native 15-bit and 32-bit paths remain available.
  Both the standard and optimized bitmap paths pad edge rectangles without
  reading outside the source or expanding the destination rectangle.
- The client's complete requested resolution takes precedence. A hostname
  containing `VAIO` defaults to 1024×768 only if a complete size is absent; RDP
  provides no reliable hardware-model identifier. Explicit VAIO sizes are kept.
- Generic/mobile sessions use the requested session size through a scaled capture surface and do
  not block activation on physical mode switching. The VAIO profile retains physical mode selection
  but makes at most one private Sonoma mode attempt before falling back to a scaled surface. Aspect
  ratio and existing mouse-coordinate mapping are retained. Requests are bounded to 200–8192 pixels
  per axis and at most 16,777,216 pixels total.
- Saved display modes survive failed rollback attempts. A subsequent connection
  retries pending restoration before saving or switching another mode. Normal
  disconnect and shutdown retain the existing restoration paths.
- The app retains `127.0.0.1:3390`, SSH operation, system audio, automatic process
  recovery, connection-scoped capture/audio, and the existing permission UI.
  Its automatic profile does not inherit external display-command overrides.

Removed runtime features: latency/comparison switches, comparison launcher,
per-publication timing counters/logging, and generated audio test tone. Regression
tests are separate build targets and are not shipped in the app. The stock CLI
and FreeRDP transport/security defaults remain available.

Submission budgets are eight operations, 16 KiB, and an eight-millisecond deadline
checked between operations. They bound application scheduling, not end-to-end
latency through SSH, the network, or client decoding/display queues.

The September 22, 2026 reliability work adds two inner scheduling bounds. Warm
cache copy search checks candidates from the tile outward and stops at 16,384
candidate probes; exact pixel verification and overlap-safe ordering are kept,
and an exhausted search falls back to a bitmap update. Shadow virtual-channel
output is serviced in ordered slices of at most 32 queue messages, 32 KiB, or
2 ms. The generic FreeRDP channel-manager API remains unchanged for unrelated
servers.

Clipboard text requests have a five-second response deadline and a one-response
quarantine for late data because the cliprdr response has no request ID. Payloads
over 8 MiB are rejected before decoding. A missing response can therefore leave
clipboard transfer degraded for the connection, but it does not trigger a blind
replacement request or require disconnecting to restore graphics/input. This is
a protocol-safety limit, not a claim that AppKit pasteboard calls are
cancellable.

## Validation on September 8, 2026

- Warnings-enabled Release server and native app build on Intel macOS Sonoma.
- Twelve tests: events, critical sections, threads, message queue, message pipe,
  regions, color, image copy, interleaved codec, planar codec, shadow bitmap replay,
  and Mac publication/lifecycle.
- Replay covers all requested sizes: 800×600, 1024×768, 1280×720, 1280×800,
  1440×900, 1680×1050, 1920×1080, 2560×1600, and 2880×1800; also odd dimensions,
  corner pixels, refresh, partial replacement, quantization, scrolling, and
  overlapping copies. Capability checks cover 8/15/16/24/32-bit requests and
  unsupported graphics capabilities. Padded edge codec round trips cover
  15/16/24-bit interleaved and 32-bit planar data.
- The publication test stalls a subscriber, verifies immutable publication and
  pending damage, exercises 25 synthetic worker cycles, verifies VAIO request
  precedence, and injects mode-API unavailability to check restoration ownership
  across a failed reconnect.
- AddressSanitizer and UndefinedBehaviorSanitizer pass with the bitmap helper and
  replay tests instrumented. Dependencies are not rebuilt with sanitizers.
- App plist/signature validation and bundled-server loading checks pass. No test
  executables or removed runtime switches are packaged. Existing upstream
  deprecated-API and unused-code compiler warnings remain.

Run the separate test build with:

```zsh
cmake --build build-sonoma-shadow-p0-channels \
  --target freerdp-shadow-cli TestShadowBitmap TestMacShadowPublication TestFreeRDPCodec -j 6
ctest --test-dir build-sonoma-shadow-p0-channels --output-on-failure \
  -R '^TestShadowBitmap$|^TestMacShadowPublication$|^TestFreeRDPRegion$|^TestFreeRDPCodec(Color|Copy|Interleaved|Planar)$|^Test(SynchEvent|SynchCritical|SynchThread|MessageQueue|MessagePipe)$'
```

This is a production-configured build ready for real hardware testing, not a
claim of completed hardware qualification. Actual display-mode availability,
Screen Recording/Accessibility grants, first frame, idle/resume, audio playback,
sleep/wake, repeated reconnects, Win98 screen copies/input, display restoration,
and clean shutdown still need the physical Mac/client matrix. Record latency
and bandwidth separately for idle, typing, window movement, scrolling, and
full-screen motion; no new end-to-end latency measurements are claimed here.

## Focused reliability validation — September 22, 2026

The earlier Release-checks directory could not launch its tests because it
pointed at a missing machine-specific Jansson library and stale OpenSSL 3.6.3
paths. The production directory was subsequently regenerated against OpenSSL
3.6.4 with json-c available. A complete signed app was built at
`dist/FreeRDP Shadow.app`; deep/strict signing verification passed on the
physical Mac with the required Apple Development identity. A sandboxed repeat
of `codesign --verify --deep --strict` returned `CSSMERR_TP_NOT_TRUSTED`, so
that environment does not independently establish the host trust result.

An isolated Release validation build used OpenSSL 3.6.4 and disabled JSON. It
did not install or modify the active app.

The new `TestShadowBitmap` and `TestMacShadowClipboard` binaries passed. The
targeted 13-test CTest set in that isolated build had 12 passes, including
`TestMacShadowPublication` and all selected WinPR/codec tests; only the existing
`TestFreeRDPCodecInterleaved` case exited 255 without diagnostic output. An
isolated build of clean commit `8ded81d898ca2c8b5432fec73f2d00a68c3bd6eb`
with JSON disabled reproduced the same exit. The test's final fixture phase
loads `interleaved/encoder.json`; WinPR's JSON stub always returns null when
JSON is disabled. The current tree's interleaved test passed in a separate
JSON-enabled Release build using json-c; the complete targeted set passed
13/13 there. This classifies the 255 exit as a test-build configuration
failure, not a codec or working-tree regression.

An isolated Release diagnostic called `shadow_bitmap_next()` on an established
1920×1080 32-bit cache with an unrelated new frame, forcing copy-search misses.
Across 100 calls on this host, median was 4.761 ms, p95 was 5.005 ms, and
maximum was 5.605 ms. The 1024×768 16-bit case measured 4.617 ms median,
4.886 ms p95, and 5.438 ms maximum. This temporary test harness was kept
outside the repository; these are local CPU timings, not network latency.
One `shadow_bitmap_next()` can perform three directional probes plus the chosen
tile's search, each capped at 16,384 candidates. The outer 8 ms flush deadline
is checked between calls and cannot interrupt one. No measured call exceeded
8 ms here, so an internal deadline remains a measured follow-up for slower or
loaded hardware, not an established source fix. A channel service slice also
allows its first fragment regardless of byte budget and cannot interrupt an
individual `SendChannelData` call; its 2 ms target is therefore cooperative.
Bitmap staging, per-tile compression, and output-buffer draining likewise run
to completion inside a loop iteration, so the 8 ms value is a scheduling
target rather than a hard wall-clock limit. Resetting `bitmapFallback` only
changes eligibility at an encoder generation boundary and adds no inner loop.
The clipboard's five-second request timer and late-response quarantine run on
its serial queue; they do not themselves impose a five-second wait on graphics
or input. Physical aged-session timings and queue-age measurements are still
needed to determine whether any of these operations needs a tighter bound.

The next candidate target is 0.1.9; the checkout remains 0.1.8. Physical
Android, Windows 98, and aged-session validation of this candidate remains
outstanding. No hardware latency or new paste result is claimed by these tests.
