# Mac Shadow RDP — agent instructions

## Project and priorities

This is a machine-specific FreeRDP macOS 14 Sonoma shadow-server fork, not a
general-purpose production release. Preserve the working physical-desktop
experience while fixing correctness and sustained interactive latency.

The primary compatibility target is Microsoft RDP 5.2 on Windows 98 SE/VAIO at
1024×768, 16-bit color. Android/aRDP and Microsoft Remote Desktop for Mac are
active regression targets, not disposable test clients. Prefer responsive input,
readable text, fresh desktop state, and bounded resource use over animation FPS.

## Start with the right evidence

- Read `DEVELOPMENT.md` for the build, signing, compatibility, and test contract.
  Read `docs/mac-shadow-architecture.md` for the current data path and
  invariants, `docs/mac-shadow-1.0.0.md` for release evidence, and
  `docs/mac-shadow-engineering-history.md` for retained experiment conclusions.
  Read other history only when it bears on the task.
- Record the actual branch, HEAD, worktree, build directory, configuration, and
  tested executable. Do not equate a source checkout, built app, installed app,
  and running server. Verify the runtime profile and negotiated capabilities.
- The published 1.0.0 release is commit `f225ac1bb2335bae53ac8d39deea54b446fa5e44`
  and annotated tag `1.0.0`. Treat both as immutable. Historical checkpoint
  `mac-shadow-rdp-0.1.8` is `8ded81d898ca2c8b5432fec73f2d00a68c3bd6eb`; recovery
  baseline `mac-shadow-rdp-0.1.7` is `59386e731`. Verify the checkout; never
  reset to an older reference merely to make the worktree match this description.
- Separate observed behavior, source findings, hypotheses, and tested fixes.
  Historical acceptance does not overrule a new failure. Do not mistake
  implemented text clipboard/audio for unimplemented features or historical
  defects for proven current bugs. Reconcile contradictions explicitly.

## Work autonomously, within scope

Inspect the relevant execution path, reproduce or add a deterministic failing
case, implement a focused repair, and run the relevant checks. Do not stop at a
plan when safe implementation and validation are possible. Continue independent
work when hardware is unavailable; state the exact unvalidated remainder.

Preserve unrelated changes. Keep one logical repair and its tests per commit;
separate investigation tooling, behavioral changes, and packaging changes.
Prefer local Mac repairs, but follow a demonstrated bug into shared shadow,
channel, or transport code when necessary. Preserve generic behavior and add
shared-code tests. Do not use “Mac-only” as a reason to leave a proven shared-path
bug unfixed.

Do not rewrite history, force-push, merge, tag, bump a release, install, or restart
the user's active server without explicit authorization. Never interrupt the
only remote-access path. Builds and isolated tests do not authorize deployment.
Do not clean unrelated Homebrew packages, reset permissions, or kill `pboard` as
a substitute for a fix. Respect any narrower instructions for the current task.

## Compatibility and correctness invariants

- Preserve 16-bit interleaved and 32-bit planar output, negotiated packet limits,
  sparse damage, newest-state replacement, and correctly negotiated ScrBlt.
  Keep legacy/security and input quirks confined to their existing profiles.
  Preserve the precise legacy Mac RDC fingerprint in `DEVELOPMENT.md`; names,
  screen size, and color depth alone are not replacement identifiers.
- Treat the latest captured pixels, published snapshot, unsent damage, and
  ordered client-cache model as distinct states. Replace obsolete *unsent* work;
  never discard/reorder serialized PDUs or advance cache state for unsent work.
  Preserve ScrBlt source validity, overlap ordering, and eventual static-screen
  convergence. Any fallback must retain pending damage and force correction.
- Preserve immutable publication, lock ownership, first-frame/static reconnect,
  client-sized mobile surfaces, input coordinate transforms, display restoration,
  and clean teardown. Do not silently add mid-session resizing or physical
  display switching for generic/mobile clients.
- Keep text clipboard interoperable in both directions. Preserve Unicode,
  existing ANSI/OEM and named UTF-8 handling, and the Win98-only four-byte trailer
  outside `dataLen`. Do not apply that trailer or its parser exception globally.
  Files, images, HTML, and drive redirection are outside the text-clipboard scope.
- Clipboard and audio failures must not unnecessarily stall graphics/input.
  Keep potentially blocking OS work off critical session paths; specify callback
  ownership, queue ordering, payload limits, cancellation, and teardown lifetime.
  Asynchronous dispatch alone is not a bounded queue or cancellation mechanism.
- Clipboard data responses have no request identifier. Keep requests serialized;
  a local generation counter cannot identify an old response arriving after a
  timeout. Define safe late-response handling before permitting another request.
  Never replay a delayed paste keystroke into a potentially different app.
- Preserve asynchronous system audio, local playback, supported negotiation, and
  channel-disabled sessions. Do not trade an unbounded audio backlog for smooth
  playback or globally disable accepted features to make a benchmark pass.

## Performance discipline

Investigate the entire path: input, capture/scaling, publication, cache/search,
encoding, application/channel queues, transport, SSH/VPN/network, and client
decoding/presentation. A healthy loopback socket is not proof of a healthy WAN
path. A reconnect improvement is evidence, not a root-cause diagnosis.

Bound expensive work *inside* operations as well as between them. Check queue
age, bytes, and resource growth, not just frame count or FPS. Distinguish cold
and warm caches, fresh and aged sessions, and stopped-motion recovery. Preserve
fairness for small control messages and pending screen regions.

Use bounded, opt-in diagnostics: monotonic timestamps, session/publication IDs,
counters, histograms, queue age/depth, and sampled durations. Do not log clipboard
contents, keystrokes, credentials, or private screenshots. Keep hot-path tracing
out of normal INFO logs; measure its overhead. Never label server send timing
“end-to-end latency” without client presentation evidence.

Prefer one-variable, reversible experiments. Periodic reconnects/cache flushes,
larger buffers, lower resolution, disabled audio, or a new capture backend are
not root-cause fixes unless the evidence and acceptance criteria justify them.
Novel optimizations need a hypothesis, comparison, correctness check, and rollback.

## Build, test, and report

Use `python3 scripts/build-macos-shadow-app.py` for the production-configured
package. Use the separate regression build and commands in `DEVELOPMENT.md`.
Preserve Release optimization and signing with
`Apple Development: shardsofaperture (H7V72A5WH6)` and bundle ID
`io.freerdp.shadow.sonoma.menu`; never substitute ad-hoc signing. Do not modify
signed contents afterward or ship diagnostic/test products in the app.

A missing dylib or stale CMake/rpath is an environment failure until proven
otherwise. Inspect architecture, dependency paths, and the actual failing binary;
repair the narrow dependency/configuration issue. Do not fabricate compatibility
symlinks, perform broad upgrades, or count aborted/unstarted tests as passes.

Run relevant tests after source changes, add regression coverage for the failure
and its recovery, and run `git diff --check`. Validate Release behavior separately
from instrumented/sanitized builds. Hardware acceptance includes the affected
Android path plus Win98/Mac compatibility, first frame, static reconnect,
input/modifier release, audio, and display restoration. Long-session fixes need
repeatable soak and congestion-recovery evidence, not only a fresh connection.

Finish with: changed files and behavior; evidence for the diagnosis; exact checks
and results; before/after measurements when applicable; remaining limitations;
and whether anything was committed, installed, restarted, or left unvalidated.
Do not promote an untested hypothesis to “fixed.”

## Security boundary

Keep the intended listener at `127.0.0.1:3390` behind SSH forwarding. An isolated
test may use another loopback port without taking over the active service. Never
expose the unauthenticated legacy listener to the LAN/Internet, disable SSH
security, or weaken FreeRDP defaults globally.
