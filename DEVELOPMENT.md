# Mac Shadow RDP development

This focused FreeRDP fork shadows a macOS Sonoma desktop for legacy RDP clients.
**Mac Shadow RDP Build 0.1.7** is the known-good production baseline
(`mac-shadow-rdp-0.1.7` at `59386e731`); its tag is an immutable recovery point.
This repository-maintenance work does not change the application version.

The primary target is Microsoft Remote Desktop 5.2 on a Windows 98 VAIO. Android
/ aFreeRDP and other RDP clients are secondary clients. The server is intended to
listen only on `127.0.0.1:3390`; remote use is through an SSH tunnel, never a direct
LAN or Internet listener.

## Build and package

The authoritative production workflow is:

```zsh
python3 scripts/build-macos-shadow-app.py
```

It configures the Release preset, builds `freerdp-shadow-cli`, assembles
`dist/FreeRDP Shadow.app`, and verifies nested signatures. The app is a generated
local product and is intentionally not tracked. Installation uses the same build via
`./scripts/install-macos-shadow-menu.sh`.

Signing is mandatory: `Apple Development: shardsofaperture (H7V72A5WH6)` with bundle
ID `io.freerdp.shadow.sonoma.menu`. Do not substitute ad-hoc signing.

For targeted regression tests, use the separate test build:

```zsh
cmake -S . -B build-macos-shadow-release-checks -G Ninja \
  -C packaging/macos-shadow-menu/production-cache.cmake -DBUILD_TESTING=ON
cmake --build build-macos-shadow-release-checks --target \
  TestSynch TestWinPRUtils TestFreeRDPCodec TestShadowBitmap TestMacShadowPublication -j 6
ctest --test-dir build-macos-shadow-release-checks --output-on-failure \
  -R '^TestShadowBitmap$|^TestMacShadowPublication$|^TestFreeRDPRegion$|^TestFreeRDPCodec(Color|Copy|Interleaved|Planar)$|^Test(SynchEvent|SynchCritical|SynchThread|MessageQueue|MessagePipe)$'
```

## Custom code map

| Area | Primary location |
| --- | --- |
| macOS capture, display changes/restoration, client profiles, input, lifecycle | `server/shadow/Mac/mac_shadow.c`, `mac_shadow.h` |
| macOS system-audio capture | `server/shadow/Mac/mac_shadow_audio.m` |
| newest-state/sparse damage/ScrBlt scheduling and backpressure | `server/shadow/shadow_client.c`, `shadow_bitmap.c`, `shadow_encoder.c` |
| channel handling | `server/shadow/shadow_channels.c` |
| legacy transport behavior | `libfreerdp/core/transport.c`, `libfreerdp/core/info.c`, `libfreerdp/core/server.c` |
| menu app, packaging, signing | `packaging/macos-shadow-menu/`, `scripts/build-macos-shadow-app.py` |
| deterministic custom regressions | `server/shadow/test/TestShadowBitmap.c`, `TestMacShadowPublication.c` |

The Mac backend gives input priority and favors newest desktop state over animation
smoothness. Brief self-correcting visual roughness is acceptable; persistent
corruption is not. Preserve 16-bit interleaved and 32-bit planar paths, negotiated
packet sizing, sparse damage/ScrBlt, bounded output backpressure, Android button
compatibility, automatic resolution (including VAIO behavior), asynchronous system
audio, and normal FreeRDP channels/transport.

## Hardware smoke test

On Sonoma, grant **Screen & System Audio Recording** and **Accessibility** to the
signed app, then connect through loopback SSH. Confirm the first frame, input, audio
(when supported), disconnect/reconnect, and display restoration. For latency
regression, run a high-motion screensaver locally, connect at target depth/resolution
(Win98: 1024x768, 16-bit), and separately note idle, typing, window movement,
scrolling, and full-screen-motion responsiveness. See `docs/mac-shadow-latency.md`
for detailed behavior and test notes.
