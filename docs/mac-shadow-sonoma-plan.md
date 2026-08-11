<!-- markdownlint-disable MD013 -->

# FreeRDP macOS Shadow Server: Sonoma and Windows 98 Roadmap

## Mission and constraints

The project exports the physical macOS 14 Sonoma desktop through FreeRDP's
shadow server to Microsoft Remote Desktop 5.2 on Windows 98 SE. The target is a
sharp 1024×768, 16-bit-color desktop with low interactive latency.

The initial work repairs the existing `CGDisplayStream` backend, which Sonoma
still provides. ScreenCaptureKit is a later, separately reviewable capture
backend, not a prerequisite for the Sonoma experiment.

The first validated deliverable does not include video codecs, audio, drive
redirection, clipboard, multi-monitor support, or Retina optimization. It must
not expose a listener directly to the Internet or weaken FreeRDP's security
defaults globally. The intended deployment is `127.0.0.1:3390` through an SSH
tunnel.

## Baseline and retained architecture

| Item | Value |
| --- | --- |
| Upstream | [`FreeRDP/FreeRDP`](https://github.com/FreeRDP/FreeRDP) |
| Baseline commit | `9415f2d11e4cbc4e25d3d9fd0c4271e2e05d5c58` |
| Repository | `FreeRDP-mac-shadow--sonoma` |
| Integration branch | `master` |
| Capture backend/state | `server/shadow/Mac/mac_shadow.c`, `server/shadow/Mac/mac_shadow.h` |
| Legacy bitmap path | `server/shadow/shadow_client.c` |
| Arguments/security | `server/shadow/shadow_server.c`, `server/shadow/cli/shadow.c` |

At less than 32-bit client color depth, the generic shadow client already
selects `FREERDP_CODEC_INTERLEAVED`, splits updates into 64×64 rectangles, and
compresses them with the classic bitmap path. Retaining the generic shadow
server, RDP transport, classic encoder, and command-line interface minimizes
fork-specific behavior and preserves current-client compatibility. Capture can
later move to ScreenCaptureKit without replacing that publication and protocol
path.

## Roadmap at a glance

Patch 0 is the immediate, hardware-dependent blocker. No source repair can be
called validated until that Sonoma baseline exists.

| Patch/phase | Milestone | Status | Depends on | Exit criterion |
| --- | --- | --- | --- | --- |
| 0. Sonoma baseline | Baseline/correctness | **Immediate blocker; hardware work required** | Sonoma host | Reproducible baseline matrix and saved logs, including failures |
| 1. Capture callback | Baseline/correctness | **Implemented in source; compilation and runtime validation pending** | Patch 0 environment | No first-frame null access; dirty state contains only pending work |
| 2. Locking/publication | Baseline/correctness | **Implemented in source; compilation and runtime validation pending** | Patches 0–1 | Stress/runtime checks find no obvious race or unbalanced lock |
| 3. First frame/refresh | Baseline/correctness | Planned | Patches 1–2 | Every initial connection and reconnect immediately gets a complete desktop |
| 4. Lifecycle | Baseline/correctness | Planned | Patches 1–3 | Repeated start/stop and failure cycles leave no capture or worker resources |
| 5. Input | Usability | Planned | Patch 0; coordinate with Patch 4 ownership | Written Win98 keyboard/mouse matrix passes |
| 6. Permissions | Usability | Planned | Patch 0; precedes packaging | Missing grants produce distinct, actionable diagnostics |
| 7. Legacy profile | Win98 compatibility | Planned | Patches 3–6 | VAIO connects through SSH without changing normal security defaults |
| 8. Measurement/tuning | Performance | Planned | Correctness and Patch 7 | Lowest-latency stable settings selected from recorded measurements |
| 9. Stable wrapper | Packaging | Planned | Patches 4, 6–8 | Repeatable install and start/stop procedure on the iMac |
| 10. ScreenCaptureKit | Modernization | Later | Validated `CGDisplayStream` publication contract | Current-SDK build retains the legacy RDP client path |
| 11. Clipboard/files | Later client/channel | Later | Stable capture, input, lifecycle, and legacy security | Predictable opt-in transfer without broader default exposure |
| 12. Win98 companion | Later client/channel | Later | Validated Patch 7 settings | Optional launcher/add-on makes the connection repeatable |
| 13. Open-source client | Later client/channel | Later research | Measurements from Patch 8 | Auditable client measurably improves bounded interactive latency |

Status is defined once in this table. The milestone sections below describe
scope, dependencies, and verification rather than repeating readiness claims.

## Milestone A: Baseline and correctness

### Patch 0 — Reproducible Sonoma baseline

1. Build unmodified FreeRDP on the Sonoma iMac with warnings and the macOS
   shadow subsystem enabled.
2. Record compiler, SDK, architecture, CMake options, dependencies, binary path,
   and exact launch command.
3. Check startup and capture with a modern RDP client, then Microsoft RDP 5.2.
4. Save debug logs for black screen, first connection, reconnect, and shutdown.

This baseline gates every subsequent validation claim. Its output is the
repeatable baseline matrix, whether individual cells pass or fail.

### Patch 1 — Capture callback correctness

1. Reject non-complete statuses before dereferencing frame/update objects.
2. Validate subsystem, server, surface, `frameSurface`, and `updateRef`.
3. Derive dirty rectangles from the callback's current `updateRef`.
4. Remove indefinite `lastUpdate` accumulation; retain update state only when a
   bounded handoff genuinely requires it.
5. Clamp rectangles to the shadow-surface dimensions before conversion.
6. Log malformed or empty updates at an appropriate debug level.

Validation requires a warnings-enabled build plus first-frame, malformed-update,
idle/resume, and reconnect exercises on Sonoma.

### Patch 2 — Surface locking and frame publication

1. Keep a defined lock boundary around region mutation, extent calculation,
   surface copy, publication, and invalid-region clearing.
2. Eliminate unmatched critical-section exits.
3. Ensure the encoder cannot read pixels while capture mutates them.
4. Avoid holding the client-list lock during expensive conversion when safe.
5. Use assertions or structured cleanup for each critical-section and IOSurface
   lock.

Validation requires Thread Sanitizer or an equivalent runtime check plus manual
stress with rapid display changes and reconnects.

### Patch 3 — First-frame and refresh behavior

1. Maintain a current framebuffer with no connected client, or take a one-shot
   full capture when the first client arrives.
2. Make `SHADOW_MSG_IN_REFRESH_REQUEST_ID` trigger full invalidation and fresh
   publication rather than sending potentially stale pixels.
3. Connect to a completely static desktop.
4. Disconnect, wait on a static desktop, and reconnect without local motion.

### Patch 4 — Clean lifecycle

1. Store the worker handle and explicit running/stopping state.
2. Signal the queue, stop `CGDisplayStream`, wait for the worker, and prevent new
   callback work during shutdown.
3. Release the stream, dispatch queue where required, event source, retained
   updates, and thread handle through one ownership path.
4. Clear `g_Subsystem` safely; prefer per-instance callback context if feasible.
5. Unwind partial initialization and make failed/repeated starts deterministic.

Twenty-five start/connect/disconnect/stop cycles, failed startup, sleep/wake,
and clean process termination are required to exit this milestone.

## Milestone B: Usability

### Patch 5 — Keyboard and mouse correctness

1. Treat key-down as the absence of the release flag and preserve extended
   scan-code state.
2. Reject failed scan-code/key-code translations before posting.
3. Retain one `CGEventSource` per subsystem.
4. Synchronize Shift, Control, Option/Alt, Caps Lock, and the documented Command
   mapping.
5. Implement Unicode input for clients that send Unicode events.
6. Post exactly one move/drag event and one button transition per RDP event.
7. Normalize both wheel directions to 120 units and preserve partial deltas.
8. Implement extended buttons or reject them with a clear diagnostic.

Exit requires the written Win98 input matrix: typing, shortcuts, extended keys,
drag, right-click, wheel both ways, and stuck-modifier recovery after disconnect.

### Patch 6 — Permission preflight and diagnostics

1. Preflight Screen Recording before capture startup and Accessibility before
   input injection.
2. Distinguish capture unavailable, permission denied, stream creation failed,
   and input unavailable in logs and exit behavior.
3. Permit an optional prompt in a future interactive wrapper while retaining a
   useful CLI for already-granted permissions.
4. Document why macOS privacy grants require a stable executable identity.

Exit requires separate, actionable results for neither grant, capture only,
input only, and both grants.

## Milestone C: Windows 98 compatibility

### Patch 7 — Explicit legacy-client profile

1. Verify the exact switches for classic RDP security, disabled NLA/TLS where
   RDP 5.2 requires it, 16-bit color, port 3390, and loopback bind.
2. Add a named profile or documented wrapper only if existing switches cannot
   express the configuration clearly.
3. Confirm interleaved bitmap compression is selected instead of NSCodec,
   RemoteFX, AVC, or GFX.
4. Keep legacy authentication/security behavior local to this profile.
5. Verify the entire session through SSH without network listener exposure.

This milestone depends on reliable first frame, lifecycle, input, and permission
behavior. It exits only when the VAIO repeatedly reconnects through the tunnel
without weakening server defaults.

## Milestone D: Performance

### Patch 8 — Measure before tuning

1. Instrument capture-to-send time, encoded bytes, queued frames, and dropped or
   coalesced frames.
2. Compare caps of 8, 12, 16, and 20 FPS at 16-bit color.
3. Compare a bounding box with multiple dirty rectangles.
4. Discard stale intermediate frames rather than accumulating latency.
5. Verify cursor exclusion and client-side cursor updates.
6. Measure idle, typing, menu use, dragging, scrolling, and full-screen motion
   separately.

The exit artifact is a recorded comparison and a justified, bounded-latency
configuration at native 1024×768—not a claim of video-rate performance.

## Milestone E: Packaging

### Patch 9 — Stable identity and operation

1. Produce a signed minimal `.app` wrapper or stable launcher identity for macOS
   privacy permissions.
2. Add LaunchAgent/start-stop tooling only after lifecycle validation.
3. Make the local wrapper bind to `127.0.0.1:3390` by default.
4. Document dependencies, source build, permissions, launch, tunnel, client
   settings, logs, and complete removal only after Sonoma validation.
5. Keep the stock Homebrew installation untouched and package the fork
   separately.

Packaging depends on lifecycle, permissions, the legacy profile, and chosen
performance settings. Its exit artifact is a repeatable install plus one
start/stop procedure on the target iMac.

## Milestone F: Modernization

### Patch 10 — ScreenCaptureKit follow-on

1. Add a small Objective-C or Objective-C++ ScreenCaptureKit adapter.
2. Preserve the repaired backend's `rdpShadowSurface` publication contract.
3. Select the backend by supported SDK/OS policy, or retire `CGDisplayStream`
   only after comparative validation.
4. Keep input injection separate because ScreenCaptureKit replaces capture only.

This work is intentionally isolated from the Sonoma repair series. It exits
when current macOS SDKs build without removed declarations and both modern and
legacy clients retain the established bitmap path.

## Milestone G: Later client and virtual-channel work

### Phase 11 — Clipboard and limited file transfer

Add clipboard redirection only after capture, input, lifecycle, and legacy
security are stable. File transfer must be opt-in and define size, destination,
overwrite, and cancellation behavior instead of enabling broad drive
redirection. Treat content as untrusted; test text encodings and filenames with
modern and Windows 98 clients independently. Exit requires predictable behavior
without broader default exposure or desktop-update regressions.

### Phase 12 — Windows 98 companion launcher and add-on

Build an optional Windows 98 launcher that applies validated RDP 5.2 settings
and establishes or guides SSH forwarding. A separately installable
virtual-channel add-on may supply features absent from the Microsoft client.
The unmodified Microsoft client remains the supported baseline. Document
installation, removal, compatibility, and recovery. Exit requires a repeatable
VAIO connection without FreeRDP-wide defaults or mandatory client changes.

### Phase 13 — Latency-optimized open-source Windows 98 client

Evaluate open-source RDP code that can target Windows 98 or an appropriate
lightweight companion device. Optimize input priority, frame pacing, dirty
rectangles, and classic bitmap decode for bounded interactive latency rather
than video FPS. Retain interoperability with the loopback/SSH and explicit
legacy-security profile. Publish source, build instructions, measurement method,
and comparable results. Exit requires an auditable client that meets deployment
constraints and measurably improves on the Microsoft RDP 5.2 baseline.

## Detailed defect analysis

### Capture callback and dirty regions

The original callback could consult retained update state instead of its current
`updateRef`, dereference callback objects without sufficient checks, pass
unclamped dirty rectangles into conversion, and clear invalid state too early.
Patches 1–2 derive rectangles from the current callback, validate context, clamp
coordinates, and retain invalid state through publication.

### Locking and publication

The former callback had mismatched cleanup possibilities around the surface
critical section and IOSurface lock. Image-copy failure and invalid-region
lifetime also needed a single structured publication boundary. Stress testing
must still prove that callback mutation and encoder reads do not race.

### First frame and reconnect

With no clients, the callback returns before copying the current frame into the
shadow surface. A new client can therefore see empty or stale pixels until the
display changes. A refresh must force a full capture and update, not merely
republish old storage.

### Lifecycle

`mac_shadow_subsystem_start()` does not retain its worker-thread handle, while
`mac_shadow_subsystem_stop()` does no ordered shutdown. The stream, worker,
dispatch queue, and global subsystem pointer can leak or race during restart and
exit.

### Input

- Keyboard logic treats `KBD_FLAGS_DOWN` as a positive flag although RDP
  key-down is normally represented by the absence of the release flag.
- Unicode and synchronize/modifier handlers are stubs.
- Motion can be posted once in the move branch and again unconditionally,
  sometimes using `kCGEventNull`.
- Negative wheel movement divides by 392 while positive movement divides by
  120.
- Extended mouse input is a stub.
- Nearly every event allocates a new `CGEventSource`.

### Sonoma permissions

The backend does not preflight Screen Recording or Accessibility. A missing
capture grant can resemble a black screen, while a missing input grant can
resemble dead input. Startup must diagnose these capabilities independently.

## Validation matrix

| Area | Cases |
| --- | --- |
| Build | Intel Sonoma host; Apple Silicon if available; warnings enabled; macOS shadow enabled |
| Capture | First frame; idle; blank status; malformed update; resolution change; sleep/wake |
| Lifecycle | Start; stop; failed start; reconnect; 25 repeated cycles; clean exit |
| Client | Modern RDP control; Microsoft RDP 5.2 on Windows 98 SE |
| Display | 1024×768; 16-bit target; static desktop; scroll; full-screen change |
| Keyboard | Letters; symbols; modifiers; extended keys; Unicode; disconnect while held |
| Mouse | Move; left/right/middle; drag; wheel up/down; edges/corners; local cursor |
| Permissions | Neither grant; capture only; input only; both grants |
| Network | Loopback direct; SSH tunnel; forced disconnect; high-latency link |
| Security | Legacy profile only; listener visibility; unchanged default negotiation |

## Performance record

For every idle desktop, typing, window movement, scrolling, and full-screen
motion run, record separately:

- Mac callback rate and CPU use;
- dirty pixels and encoded bytes per second;
- VAIO CPU use and observed update rate;
- input-to-visible-response latency, preferably from high-frame-rate video; and
- whether latency remains bounded under continuous activity.

Success means pixel-sharp 1024×768 work, immediate local cursor motion,
responsive typing, and no ever-growing update queue. It is not defined as 30 FPS
video.

## Integration and review order

- Keep each patch independently reviewable and land correctness before tuning.
- Do not combine capture, input, security, packaging, or ScreenCaptureKit work.
- Attach baseline and post-fix logs to their commits or pull requests.
- Maintain the deployment fork while preparing smaller upstreamable repairs.
- Begin with Patch 0 on the Sonoma iMac; it is the only immediate next action.

## Attribution and license

This work retains the architecture and implementation of
[FreeRDP](https://github.com/FreeRDP/FreeRDP) and is distributed under the
repository's [Apache License 2.0](../LICENSE). Credit remains with the upstream
FreeRDP contributors and the Microsoft Open Specifications on which protocol
interoperability depends.
