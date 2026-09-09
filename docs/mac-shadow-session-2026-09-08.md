> Historical implementation record. Production cleanup supersedes the opt-in switches,
> diagnostics, and comparison launcher described below; see mac-shadow-latency.md.

# September 8, 2026: latency work and comparison guide

## What changed in this session

The work implements the five requested latency improvements. Changes remain
uncommitted. No commits dated today were found when preparing this summary.
The pre-change repository commit is `7c8084cf5`.

The initial code/protocol review recommended optimizing this RDP implementation
before replacing it with VNC. VNC remains a useful benchmark candidate; no VNC
server/viewer was installed or benchmarked in this session, and no measured
claim that RDP is faster was made.

| Area | Change | Activation |
| --- | --- | --- |
| Input | Read incoming protocol traffic before graphics, with up to 32 already-buffered PDUs processed per iteration | Default |
| Capture | Capture into a private framebuffer while one worker publishes an immutable snapshot | Default on Mac |
| Sparse updates | Compare RGB565 pixels in 64×64 tiles, trim changed bounds, and encode only pending changes | `FREERDP_SHADOW_LOW_LATENCY=1`, eligible 16-bit sessions |
| Backpressure | Stop new graphics submissions when FreeRDP output is blocked; explicitly drain it and resume on timed wakeups | New scheduler |
| Newest state | Replace pending image contents instead of accumulating a frame FIFO; retain unsent changes | New scheduler |
| Movement | Verify screen-copy sources against the submitted framebuffer; send ScrBlt or fall back to bitmaps | Also `FREERDP_SHADOW_SCRBLT=1` and negotiated support |

The legacy scheduler bounds each event-loop graphics batch to eight operations
and a 16 KiB scheduling budget, checking an eight-millisecond deadline between
operations. Individual operations are not preemptible. These are scheduling
limits, not a promise of eight-millisecond end-to-end latency.

Movement detection searches within 128 pixels in each direction. It supports
non-tile-aligned scrolling and diagonal moves, verifies exact pixels, limits
expensive comparisons on repetitive content, and chooses traversal direction
to preserve sources. Partial copies at screen edges leave exposed areas for
bitmap repair. Copies whose sources are unknown or no longer match use bitmaps.
The drawing-order sender requires both client-advertised ScrBlt support and
fast-path output.

Additional correctness work necessary for this pipeline:

- Track and join the Mac worker before the generic layer frees its message pipe.
- Drain capture callbacks and wait for publication before releasing or resizing
  framebuffer storage; release worker handles and dispatch queues on teardown.
- Replace the global capture-subsystem pointer with an instance captured by the
  stream callback, and reject new connections once shutdown has begun.
- Signal clients to quit before joining a publisher that may be waiting for them.
- Retain callback storage if a display stream refuses to stop, avoiding a
  callback into freed memory on that exceptional path.
- Keep refresh and resize cache invalidation explicit, and suppress outgoing
  graphics when output is suppressed.
- Use padded scratch storage for narrow/odd-width rectangles so bitmap
  alignment cannot read past the framebuffer or enlarge destination bounds.
- Fix an unmatched surface-lock exit on region-copy failure in the existing
  client update function.
- Preserve pixel comparison against the last publication for the original
  encoder, and add debug capture/publication counters and duration diagnostics.

## Files changed or added

| Files | Purpose |
| --- | --- |
| `server/shadow/Mac/mac_shadow.c`, `.h` | Independent capture storage, publication worker, callback ownership, lifecycle, diagnostics |
| `server/shadow/shadow_client.c` | Input scheduling, new bitmap submission path, output draining, ScrBlt emission, lock cleanup |
| `server/shadow/shadow_encoder.c`, `.h` | Per-client feature settings and cache ownership/reset |
| `server/shadow/shadow_bitmap.c`, `.h` | Newest-image state, sparse damage, RGB565 comparisons, verified movement detection |
| `server/shadow/test/TestShadowBitmap.c` | Independent compressed-bitmap and screen-copy replay tests |
| `server/shadow/test/TestMacShadowPublication.c` | Stalled-subscriber and worker-lifecycle tests |
| Both shadow `CMakeLists.txt` files | Build the helper and register tests |
| `docs/mac-shadow-latency.md` | Enablement, architecture, limits, validation instructions |
| `scripts/compare-macos-shadow.sh` | Reproducible profile launcher with dry-run mode |
| This document | Session summary and comparison procedure |

`docs/mac-shadow-sonoma-plan.md` was already modified and
`docs/mac-shadow-mobile-resolution-research.md` was already untracked before
this session's implementation. Those changes were preserved and are not
attributed to this work.

Security defaults, RDP transport implementation, audio implementation, menu-app
packaging, and ScreenCaptureKit were not changed. The listener remains loopback
through SSH. Modern graphics paths retain their existing encoders. The two new
encoder switches are opt-in; the installed menu app was not reconfigured or
restarted by this work.

## Validation results

- The shadow server builds on macOS 14.7.1 with warnings enabled and the Mac
  subsystem enabled. Existing deprecation/unused-code warnings remain.
- Eleven relevant tests pass: events, critical sections, threads, message queue,
  message pipe, region, color, image copy, interleaved codec, and both new tests.
- Bitmap replay passed AddressSanitizer and UndefinedBehaviorSanitizer with
  the new scheduler instrumented; dependencies were not rebuilt with sanitizers.
- The publication test deliberately stalls a subscriber, verifies that capture
  can continue without modifying the published image, and checks the next
  snapshot contains pending damage. It also checks 25 synthetic worker cycles.
- The bitmap tests cover sparse changes, repeated replacement while partially
  sent, refresh, quantization, odd edges, scrolling, and overlapping diagonal
  copies. A deterministic 256×192 test scrolling 13 pixels used 15 copy orders
  and 832 bitmap pixels, versus 49,152 pixels in a full frame. This synthetic
  content includes reusable exposed pixels; it is not a measured bandwidth or
  latency result on the VAIO.
- An attempted additional `TestCore` build could not link because this
  server-only configuration lacks `freerdp-client`. It is not counted as passing.

Real Windows 98 interoperability, permissions, display-stream lifecycle,
reconnect/sleep/wake behavior, and end-to-end latency still require hardware
validation. Successful ordered submission does not mean the client has
displayed the pixels. SSH, kernel, network, and viewer queues remain outside
the scheduler's direct control.

