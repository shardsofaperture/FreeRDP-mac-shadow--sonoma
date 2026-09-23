# Post-0.1.9 accepted-socket SO_SNDBUF experiment (2026-09-22)

## Scope and socket ownership

This separately packaged Mac shadow candidate adds an opt-in send-buffer cap
to its **accepted TCP client socket**. No generic FreeRDP socket default,
installed 0.1.9 app, SSH setting, or production listener is changed. This is
an experiment; no cap is a recommended default without physical evidence.

`libfreerdp/core/listener.c:freerdp_listener_check_fds()` calls `_accept`, then
`freerdp_check_and_create_client()` passes the descriptor to
`freerdp_peer_new()`. `freerdp_peer_new()` stores it in `peer->sockfd`.
`shadow_client_accepted()` runs before `freerdp_peer_context_new_ex()`;
the latter calls `freerdp_peer_transport_setup()`, which passes the descriptor
to `transport_attach()`, then sets `peer->sockfd=-1`. The transport's socket
BIO subsequently owns and closes it. The Mac shadow callback therefore
validates the environment value and uses `setsockopt(SO_SNDBUF)` **before**
that transfer. It duplicates the accepted socket for per-client `SO_NWRITE`
and `SO_SNDBUF` reads after transfer, marks the duplicate close-on-exec, and
closes it at client teardown. The transport keeps exclusive ownership of
its original descriptor. AF_UNIX/VSOCK clients are not capped.

This audit also found that the preceding experimental pacer queried
`peer->sockfd` after the ownership transfer. That value was `-1`, so it
could not have sampled the accepted socket. The candidate corrects this
lookup through its owned duplicate for all Mac TCP arms, including the
uncapped control. Earlier pacing
models assumed a valid queue signal and remain models, not measurements of
that previous binary's actual feedback.

`FREERDP_MAC_SHADOW_SO_SNDBUF_KIB` accepts exactly `0`, `4`, `8`, `16`, `32`,
`64`, or `128`. Unset or `0` leaves the socket default untouched. A malformed
or unsupported value rejects the new experimental connection with an error
rather than silently running the wrong arm. A nonzero cap is only applied
to accepted AF_INET/AF_INET6 sockets. The existing bitmap pacer remains
active and otherwise unchanged. The existing opt-in
`FREERDP_MAC_SHADOW_PACING_DIAGNOSTICS=1` produces one summary every five
seconds with `SO_NWRITE`, BIO blocked duration, graphics bytes, operations,
pauses, estimated rate, pending tiles, and requested, initial, and current
`SO_SNDBUF`. An explicitly configured cap also logs request and readback
once on acceptance. No pixels, keystrokes, or clipboard data are logged.

## Darwin socket probe

On this x86_64 Sonoma host, an isolated accepted `127.0.0.1` TCP connection
inherited 146,988 bytes. `setsockopt` and immediate `getsockopt` produced:

| Requested | Immediate readback | After 1 MiB sent and 250 ms |
| ---: | ---: | ---: |
| 4 KiB | 4,096 B | not probed after transfer |
| 8 KiB | 8,192 B | not probed after transfer |
| 16 KiB | 16,384 B | 16,384 B |
| 32 KiB | 32,768 B | 32,768 B |
| 64 KiB | 65,536 B | 65,536 B |
| 128 KiB | 131,072 B | 131,072 B |

No doubling, rounding, or change after this short transfer was observed.
That does not establish behavior during an aged session or under prolonged
congestion; the five-second readback in the candidate checks for later
autotuning or clamping. The socket regression additionally tests parsing,
default/no-set behavior, and readback using a local stream socketpair.

The 0.2.0E focused accepted-loopback regression additionally reads back
4,096 and 8,192 bytes exactly on this Darwin host. The 8 KiB allowlist entry
is the only pacing-related change in E; it retains the existing adaptive
controller.

## 0.2.0E-I follow-on matrix

The signed follow-on packages separate accepted-socket capacity from fixed
pre-serialization bitmap admission:

| Arm | Accepted `SO_SNDBUF` | Bitmap pacing | Maximum credit |
| --- | ---: | --- | ---: |
| E | 8 KiB | existing adaptive controller | adaptive |
| F | system default | fixed 150 KiB/s | 16,478 B |
| G | 32 KiB | fixed 150 KiB/s | 16,478 B |
| H | system default | fixed 250 KiB/s | 25,600 B |
| I | 32 KiB | fixed 250 KiB/s | 25,600 B |
| J | system default | fixed 175 KiB/s | 17,920 B |
| K | system default | fixed 200 KiB/s | 20,480 B |
| L | system default | fixed 150 KiB/s plus bounded large-refresh burst | 16,478 B base / 32 KiB burst |
| M | system default | fixed 150 KiB/s plus diagnostic large-refresh burst | 16,478 B base / 32 KiB burst |

F/G and H/I are matched pairs; only their signed socket setting differs.
J/K interpolate between the physically preferred F rate and the higher H
rate without changing the fixed-rate implementation or socket behavior.
L retains F's sustained behavior and adds one opt-in experiment driven by the
exact area of the new publication's disjoint invalid rectangles. Damage of at
least 25% of the desktop can enter 300 KiB/s mode for at most 150 ms and at
most 48 KiB of admitted graphics. Entry grants no credit. A burst is followed
by a 1,000 ms baseline-only cooldown, so repeated broad damage cannot extend a
burst or keep the connection at the higher rate.
M keeps the same fresh-publication 25% trigger and F's base policy. Its opt-in
window is 600 KiB/s for at most 500 ms, with a separate 256 KiB cap on admitted
pre-serialization graphics bytes. The next entry is eligible only 1,000 ms
after the nominal window end. Entry grants no credit, idle time inside the
window earns no burst-rate credit, and an operation that cannot fit the
remaining burst bytes is deferred until the time bound or replaced by a newer
publication. M is a diagnostic arm; physical F/M comparison is pending.

**September 22 erratum:** The sentence above records the design intent at
the time of that note. The physical comparison was subsequently performed:
M looked the same or worse than F. A later source review found that the
custom bitmap branch used `client->invalidRegion` for its trigger and returned
before merging `surface->invalidRegion`; normal capture publications could
therefore stage new pixels with a zero-area trigger. The M log proves only
the initial full refresh qualified, not the later window promotion. The
0.2.0-rc3 source uses the published surface region for the trigger and keeps
the client refresh region exclusively for cache invalidation. See
[the RC note](remote-endpoint.invalid). Historical signed F/L/M bundles are
preserved unchanged.
Fixed mode refills byte credit from monotonic wall-clock time, never changes
its configured rate in response to queue samples, and caps idle accumulation.
The 250 KiB/s bucket is 100 ms of rate. The 150 KiB/s bucket has a small
1,118-byte floor above 100 ms so one maximum 64x64 planar bitmap plus framing
can fit atomically; this is about 107 ms and remains far below a multi-second
burst. Admission is checked after compression determines the operation size
but before `BitmapUpdateProxy`/`ScrBlt` serializes it. Actual FreeRDP transport
byte deltas are charged afterward. While credit is unavailable, the existing
single latest-state bitmap store remains replaceable; no encoded-PDU FIFO is
introduced. Input and channels continue to be serviced before and after the
bounded graphics pass.

## Deterministic model

`TestShadowPacer` now models application submissions into a buffered BIO,
an accepted socket with finite capacity, sshd's separate 648 KiB local
reservoir, and a 150 KiB/s downstream drain. The control uses the 146,988 B
accepted-socket default measured above. Motion generates one 4 KiB newest
tile per 10 ms for 30 seconds. All queued bytes below are **serialized bytes
downstream of replaceable newest-state storage**; the latency column divides
the peak combined backlog by 150 KiB/s. Local socket pressure is observed
after sshd has had a chance to read it each tick.

| Arm | Peak socket | Peak sshd hidden | Peak BIO | Peak combined | Queued time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Default (146,988 B) | 67,584 B | 663,552 B | 0 B | 731,136 B | 4.76 s |
| 128 KiB | 67,584 B | 663,552 B | 0 B | 731,136 B | 4.76 s |
| 64 KiB | 65,536 B | 663,552 B | 2,048 B | 731,136 B | 4.76 s |
| 32 KiB | 32,768 B | 663,552 B | 4,096 B | 700,416 B | 4.56 s |
| 16 KiB | 16,384 B | 663,552 B | 4,096 B | 684,032 B | 4.45 s |

The 648 KiB hidden reservoir filled in **every** arm. The model therefore
does not support the hypothesis that a smaller accepted socket alone prevents
sshd from holding hundreds of kilobytes. The older 648,000 B-forwarder
model peaked at 716,260 B; the new default peaks at 731,136 B because it
uses 648 * 1024 B and explicitly accounts for BIO/socket capacity. These
are deliberately similar scenarios, not identical measurements.

A 20 MB/s path with one 4 KiB tile generated each 10 ms submitted 384,614 B/s
over its first ten seconds for **every** cap, versus 409,600 B/s generated.
No BIO-blocked tick or rate decrease was observed; the roughly 6% early
shortfall comes from the shared conservative rate ramp in this model.
A 2 MiB static refresh on that path finished submitting at 3,306 ms and
drained at 3,308 ms in **every** arm. The pacer's conservative startup and
rate ramp dominate this modeled refresh. The model assumes instantaneous
local forwarding up to capacity each tick; it does not simulate kernel
wakeups, syscall overhead, scheduling jitter, TCP autotuning, SSH encryption,
or Android presentation. Those remain physical checks.

## Signed candidates and physical procedure — prepared, not run

Package each arm without executing its CLI:

```zsh
for ARM in E F G H I J K L; do
  python3 scripts/build-macos-shadow-app.py --experimental-arm "$ARM"
done
```

The resulting apps have distinct paths but retain the same signed bundle/TCC
identity. Their signed `ShadowConfig.plist` selects the socket cap and fixed
rate; physical testing must not override these through an interactive shell.
Use only the established `127.0.0.1:3390` SSH forward. For one arm at a time,
cleanly quit the prior menu app, verify that no process still listens on 3390,
then launch the exact candidate path:

```zsh
ARM=F
APP="$PWD/dist/FreeRDP Shadow 0.2.0${ARM}.app"
codesign --verify --deep --strict --verbose=2 "$APP"
lsof -nP -iTCP:3390 -sTCP:LISTEN
open -n "$APP"
```

Repeat the relevant sequence one arm at a time. F/G and H/I must use identical client,
display, SSH route, workload, duration, and measurement method. Identify the
FreeRDP-to-sshd loopback pair and the public SSH socket with `lsof`; set
`PUBLIC_IP` to that observed remote address and collect timestamped queue
samples for each arm:

```zsh
PUBLIC_IP=REPLACE_WITH_OBSERVED_PHONE_PUBLIC_IP
ARM=F
for ((i=0; i<600; i++)); do
  date -u '+%Y-%m-%dT%H:%M:%SZ'
  netstat -anv -p tcp | rg "127\\.0\\.0\\.1\\.3390|${PUBLIC_IP}"
  sleep 0.2
done > "/tmp/mac-shadow-0.2.0${ARM}-queues.log"
```

Keep the same 120/240-fps video method in every arm: show the contact or
input event and first visible Android response. Measure median, p95, max,
Dock stabilization, dragging, scrolling, menu response, stopped-motion
recovery, and apparent useful update rate. Include an initial/reconnect
2 MiB-class static refresh and measure time to a complete stable desktop.
Repeat sustained motion for at least two minutes and after an aged session.
Record separately FreeRDP Send-Q, sshd local Recv-Q, public SSH Send-Q,
five-second pacer summaries, CPU, and any corruption. The three queues must
not be collapsed into a single socket metric.

Do not select a default from model results. A candidate arm must lower the
**combined** FreeRDP+sshd backlog and video input-to-visible delay without
multi-second stale frames or a severe full-refresh penalty. Confirm input,
clipboard, audio, first frame, static reconnect, and display restoration;
then check Win98 1024×768/16-bit and the exact Mac RDC profile before any
policy consideration. If all caps leave sshd's hidden queue large, conclude
that this transport experiment did not solve visible latency.
