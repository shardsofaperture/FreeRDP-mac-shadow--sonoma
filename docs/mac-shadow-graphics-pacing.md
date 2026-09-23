# Post-0.1.9 Mac bitmap pacing candidate (2026-09-22)

**Later socket-ownership correction:** The initial note below treats
`freerdp_peer::sockfd` as retained during the session. It is actually set
to `-1` when `freerdp_peer_context_new_ex()` transfers the accepted socket
to the transport BIO. Thus the earlier pacer code could not read the live
`SO_NWRITE` from that field. The separate
[socket-cap experiment](mac-shadow-socket-cap-experiment.md) captures a
borrowed descriptor before transfer, fixes the diagnostic lookup, and models
finite socket and sshd queues. The earlier model remains historical evidence
about the observability limit, not a measurement of live feedback in that
binary.

## Output-path audit

The optimized Mac path stages the newest pixels in `shadow_bitmap_stage`, then
`shadow_bitmap_next` selects an unsent tile or valid ScrBlt copy. The client
loop compresses a 16-bit interleaved or 32-bit planar tile, calls
`BitmapUpdateProxy` (or BeginPaint/ScrBlt/EndPaint), and commits the tile only
after the update callback succeeds. `update_send_bitmap_update` writes the
bitmap into a fast-path PDU and forces a flush; the fast-path and RDP send path
call `transport_write`. `transport_default_write` writes to a buffered BIO
whose ring buffer starts at 64 KiB and can grow. That BIO writes to the
accepted TCP socket. With SSH forwarding, the kernel socket send buffer, the
sshd local receive socket/process, the public SSH socket, network, and client
all follow in order. None of those later bytes can be replaced by newer pixels.

`IsWriteBlocked` is `BIO_write_blocked(frontBio)`: the buffered BIO sets this
only after a downstream BIO write returns a retryable would-block condition.
It is **not** a test for an empty TCP send queue. Before it becomes true,
FreeRDP may have accepted bytes in its BIO and the kernel may have accepted
hundreds of kilobytes; sshd may also have already read and buffered data. The
BIO ring's 64 KiB initial allocation is not a maximum. `DrainOutputBuffer`
calls `BIO_flush` only when the buffered BIO reports blocked and returns
whether it remains blocked. The current loop uses this check but can continue
serializing 16 KiB every pass while sshd is still absorbing bytes locally.

`freerdp_peer::sockfd` initially holds the accepted descriptor, but becomes
`-1` when context setup attaches that socket to the transport BIO. This
earlier candidate therefore did not query the live socket. The later
socket-cap experiment duplicates the descriptor before transfer. Darwin
defines `SO_NWRITE` as the
number of bytes currently in that socket's send buffer. A successful query
provides local pressure feedback; zero does **not** mean sshd has delivered
data over its public socket or that the RDP client has displayed it. A failed
query leaves conservative time-based pacing active without upward probing.
No generic transport default or socket
buffer size changes are involved.

The last replaceable state is the staged, uncommitted bitmap state before
`BitmapUpdate` or ScrBlt serialization. The pacer never drops serialized PDUs.
Its accounting reads `freerdp_get_stats(..., outBytes, ...)` around each
successful tile submission, so the controller charges the bytes passed to the
transport rather than the compressed-payload estimate. The estimate plus
framing headroom only gates the tile before serialization. Concurrent channel
output in the same interval would be conservatively charged to graphics. A
single tile may exceed available credit by its framing difference; that debt is repaid by the
next time refill. Existing negotiated packet checks remain in place.

## Controller

Each Mac client has a byte credit and a rate estimate. It starts at 150 KB/s
with a 16 KiB credit. Monotonic elapsed time refills credit, capped to a
150 ms burst and an absolute 64 KiB. A successful bitmap or ScrBlt consumes
its exact submitted bytes. With pending damage and no observed pressure, the
rate probes upward by 50% every 500 ms while credit is being consumed, to a
50 MB/s ceiling. No idle-desktop ramp or periodic graphics are generated. A positive `SO_NWRITE` above a
rate-scaled high threshold, or `IsWriteBlocked`, pauses new graphics and
reduces the rate by 35% per 100 ms until the low threshold clears. A measured
positive-queue drain can lower the rate sooner; this sample is only a local
socket consumption estimate. The high/low thresholds have 12/4 KiB floors
and 64/16 KiB ceilings. Input, capture publication, and channel servicing
continue during a graphics pause. New captures replace uncommitted work.

This is an asymmetric AIMD controller with a token bucket and local queue
hysteresis. It retains the old 8 ms, eight-tile, and 16 KiB per-pass work
bounds for input fairness. The controller does not use a fixed sleep/byte cap
as its steady-state policy.

There is a fundamental observability limit: a forwarding process can accept
an arbitrarily large amount while the local socket remains empty. In that
period, this server has no acknowledgement of remote graphics delivery. The
conservative start and bounded rate probes reduce blind overshoot, but cannot
guarantee a 100–250 ms WAN graphics queue. A true bound would require client
presentation/consumption feedback or explicit control of the forwarder's
queue. A LAN starts conservatively and needs roughly 2 seconds in the model
to reach a 400 KB/s generated stream; this is the startup tradeoff.

Alternatives rejected for this candidate: a fixed N KiB/s throttle caps a
healthy LAN; `IsWriteBlocked` alone fires too late; `SO_NWRITE == 0` as a
delivery acknowledgement is false through SSH; changing global TCP buffers
alters unrelated FreeRDP behavior; inspecting sshd process internals would
couple production code to a particular tunnel. The initial 150 KB/s is a
measured-path prior, not a claim that every path has that rate.

## Deterministic model and diagnostics

`TestShadowPacer` injects time, submitted bytes, queue samples, blocked state,
and a forwarding buffer with a separate simulated WAN drain. It covers
150 KB/s sustained motion, a 20 MB/s path, sudden degradation, recovery,
static desktop, and the fact that local queue zero cannot refund submitted
credit. A second slow-path model uses a 648 KiB hidden forwarding reservoir,
similar to the observed sshd Recv-Q: it reaches about 716 KiB despite an
empty local socket during startup. This explicitly fails the 100–250 ms
objective and requires physical validation or a stronger feedback signal.
`TestShadowBitmap` separately checks newest-state replacement, damage,
and copy correctness. The model does not decode RDP or measure client display.

With one 4 KiB latest-state tile generated every 10 ms and a 150 KB/s WAN
drain, the unpaced comparison accumulates about 15.6 MB in 60 seconds. The
pacer model submits about 150 KB/s after startup and peaks at 118 KB total
queue with a 64 KiB hidden forwarder; its late maximum is 115 KB, or about
770 ms at 150 KB/s. The 648 KiB hidden-forwarder case peaks at 716 KB, or
about 4.8 seconds. A 20 MB/s path carrying the same 400 KB/s generated load
reaches 506 KB/s controller rate and submits 385 KB/s over the first 10
seconds. On sudden degradation, the 64 KiB-forwarder model peaks at 133 KB,
then backs down to about 203 KB/s; after recovery it submits about 401 KB/s
over the next 10 seconds. A 2 MiB static full refresh on a fast path takes
3.32 seconds to submit from a cold controller; this may be an unacceptable
first-frame cost. These are model results, not hardware measurements.

An isolated accepted loopback TCP socket returned `SO_NWRITE=0` when empty
and 179,964 bytes after 245,292 bytes were submitted without a reader. This
confirms the Darwin query operates on the intended socket kind; it does not
validate the SSH/WAN inference.

Set `FREERDP_MAC_SHADOW_PACING_DIAGNOSTICS=1` **only for a future isolated
hardware run** to emit one summary every five seconds: submitted graphics
bytes and operations, pause count, current and interval-maximum `SO_NWRITE`
(or unsupported), write-blocked milliseconds, pacing and measured local-drain
rates, staged publications, and pending dirty tiles. No content
or keystrokes are logged. Summaries are absent by default. They are server
metrics, not end-to-end latency.

## Physical A/B handoff (not run)

1. Keep installed 0.1.9 untouched. Record its app version, executable path,
   signature, running PID, client capabilities/profile, and existing SSH
   forwarding endpoints. Use Android `aFreeRDP-14a80a` at 1920×1080, 32-bit,
   classic bitmap/newest-state, captured cursor for both arms. Keep the same
   phone, network route, Mac display, content, and SSH configuration.
2. Baseline on the current 0.1.9 connection: record high-frame-rate video
   showing a local click/input marker and the Android visible response. Do
   Dock magnification, window dragging, scrolling, clicking/menu response,
   and at least two minutes of sustained motion; repeat after an aged session.
   Record median, p95, and maximum input-to-visible time, useful update rate,
   stopped-motion recovery, and visible corruption.
3. For the candidate, use a separately signed experimental app/CLI from this
   checkout only after the operator explicitly authorizes a hardware run.
   Listen on `127.0.0.1:3391`, forward that loopback port through the same
   SSH/WAN route, and enable the diagnostic environment variable above. Do
   not take over port 3390 or restart the installed server. Verify the
   candidate's actual executable, version, negotiated 32-bit classic bitmap
   path, and socket endpoints before measuring. Repeat step 2.

   The separate package and candidate command for that future run are:

   ```zsh
   python3 scripts/build-macos-shadow-app.py \
     --build-dir build-macos-shadow-pacer-package \
     --output 'dist/FreeRDP Shadow Pacer.app'
   FREERDP_MAC_SHADOW_AUTO_CLIENT_PROFILE=1 \
   FREERDP_MAC_SHADOW_PACING_DIAGNOSTICS=1 \
     'dist/FreeRDP Shadow Pacer.app/Contents/MacOS/freerdp-shadow-cli' \
     /bind-address:127.0.0.1 /port:3391 /sec:rdp /max-connections:1 \
     -auth -gfx -rfx -nsc 2>&1 | tee /tmp/mac-shadow-pacer-candidate.log
   ```

   On the phone, create a separate SSH local forward to Mac
   `127.0.0.1:3391` using the same SSH host/account and WAN connection class,
   then point aFreeRDP at that forwarded local port. The package command
   performs a `/version` smoke check; it was **not** run for this candidate.
4. During each arm, use `lsof -nP -iTCP -sTCP:ESTABLISHED` to identify the
   FreeRDP-to-sshd local pair and the corresponding public `sshd` connection.
   Sample `netstat -anv -p tcp` at about 200 ms intervals and retain the rows
   for both directions of the local pair and the public SSH pair. The server
   row's Send-Q, sshd local row's Recv-Q, and public SSH row's Send-Q are
   distinct reservoirs. Save timestamps with every sample, along with the
   five-second pacing summaries. Avoid logging desktop pixels or input text.

   For the currently observed phone public IP, one capture command is:

   ```zsh
   lsof -nP -iTCP -sTCP:ESTABLISHED | rg 'freerdp-shadow-cli|sshd'
   while :; do
     date -u '+%Y-%m-%dT%H:%M:%SZ'
     netstat -anv -p tcp | rg '127\.0\.0\.1\.(3390|3391)|45\.134\.140\.153'
     sleep 0.2
   done > /tmp/mac-shadow-pacer-queues.log
   ```

   Replace the public IP filter if the phone address changes. Capture the
   baseline and candidate in separate files. Use a 120/240 fps camera showing
   both the input contact and Android display; divide the frame count from
   contact to first visible response by the recorded frame rate.
5. Compare matched motion windows and the stop/recovery interval. Acceptance
   requires no multi-second stale graphics reservoir with a useful interactive
   update rate, no lasting static-screen damage, and no regression in input,
   clipboard, audio, first frame, reconnect, or display restoration. Check
   Win98 1024×768/16-bit and Mac RDC compatibility before considering this
   the default Mac policy. A local queue decrease alone is insufficient.

The current deterministic evidence supports an isolated hardware candidate,
not a default-policy promotion or a claim of physical latency improvement.
