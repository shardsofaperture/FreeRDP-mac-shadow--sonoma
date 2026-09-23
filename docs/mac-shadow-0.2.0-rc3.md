# Mac Shadow 0.2.0-rc3 engineering candidate (2026-09-22)

This is an uncommitted candidate from `master` at
`c72de69d5066a5a48346385484f3becf8f424483`. The signed 0.1.9
recovery bundle and 0.2.0F physical reference remain untouched. The
installed 127.0.0.1:3390 server was not replaced for this work. Android,
Win98, Mac RDC, SSH/WAN, aged-session, audio, display restoration, and
client-visible latency acceptance remain physical checks.

## Verified defects and repair

The Mac publisher stores current capture damage in
`server->surface->invalidRegion` under `surface->lock`. The custom bitmap
branch previously staged current pixels but used `client->invalidRegion`
for M/L burst qualification, then returned before the generic union of the
published surface region. Client invalidRegion is a forced refresh/cache
invalidation signal, so normal capture could stage new pixels with a zero
trigger. The historical M log shows one initial 100% burst and subsequent
staged work with zero recorded trigger area; it does not identify a later
window promotion. `shadow_publication_stage` now stages under the same
surface lock, measures the clipped published region after successful staging,
and passes only the client refresh region to `shadow_bitmap_stage`. The
publisher may reduce changed pixels to a bounding rectangle; the metric is
published-region area, not exact changed-pixel count.

M could also remain active with insufficient residual admitted-byte
allowance for the next atomic operation. The pacer now ends that allowance
with `unusable-remainder`, retains the nominal-end cooldown, clamps to
the 150 KiB/s bucket, and retries the same operation under base rules.
Cheap preflight no longer reserves cap bytes. A remembered large credit
wait is canceled only when nonempty published damage or client refresh
replaces the deferred state; an empty update event retains the cheap wait.
A final admission checks
monotonic time after compression, so a burst that expires during encode
uses base rules. An operation admitted before expiry can finish afterward,
then is charged once.

The published-area trigger can now fire repeatedly during continuous
motion. After a burst, another qualifying large publication requires two
seconds without a qualifying large publication as well as the nominal
cooldown. Entry is denied on an observed local queue at least as large as
the base credit bucket, and an active burst ends if that queue reaches
64 KiB. Time and byte cap also bound the policy.
These are local socket safeguards; sshd/WAN and client queues remain
unobserved. The policy stays at system-default SO_SNDBUF, fixed
153,600 B/s base, 614,400 B/s burst, 500 ms, 262,144 admitted bytes,
1,000 ms nominal cooldown, 2,000 ms quiet rearm, and a 25%
published-area threshold. Tile order,
codecs, resolution, and SSH policy are unchanged.

## Regression and synthetic evidence

Before the region fix, `TestShadowPublicationPacing` failed with empty
client refresh and a half-desktop published region; after the fix it passes.
It uses the production staging/qualification and commit functions with
mock pixels/transport. It checks cache preservation, explicit identical-pixel
refresh, clipped/overlapping region area, stage failure, an empty event that
preserves a deferred credit wait, unsent replacement,
lower-screen progress, and both 16/32-bit client reconstruction.
`TestShadowDiagnosticM` now checks the 260,000-byte non-divisible cap
case, 2,048-byte preflight, tiny order, exact cap, deadline, superseded
16,000-byte wait, blocked writes, local queue guard, prolonged motion,
and quiet rearm.
The existing bitmap test covers protocol sizing, overlapping copy, and
newest-state behavior. The prior M fairness test increments synthetic
input/channel counters itself; it is not a test of the real service loop.

Release/json-c targeted suite: 18/18 passed after the socket-cap test ran
with its test-owned ephemeral loopback bind permitted. The first sandboxed
suite invocation passed 17/18; its socket test could not bind in the network
sandbox. All required CMake FILEPATH entries in the Release test cache
existed. No environment dependency was changed. `git diff --check`
passed. The service loop still processes input before bitmap work and uses
bounded channel slices; a client/WAN acceptance test is outstanding.

The deterministic workload exercises real staging, tile choice, 16-bit
interleaved/32-bit planar compression, preflight, admission, and commit.
The clock, successful transport, and 1 ms service tick are simulated;
charged bytes equal the admission estimate. The same patterned surface was
used for F policy and this candidate in each depth:

| Surface | Policy | Bytes | First / last submission ms | Complete ms | Dirty tiles | Deferrals |
|---|---|---:|---:|---:|---:|---:|
| 1920×1080×32 | F | 348,076 | 1000 / 3170 | 3171 | 312 | 2170 |
| 1920×1080×32 | candidate | 348,076 | 1000 / 1958 | 1959 | 312 | 958 |
| 1024×768×16 | F | 896,376 | 1000 / 6729 | 6730 | 120 | 5729 |
| 1024×768×16 | candidate | 896,376 | 1000 / 5541 | 5542 | 120 | 4541 |

Both runs reconstructed every pixel. These numbers demonstrate a
server-side admission difference in a zero-queue model. They do not measure
socket, SSH, Android decode, or client display time.

## Package and provenance

Build with
`python3 scripts/build-macos-shadow-app.py --release-candidate --build-dir build-macos-shadow-rc3-package`.
The builder chooses the next unused RC app path under `dist/`, keeps
the required Apple Development signature and bundle ID, and verifies deep
strict signing. The earlier signed RC1 and RC2 builds are retained but
superseded by the stronger production-wired surface-region and empty-event
regressions; they are not physical-test candidates. The selected path is
`dist/FreeRDP Shadow 0.2.0-rc3.app`. Its BuildLabel identifies the
published-region quiet-rearmed burst and fixed base rate. Signed
`Contents/Resources/SourceProvenance.json` records HEAD, branch, dirty
status, source patch SHA-256, build configuration, and policy. Adjacent
`.source.patch` and `.sha256.txt` sidecars retain exact source and
artifact hashes. Confirm the actual selected RC number and plist values
before any physical swap.

## Physical acceptance

Only a user-authorized manual test can decide whether window promotion
is faster and still responsive. Let the desktop settle for at least two
seconds after startup or another large change. Verify the installed BuildLabel and
signature before launch. Capture a post-startup qualifying
`M burst start: publicationId=...` for the actual window promotion,
then a bounded `M burst end` and periodic queue/deferral summaries.
Record wall-clock visible completion, number of visible bands, mouse/menu
response during the refresh, sustained movement for at least two minutes,
and stopped-motion convergence. The server log reports send admission,
not client presentation. Exercise Android first, then Win98
1024×768×16, Mac RDC, first frame, static reconnect, clipboard, audio,
input/modifier release, and display restoration before acceptance.
