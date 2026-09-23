# Mac Shadow RDP architecture

This note describes the shipped 1.0.0 path. The implementation remains inside
FreeRDP's shadow server and classic RDP bitmap pipeline.

## Data path

1. `server/shadow/Mac/mac_shadow.c` captures the physical Mac display and
   publishes immutable framebuffer state through the shadow surface.
2. Mac capture damage is accumulated for up to 50 ms. Capture changes during
   that interval union their dirty regions while replacing obsolete pixels
   with the newest state. First frame, reconnect, and client-requested refresh
   bypass this aggregation window.
3. `server/shadow/shadow_client.c` maps pending damage to negotiated bitmap
   updates. The sender retains its committed client-cache model and replaces
   only work that has not yet been serialized. Serialized RDP updates remain
   ordered.
4. The ordinary/F scheduler uses bounded bitmap work and fixed 256000 B/s
   graphics pacing. The server checks for input between graphics batches and
   respects transport backpressure.
5. FreeRDP owns session transport, security negotiation, input, and virtual
   channels. The menu app supervises the loopback server process and manages
   macOS permissions and Launch at Login registration.

Publication cadence and transport pacing control different parts of the path:
the 50 ms aggregation window reduces redundant capture publications; the fixed
250 KiB/s pacer limits graphics bytes sent toward the client. Aggregation does
not create an encoded-frame FIFO or discard serialized updates.

## Compatibility and state invariants

- Preserve the classic 16-bit interleaved path and 32-bit planar path, client
  packet limits, sparse damage, and negotiated ScrBlt behavior.
- Treat captured pixels, published snapshots, unsent damage, and the ordered
  client-cache state as separate states. Replacing unsent content must not
  advance cache state or invalidate ScrBlt sources prematurely.
- Keep immutable frame publication and lock ownership. Full refresh and first
  frame must converge after reconnect, including a static desktop.
- Keep mobile/client-sized surfaces and coordinate transforms separate from
  physical display-mode changes. Restore any physical mode changed for the
  explicit legacy profile.
- Clipboard, audio, and other channel work must not starve input or graphics.
  Queue bounds and cancellation must be explicit at the relevant operation,
  not inferred from asynchronous dispatch alone.

## Runtime boundary

The supported deployment binds only to `127.0.0.1:3390`, behind SSH. RDP
legacy security is enabled for the Windows 98 client profile; it is not a
reason to expose the unauthenticated listener externally. The installed app is
`~/Applications/FreeRDP Shadow.app`, signed with the project's Apple
Development identity, and registered through macOS SMAppService Launch at
Login.

## Code map

| Responsibility | Main files |
| --- | --- |
| Mac capture, publication, display and input | `server/shadow/Mac/mac_shadow.c`, `mac_shadow.h` |
| Bitmap scheduling, sparse damage, cache and pacing | `server/shadow/shadow_client.c`, `shadow_bitmap.c`, `shadow_publication.c`, `shadow_pacer.c`, `shadow_encoder.c` |
| Channel handling | `server/shadow/shadow_channels.c` and channel implementations under `server/shadow/Mac/` |
| Menu app and Service Management | `packaging/macos-shadow-menu/FreeRDPShadowMenu.m` |
| Release build, signature and installation | `scripts/build-macos-shadow-app.py`, `scripts/install-macos-shadow-menu.sh` |
| Regression coverage | `server/shadow/test/`, `packaging/macos-shadow-menu/test/` |

See [DEVELOPMENT.md](../DEVELOPMENT.md) for the build/test workflow and the
[engineering history](mac-shadow-engineering-history.md) for experiment
results and design decisions.
