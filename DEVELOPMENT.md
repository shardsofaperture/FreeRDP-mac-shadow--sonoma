# Mac Shadow RDP development

This focused FreeRDP fork shadows a macOS Sonoma desktop for legacy RDP clients.
The checkout remains **Mac Shadow RDP Build 0.1.8**. The next candidate target is
**0.1.9**, pending the remaining physical validation; no version has been bumped.
Build 0.1.7 is the known-good production baseline
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
On the target Mac, the signing identity is available. The regenerated
`build-macos-shadow-production` cache resolves OpenSSL 3.6.4 and json-c; a
complete `dist/FreeRDP Shadow.app` was built and passed deep/strict signing
verification on that Mac. Historical Jansson lookup warnings are not a current
production-build blocker. This build has not completed the Android, Windows 98,
or aged-session hardware acceptance for the next candidate.

For targeted regression tests, use the separate test build:

```zsh
cmake -S . -B build-macos-shadow-release-checks -G Ninja \
  -C packaging/macos-shadow-menu/production-cache.cmake -DBUILD_TESTING=ON \
  -DWITH_JSONC_REQUIRED=ON
cmake --build build-macos-shadow-release-checks --target \
  TestSynch TestWinPRUtils TestFreeRDPCodec TestShadowBitmap TestMacShadowPublication TestMacShadowClipboard -j 6
ctest --test-dir build-macos-shadow-release-checks --output-on-failure \
  -R '^TestShadowBitmap$|^TestMacShadow(Publication|Clipboard)$|^TestFreeRDPRegion$|^TestFreeRDPCodec(Color|Copy|Interleaved|Planar)$|^Test(SynchEvent|SynchCritical|SynchThread|MessageQueue|MessagePipe)$'
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
| deterministic custom regressions | `server/shadow/test/TestShadowBitmap.c`, `TestMacShadowPublication.c`, `TestMacShadowClipboard.m` |

The Mac backend gives input priority and favors newest desktop state over animation
smoothness. Brief self-correcting visual roughness is acceptable; persistent
corruption is not. Preserve 16-bit interleaved and 32-bit planar paths, negotiated
packet sizing, sparse damage/ScrBlt, bounded output backpressure, Android button
compatibility, automatic resolution (including VAIO behavior), asynchronous system
audio, and normal FreeRDP channels/transport.

Build 0.1.8 adds plain-text-only `cliprdr`: Unicode text is preferred, with
legacy ANSI/OEM text accepted from clients. Unsupported clipboard clients remain
normal sessions. Client format-data requests now have a five-second deadline,
one-response late-data quarantine, and an 8 MiB payload limit; a quarantined
request can leave clipboard transfer degraded without stopping input or graphics.
Per-key and clipboard-PDU tracing is DEBUG-level only. The
hardware-validated legacy Mac RDC keyboard profile requires all of: build 0,
RDP version `0x00080004`, OS fields Windows/NT (`0x0001/0x0003`), and nonempty,
identical hostname and product ID. It does not depend on "Mac" in either name or
on resolution/color depth. Clients not matching this fingerprint keep their
existing mappings.

For this profile only, incoming RDP Left Control maps generally to macOS Command
on both key-down and key-up. Hardware testing established that physical right
Command and physical left Control send identical Left Control events: **both
therefore become Command**. Physical left Command is consumed locally by RDC and
cannot be restored server-side. Extended RDP Right Control remains genuine macOS
Control if the client can transmit it; the tested MacBook has no physical right
Control, so no distinct physical Control route has been established there.
Option, Shift, Caps Lock, and other keys retain their existing behavior. This is
an observed compatibility fingerprint, not a unique authenticated client ID.

## Hardware smoke test

On Sonoma, grant **Screen & System Audio Recording** and **Accessibility** to the
signed app, then connect through loopback SSH. Confirm the first frame, input, audio
(when supported), disconnect/reconnect, and display restoration. For latency
regression, run a high-motion screensaver locally, connect at target depth/resolution
(Win98: 1024x768, 16-bit), and separately note idle, typing, window movement,
scrolling, and full-screen-motion responsiveness. See `docs/mac-shadow-latency.md`
for detailed behavior and test notes.

Build 0.1.8 hardware acceptance, protocol repairs, and regression evidence are
recorded in [the regression report](docs/mac-shadow-0.1.8-regression.md).
