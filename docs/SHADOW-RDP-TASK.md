# Implementation task: Android clipboard stall and progressive session lag

Historical task brief for the September 22, 2026 investigation. Current
implementation and validation status are recorded in `mac-shadow-latency.md`
and `../DEVELOPMENT.md`; the instructions below are preserved as provenance.

Work in `/Users/<redacted>/gitr/RDP/FreeRDP-mac-shadow--sonoma`.
Read the replacement root `AGENTS.md`, `DEVELOPMENT.md`, and
`SHADOW-RDP-SOURCE-REVIEW.md`. This task authorizes focused repository edits,
regression tests, and builds. It does not authorize replacing/restarting the
active app, interrupting the user's remote session, exposing a listener,
changing tags/releases, or pushing/merging. Use the existing project workflows.

Proceed through inspection, deterministic reproduction, focused implementation,
and validation wherever the available environment permits. Do not return only
another plan. Do not implement every hypothesis simultaneously. Stop only the
part that genuinely needs unavailable hardware, credentials, or deployment
approval, and complete independent work.

## Objectives and supplied checkpoint

1. Apply the replacement `AGENTS.md` as the durable project guide.
2. Repair Android/aRDP phone-to-Mac text clipboard stalls without regressing
   Win98 or Mac clients and without coupling clipboard failure to session stalls.
3. Find and repair the cause of latency that grows with connection use and
   briefly improves after reconnect. Investigate the full pipeline, including
   improvements beyond the original suspected cache/buffer explanation.
4. Leave focused tests, an evidence-based result, and a safe candidate build.

The supplied September 22, 2026 state is clean `master`, synchronized with
`origin/master`, at `8ded81d898ca2c8b5432fec73f2d00a68c3bd6eb`, tag
`mac-shadow-rdp-0.1.8`. Recovery baseline `mac-shadow-rdp-0.1.7` is `59386e731`.
Verify, do not reset or assume this is still the actual checkout.

Observed failure: after copying text on the Android phone and attempting paste
into the remote Mac, paste does not finish within 30 seconds and the session
becomes nearly hung. Disconnect/reconnect restores interaction but cancels paste.
Paste wholly within the remote Mac works. Separately, the connection gets slower
over time; reconnect helps, but slowness returns relatively quickly. Treat them
as separate reproductions until evidence links them.

The baseline is a machine-specific Sonoma alpha/beta, not a general-purpose
stable release. A historical signed package and 13/13 test report exist. Current
local tests reportedly abort before execution because
`/usr/local/opt/jansson/lib/libjansson.4.dylib` is missing. No current test pass is
established by that historical report.

## A. Establish a usable baseline

Record HEAD/worktree, host/SDK/architecture, CMake/build configuration, and test
binary paths. Inspect `otool -L`, relevant rpaths/CMake cache, and installed
compatible Jansson paths. Repair the narrow dependency/configuration issue or
regenerate an isolated test build. Do not blindly symlink incompatible dylibs,
clean/upgrade Homebrew broadly, change the stock FreeRDP installation, or count
loader failures as executed tests. Record any dependency change.

Run the targeted Release checks in `DEVELOPMENT.md`; preserve the actual results
and `git diff --check`. If the environment blocks one platform-specific check,
continue useful source work and deterministic checks that can run, reporting the
remaining blocker precisely.

For runtime evidence, identify the actual server PID, executable, loaded-library
paths, installed/built version, client fingerprint, negotiated color depth,
resolution, update-size limit, graphics path, ScrBlt support, channel selection,
and audio state. Obtain the phone's exact aRDP build when accessible. Do not
assume a similarly named upstream Android client is the installed implementation.
Use read-only evidence from the existing session where safe. Do not trigger a
paste, change global clipboard contents, or disrupt a remote session unannounced.

## B. Clipboard: locate the stopped transition and fix the failure model

Inspect:
- `server/shadow/Mac/mac_shadow_clipboard.m/.h`;
- `channels/cliprdr/server/cliprdr_main.c` and relevant parser/serializer code;
- `server/shadow/shadow_channels.c` and the virtual-channel manager;
- `server/shadow/test/TestMacShadowClipboard.m`.

Trace both directions independently. For phone-to-Mac, distinguish:
client format offer → server acknowledgement → data-request enqueue → request
actually submitted → response received → decode → pasteboard operation → paste
consumed by the destination app. The keypress and clipboard-data flows are
separate; do not assume a sent paste shortcut proves data is ready. Compare a
menu paste with a keyboard shortcut only in an authorized reproduction.

Add bounded metadata-only diagnostics and injectable clock, transport, and
pasteboard seams. Capture function/queue durations and thread identity. During an
authorized failing reproduction, collect a bounded thread sample before recovery.
Establish whether only clipboard, the destination Mac app, the Android UI, or the
server session loop is waiting. The existing `cliprdr` worker is already separate
from the graphics loop; “clipboard is synchronous” alone does not prove a
session-wide deadlock.

Specifically test the findings in the source review:
- an unanswered request leaves `requestedClientFormat` occupied;
- serial-queue pasteboard work blocks later clipboard callbacks;
- shutdown depends on the same queue and an unbounded cancellation wait;
- rapid ownership changes, feedback, unsupported/no-text offers, malformed
  encodings, and large text need defined behavior.

Implement a small explicit per-session state machine if the current scattered
fields cannot express safe recovery. Keep protocol state serviceable independently
of blocking OS calls, bound outstanding work and payloads, copy/retain callback
data before asynchronous use, and prevent late work accessing a freed context.
Do not solve a stall by spawning unlimited workers or simply moving it to another
unbounded FIFO. If a pasteboard operation cannot be cancelled, isolate that fact;
a timer is not cancellation. Evaluate process isolation only with supporting
samples and a justified lifecycle/signing design.

Choose and document finite deadlines with tests, distinguishing local queue delay
from remote response delay. For incoming requests that cannot be satisfied, return
a protocol-appropriate failure. For outgoing requests, do not clear the slot and
blindly issue a replacement: responses have no request ID or format ID. Preserve
an outstanding tombstone/quarantine until it is safely resolved, or use a verified
channel recovery mechanism. Local generation tokens are necessary for local
callback lifetime but insufficient for wire-response matching. Where ambiguity
cannot be resolved safely, report clipboard degradation while preserving the
RDP session; do not silently accept stale data as a new paste.

Preserve Unicode-first modern handling, named UTF-8 wire IDs versus encoding,
ANSI/OEM support, newline/termination rules, unsupported-client behavior, and
Win98's connection-specific CF_TEXT/trailer dialect. Do not enable files, images,
rich text, or broad drive redirection. Never auto-replay a cancelled paste key.

Deterministic tests must include success in both directions through a fake
pasteboard; missing/delayed/failed/unsolicited responses; late response after
timeout; repeated offers while busy; malformed/empty/oversized data; a slow or
blocked pasteboard; concurrent local/remote changes; disconnect during each
stage; repeated teardown; and unaffected simulated input/graphics progress.
Tests must not modify the real system clipboard by default.

## C. Progressive lag: distinguish CPU work from accumulated queue age

Instrument the full path, not only the existing bitmap flush. Use monotonic time
and bounded counters/histograms/rings. At minimum collect:

| Boundary | Evidence |
| --- | --- |
| Input | Arrival-to-dispatch delay, maximum loop gap, injection duration |
| Capture/publication | Callback rate, scale/copy time, lock/publication wait, publication IDs/age |
| Cache/search | Cold/known tile counts, staging/trim/search/encode time, all search probes, copy hit rate, fallback reason |
| Pending damage | Count/bytes estimate, oldest unresolved damage age, newest available publication |
| Channels/audio | Enqueue/consume rate, queue depth/bytes/oldest age, audio age, manager service duration |
| Transport | Buffered bytes where observable, write-block/flush duration, socket metrics with their scope stated |
| Resources | CPU, RSS, threads/timers/handles and relevant memory-pressure indicators over time |
| Client | Actual presentation delay where measurable; Android decode/UI/GC/thermal evidence when accessible |

Do not reset an old unsent region's age merely because a fresh publication
arrives. Do not confuse local acceptance by SSH with remote presentation. Use
same-clock measurements for stage timing; use client evidence or external
input-to-visible recording for true end-to-end timing, with sample counts.

Prioritize these selective tests:

1. **Warm-cache computation.** Measure `shadow_bitmap_stage`, `trim_damage`,
   `shadow_bitmap_next`, and `find_copy`. The outer eight-millisecond budget does
   not bound their internals. Compare identical cold and warmed states; disable
   copy search only as a test variable. Bound every candidate probe, not just
   successful signature matches. Any optimized search must retain exact pixel
   verification, overlap ordering, pending damage, and final-screen convergence.
2. **All-channel scheduling.** Measure the unbudgeted channel-manager drain from
   the session loop. Inspect large PDU fragmentation, per-channel fairness, and
   output blocking. The shadow queue's 256-message limit does not automatically
   bound the downstream channel queue. Fix the appropriate boundary without
   globally changing FreeRDP behavior for unrelated users.
3. **Audio age/backlog.** Compare audio enabled/disabled. The newest item in one
   batch may still be old relative to the whole queue. Consider bounded
   pre-serialization admission and age policy, preserving timing/negotiation.
   Do not discard arbitrary already-serialized audio or other PDUs.
4. **Fallback and resource lifetime.** Trace every `bitmapFallback` writer/reset,
   inject a cache failure, and verify damage-preserving fallback. Check whether
   recovery can be safely retried with backoff and resynchronization. Track
   timers, workers, objects, and memory instead of calling all growth a leak.
5. **Downstream path.** Hold resolution, depth, workload, and features constant
   across LAN, direct cellular, and corporate VPN where available. Inspect the
   actual SSH path and client behavior. Do not alter firewall/route/tunnel settings
   through the only remote-access path or weaken security for an experiment.

Keep cause separation: fresh connection versus aged connection; idle-age versus
motion-age; clipboard unused versus repeated paste; audio off versus on. Capture
evidence before reconnect, and note which process/transport state reconnect
actually recreated. Recovery by server restart, client restart, session reconnect,
or waiting after motion stops are different experiments, not interchangeable ones.

## D. Evaluate improvements, including less-obvious ones

Keep an evidence table: proposal, addressed bottleneck, expected tradeoff,
measurement, correctness risk, result, and retain/reject/defer decision. Evaluate
rather than indiscriminately implement:

| Candidate | Required justification / safeguard |
| --- | --- |
| Time/candidate-bounded copy search; verified motion-vector reuse or signature index | Warm-cache CPU cost falls without corrupting copy sources or disabling useful ScrBlt globally |
| Incremental dirty-region conversion and aging/fair tile selection | Avoid full-frame work while preserving all unresolved damage and static convergence |
| Queue-age-aware admission/pacing | Reduce backlog before serialization; add hysteresis and an explicit observation limit when client presentation is unknown |
| Shared output fairness with reserved control service | Clipboard/control is not starved by graphics/audio; preserve framing and channel ordering |
| Bounded latest-audio handoff before serialization | Measured queue age stays bounded with acceptable audio behavior and clean reconnect |
| Small input-response-region priority, followed by fair remainder | Measurable interaction benefit, no indefinite peripheral damage starvation, no invented client feedback |
| Cache-aware safe fallback recovery | Observed sticky fallback is repaired without allocation thrash or invalid cache assumptions |
| Socket/SSH parameter changes or modern-client codecs | Existing setting and bottleneck verified first; do not apply blindly or change the Win98 baseline |
| ScreenCaptureKit or alternate modern transport | Separate follow-on only if capture/transport evidence supports it; not a prerequisite for these bug fixes |

Do not call a shorter queue a win when computation becomes excessive, a smoother
animation a win when input gets older, or lower bandwidth a win when copy-search
cost dominates. More compression, bigger buffers, more threads, and higher FPS
can each worsen the actual objective. Keep one-variable comparisons and rollback.

## E. Validation and deliverables

Run the original targeted Release checks plus new deterministic failures/recovery
checks. Exercise delayed readers, constrained throughput, repeated publications,
copy overlaps, static final-frame correctness, channel congestion, failed writes,
and teardown while work is pending. Run suitable sanitizers separately where
supported; do not conflate them with production performance.

For authorized hardware testing, reproduce on the user's Android/aRDP path first,
then smoke-test Win98 RDP 5.2 at 1024×768/16-bit and Microsoft Remote Desktop for
Mac. Retain first frame, input/modifier behavior, reconnect, audio, text clipboard,
resolution policy, and display restoration. Do not declare these verified from
synthetic tests alone.

A proposed primary soak is at least 60 minutes, or materially longer than the
observed failure onset, with checkpoints at fresh, 5, 15, 30, and 60 minutes.
Record idle, typing/paste, dragging, scrolling, sustained motion, and recovery when
motion stops. Repeat critical before/after conditions sufficiently to distinguish
noise, including an aged-session paste. Report p50/p95 and maximum with counts;
report p99 only with adequate samples. These are validation tasks, not promises
that this chat has already executed a soak.

Acceptance: small healthy text pastes complete without the reported stall; failed
clipboard operations have a documented bounded failure/recovery behavior and do
not require disconnecting to regain input/display; repeat workloads do not build
unexplained queue age or resource growth; warmed/aged interaction does not show
the previous degradation; the static desktop converges; compatibility/security
remain intact. State any limitation on safe clipboard-only re-synchronization.
Choose numerical latency/queue targets from the measured baseline before judging
a candidate; do not invent performance results or promise network-independent
latency.

Update relevant tests and the authoritative status/latency docs. Correct the
roadmap's obsolete “clipboard later” implication by separating implemented text
clipboard, this new reliability work, and still-unimplemented file transfer;
preserve dated acceptance evidence. Build a candidate using
`python3 scripts/build-macos-shadow-app.py` when the proper signing environment
is available. Do not install it or alter signed contents afterward.

Final report: concrete changes, demonstrated causes versus remaining hypotheses,
exact test results/environment blockers, before/after evidence, accepted/rejected
optimizations, candidate artifact identity, unperformed hardware cases, and
commit/deployment state. No automatic version bump, retag, release, or push.
