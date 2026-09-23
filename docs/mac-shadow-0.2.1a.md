# 0.2.1A: 50 ms latest-state publication experiment

Later status: the signed candidate was installed and physically accepted by
the user for promotion to 1.0.0. The original engineering handoff below
records its earlier, uninstalled state and the test procedure at that time.
The [1.0.0 release note](mac-shadow-1.0.0.md) records the production policy and
remaining acceptance limits.

This is one signed, uninstalled candidate built from `master` at 0.2.0
`934b8d0983c6878bf0280558d829c62f28e4ea2c`, plus the uncommitted source
patch captured beside the bundle. The stable installed 0.2.0 app remains the
physical baseline. No client-presented latency claim follows from host tests.

## Source audit and chosen boundary

`mac_shadow_capture_frame` receives `CGDisplayStream` callbacks, copies new
pixels into the private `captureSurface->data`, unions damage into
`captureSurface->invalidRegion` under its lock, and sets one manual-reset
`frameEvent`. The private capture surface is the newest authoritative pixel
state. There is no capture-frame FIFO. Before this experiment, the Mac worker
immediately called `mac_shadow_publish_pending` after that event. It took
`publicationLock`, the capture lock, then the shared surface lock; compared the
latest pixels against `server->surface`; copied changed pixels and a damage
region; and called `shadow_subsystem_frame_update`. Subscriber consumption
holds that immutable publication until clients finish staging. Capture can
continue into the private latest surface during that wait.

The generic client then stages `server->surface` through
`shadow_publication_stage` and `shadow_bitmap_stage`. That stage overwrites the
single latest unsent bitmap target and recomputes dirty tiles against `sent`.
Successful ordered tile submission alone advances the sent/known cache through
`shadow_publication_commit`. A failed or deferred submission leaves it
uncommitted. ScrBlt uses only known sent-cache sources and maintains move
ordering. The aggregation gate changes none of these contracts, codec paths,
packet limits, or 16/32-bit reconstruction. Client refresh damage remains
separate from the Mac capture damage.

## Gate semantics

Only ordinary capture publication is delayed. The first capture event after a
publication starts a 50 ms deadline; later events union damage and replace
pixels without extending it. At/after the deadline the worker publishes the
latest pixels once. It does not queue old generations, schedule full-screen
repaints, reset sender progress, change pacer credit, or impose a send deadline.
The gate is a maximum ordinary publication frequency of about 20 Hz when
capture is continuously active, subject to subscriber backpressure. Input and
channels remain on their existing client processing path. The worker's timed
wait watches stop and message events, so shutdown and client refresh can wake
it immediately despite the manual-reset capture event.

First frame/reconnect publication bypasses the gate while `publishedFrame` is
false. A client refresh message also bypasses it and requests the full current
surface. Display stream restart resets `publishedFrame` and the pending window.
No capture event means no timer or extra publication. Tiny damage remains
region-limited. A failed publish retains its damage/event and retries a full
refresh; successful publication alone clears the pending region. While the
worker waits for a subscriber, newly arriving capture damage starts its own
window and remains in the private buffer.

One nuance matters for continuous motion: aggregation reduces capture-to-stage
churn, but the bitmap sender already replaces unsent targets. It cannot retract
encoded PDUs or force the fixed 256000 B/s budget to carry more current pixels.
This makes the physical comparison necessary.

## Deterministic host evidence

`TestMacShadowPublication` directly executes the production Mac gate and
publication function with multiple timed damage events, disjoint geometry,
overlapping latest pixels, exact boundary, independent later window, refresh,
first-frame bypass, and disabled behavior. Existing bitmap/publication/pacer
tests cover failed versus successful commit, static convergence, ScrBlt,
reconnect/refresh, and 16/32-bit reconstruction. The new
`TestShadowAggregationWorkload` uses production bitmap staging, interleaved
compression, fixed pacer, and commit functions. Its first-event schedule is a
model; the separate Mac test checks the actual gate. Each output charge is
compressed bytes plus a fixed 94-byte simulated framing allowance, not measured
RDP wire bytes. One latest pending generation is retained by design.

| Workload / policy | Capture events | Publications | Simulated charged bytes | Max pending tiles | Max pending age | Restaged pending tiles | Static convergence from start | Menu visible after event |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Large reveal, immediate | 1 | 1 | 224676 | 36 | 778 ms | 0 | 778 ms | — |
| Large reveal, 50 ms | 1 | 1 | 224676 | 36 | 778 ms | 0 | 828 ms | — |
| Sparse icon, immediate | 20 | 20 | 42900 | 1 | 0 ms | 0 | 401 ms | — |
| Sparse icon, 50 ms | 20 | 7 | 15015 | 1 | 0 ms | 0 | 410 ms | — |
| Broad upper motion + lower menu, immediate | 100 | 100 | 538785 | 33 | 2005 ms | 3182 | 2005 ms | 469 ms |
| Broad upper motion + lower menu, 50 ms | 100 | 17 | 538785 | 33 | 2005 ms | 488 | 2055 ms | 519 ms |

The video model cuts publications and repeated staging sharply, but does **not**
reduce charged bytes or the stale tail; its menu arrives about 50 ms later.
This is a warning against assuming that 50 ms aggregation alone fixes video
lag. It may reduce real CPU work under high capture frequency, which the
physical test must verify. Continuous upper motion eventually yields to the
lower menu in the model, and all workloads converge after motion stops.

## Signed controls and diagnostics

The candidate BuildLabel is `0.2.1A — 50 ms latest-state aggregator / Fixed
250 KiB/s`; the bundle versions are `0.2.1` and the bundle ID remains
`io.freerdp.shadow.sonoma.menu`. The signed config sets 250 KiB/s, default
SO_SNDBUF, ordinary/F scheduling, burst off, coverage off, and no adaptive
probing. The only intended processing change is the 50 ms publication gate.
Pacing diagnostics are enabled to observe it, every five seconds rather than
per tile. Logs report capture events, publications, coalesced/suppressed
events, window/pending area, cadence, publication ID, pending tile count,
pending age upper bound, repeated pending tile stages, fixed rate, and socket
queue. Restaged pending tiles count work present at each new stage; they are a
churn proxy, not exact obsolete bytes. The pending age upper bound can include
newer pixels replacing an older pending target.

The candidate is packaged at `dist/FreeRDP Shadow 0.2.1A.app`; its adjacent
`.source.patch` and `.sha256.txt` files record the exact local source and
artifact hashes. The packager verifies Release `-O3 -DNDEBUG` and certificate
signing. No version bump, commit, tag, push, release, installation, or restart
is part of this engineering stage.

## Physical comparison at the Mac local console

Use the same phone, SSH route, resolution, and content. First observe the
currently installed 0.2.0 with (1) one standard large-window promotion and
(2) continuous playback of a reproducible video. During playback, move the
Mac pointer, open/close a menu, and move an icon/window locally. Note large
refresh duration/bars/convergence; menu/icon arrival delay; growing visual
lag; stale catch-up after stopping video; and session stability.

With local-console access maintained, install this candidate using one command:

```zsh
./scripts/swap-macos-shadow-aggregation.sh candidate
```

The helper quits the active menu app, preserves one signed installed 0.2.0
backup at `dist/FreeRDP Shadow installed-0.2.0.backup.app`, checks signatures,
copies the candidate into the normal installed path, and opens it. The RDP
session will disconnect; reconnect the same phone through SSH. Repeat both
workloads with the same video. Record the same observations and whether an
unrelated menu/icon becomes current promptly while the video remains choppy.
The first-frame/static screen must converge completely and input/clipboard/
audio must remain usable. Stop and roll back if queues grow, the session
becomes stale, static damage remains, or the lower region starves.

Rollback command at the local console:

```zsh
./scripts/swap-macos-shadow-aggregation.sh rollback
```

For concise candidate server logs, start this in a second local terminal just
before each workload and stop it with Ctrl-C afterward:

```zsh
tail -n 0 -F ~/Library/Logs/FreeRDPShadow/server.log | rg --line-buffered 'Mac publication aggregate:|Mac bitmap pacer:|Bounded newest-state bitmap scheduler|Failed to publish' | tee /tmp/FreeRDP-0.2.1A-video.log
```

The candidate must be judged on client-presented freshness, especially the
menu/icon during motion and the stopped-motion tail. The static large-window
test guards against accepting a candidate that simply delays all graphics.
If physical CPU/staging savings are small, the 50 ms gate should not become the
engineering baseline. A shorter interval would reduce the added delay, but
there is no evidence here to choose one; any later interval requires its own
controlled test.
