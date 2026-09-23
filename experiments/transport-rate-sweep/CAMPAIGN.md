# Physical fixed-rate transport campaign — 2026-09-23

The same phone RDP client performed a foreground large-window promotion
through SSH at each selected rate. At 300 KiB/s, the user also switched the
window back; that capture therefore includes a second large graphics action.
The 150 KiB/s capture was stopped
manually; the 200–400 KiB/s captures stopped automatically after observed
graphics traffic and queue recovery. The signed RateSweep app did not change
between rates. Each run has raw `nettop`, socket-queue samples, relevant server
logs, offline analysis, exact [metrics](campaign-metrics.json), and separate
client-visible operator notes.

## Comparison

Transport rates below are **KiB/s**, in mean / median / p95 / maximum order,
over the whole capture, including connected idle and background traffic.
Queue triplets are FreeRDP Send-Q / sshd loopback Recv-Q / sshd external Send-Q,
in KiB. The external/input ratio uses paired one-second nettop samples.
"Drain" is seconds from the largest sampled queue peak to the first of six
consecutive half-second observations with **all three queues at or below 32
KiB**. Zero means the peak was already within that band. This recovery marker
is distinct from the older parser's strict <=2 KiB marker.

| Requested | RDP→sshd plaintext mean / median / p95 / max | sshd→phone encrypted mean / median / p95 / max | External / input | Max queues | Queues at detected high-rate end | Drain | RTT mean / max ms | Material gap intervals; raw `re-tx` / nonzero samples | Client observation |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 150 | 69.6 / 39.8 / 151.4 / 152.5 | 72.8 / 46.9 / 154.2 / 162.5 | 1.046 | 14.3 / 11.1 / 27.4 | n/a | 0 | 89.6 / 135.8 | 0; 4240 / 2 | ~8 black bars; 6–8 s; responsiveness unreported |
| 200 | 97.8 / 91.7 / 200.7 / 228.1 | 102.5 / 98.6 / 206.0 / 208.4 | 1.048 | 16.9 / 31.3 / 29.0 | 8.5 / 5.3 / 11.7 | 0 | 83.5 / 97.8 | 0; 2728 / 1 | 13 bars; under 5 s estimated; icons/menus good |
| 250 | 132.0 / 117.3 / 272.9 / 290.6 | 131.6 / 128.1 / 255.8 / 262.4 | 0.985 | 17.7 / 37.6 / 29.6 | 0 / 0 / 19.5 | 0.51 | 85.8 / 107.1 | 2; 0 / 0 | Fewer bars; ~3 s; icons fast and smooth |
| 300 | 129.7 / 70.3 / 330.8 / 348.2 | 123.2 / 74.0 / 322.6 / 337.9 | 0.955 | 27.9 / 278.7 / 30.0 | 0 / 0 / 8.3 | 3.56 | 80.1 / 96.2 | 11; 1452 / 3 | 2–3 s; 2 large bars first switch, ~6–7 back; icons perhaps slightly slower |
| 400 | 140.7 / 81.5 / 446.3 / 454.2 | 132.0 / 145.5 / 293.5 / 322.8 | 0.938 | 40.1 / 314.8 / 29.3 | 0 / 314.8 / 0 | 7.64 | 87.7 / 102.1 | 13; 1364 / 1 | ~3 s; 5–7 bars; no visible gain, same phone responsiveness |

At capture end, the queue triplets were respectively 0 / 0 / 3.1,
0 / 0 / 4.1, 0 / 9.0 / 19.6, 0 / 0 / 3.4, and 0 / 0 / 4.2 KiB. The 150 run
has no detected high-rate phase metadata because the first manual harness did
not mark it. The 400 run's active-end queue was still 314.8 KiB in sshd's
loopback Recv-Q, then recovered before auto-stop. The strict <=2 KiB drain
marker was not observed at 250 or 400; the 32 KiB recovery figures above use
the preserved raw queue series and require a sustained settled interval.

The 300 run is the **first material queue-pressure signal**: sshd loopback
Recv-Q peaked at 285,412 B, spent ten half-second samples above 64 KiB and
seven above 128 KiB, then drained. The **first pressure persisting through the
detected high-rate phase end** is 400: the same queue peaked at 322,378 B,
was above 64 KiB in 32 half-second samples (longest continuous stretch 11)
and above 128 KiB in 19 (longest stretch 8). It was still high at the
detected high-rate phase end. No rate left a persistent queue after
capture. FreeRDP's own and sshd's external Send-Q remained far smaller.

The clean, low-queue region measured here is **150–250 KiB/s**. The highest
tested rate in that region, and the recommendation for the next engineering
baseline, is **250 KiB/s**. Its client-reported refresh was around three
seconds with responsive icons, while queue recovery stayed prompt. 300 had a
small possible visual improvement with a short, large loopback backlog; 400
had no reported visual gain and more sustained backlog. The evidence points to
the SSH forwarding/downstream consumption path becoming the measured queue
limit before higher pacing produces a clear client-visible benefit. The data
cannot separate VPN/network capacity from phone decoding or presentation:
there are no client-side receive, decode, or display timestamps. It also
cannot establish a general throughput ceiling from these few actions. The
extra return switch at 300 and background traffic limit direct comparison of
capture-wide averages and queue durations.

The external encrypted/plaintext ratio is **not** a packet-loss measure:
compression, SSH overhead, different sampling windows, and queue movement
all affect it. "Material gap" counts aligned one-second intervals where RDP
plaintext input exceeded encrypted SSH output by both 16,384 B/s and 20% of
output. The raw macOS nettop `re-tx` field had nonzero samples, but its units
and semantics are unconfirmed; the values are retained without interpreting
them as packet retransmission counts. Physical durations are user estimates,
not server-derived display latency. The 300 follow-up phone text was entered
after AUTO-STOP and did not affect that capture.

## Controls and evidence

The static signed config retained F/normal fixed pacing, disabled coverage
scheduling, no RC3/M burst or adaptive probing, system-default SO_SNDBUF, and
the `127.0.0.1:3390` listener. Each rate was selected only through the menu's
`--rate-sweep-kib` argument. The run logs recorded `mode=fixed`,
`adaptiveProbing=disabled`, `burstRate=0`, `burstEntries=0`, and
`sndbufRequest=0`, with base rates 153600, 204800, 256000, 307200, and
409600 B/s. The same phone, SSH route, and foreground-window action were used.
No FreeRDP binary or signed bundle changed during the physical sweep.

| Rate | Preserved run |
| ---: | --- |
| 150 | [20260923-071111-150KiB](runs/20260923-071111-150KiB/) |
| 200 | [20260923-073051-200KiB](runs/20260923-073051-200KiB/) |
| 250 | [20260923-073259-250KiB](runs/20260923-073259-250KiB/) |
| 300 | [20260923-073511-300KiB](runs/20260923-073511-300KiB/) |
| 400 | [20260923-073814-400KiB](runs/20260923-073814-400KiB/) |

Two earlier 200 KiB/s capture directories, `20260923-072633-200KiB` and
`20260923-072827-200KiB`, ended **before READY** during harness correction.
No physical promotion was requested or counted in either. They remain
preserved for audit. The successful 200–400 runs used bounded telemetry
auto-stop; the harness and offline parser changed, not the tested FreeRDP
behavior. Exact numeric definitions and all per-rate fields are in
[`campaign-metrics.json`](campaign-metrics.json), regenerated by
[`campaign_metrics.py`](campaign_metrics.py).
