# Graphics feedback decision (2026-09-22)

The later opt-in [socket-cap experiment](mac-shadow-socket-cap-experiment.md)
implements the transport test proposed below. It does not change the ACK
decision or establish a default policy.

## Decision

Do not build an ACK-window controller around the current Mac classic bitmap
scheduler. No negotiated, standard RDP acknowledgement has been shown to cover
its `BitmapUpdate` and `ScrBlt` output. The standard frame ACK is promising for
**surface-command** frames, but those are a different graphics path. Keep the
post-0.1.9 `SO_NWRITE` pacer as an experimental, reversible fallback candidate;
its 648 KiB hidden-forwarder model reaches 716 KiB (about 4.8 seconds at
150 KB/s), so it is not an end-to-end latency bound or an accepted primary
policy. The best currently available classic-path fallback to test is a
**per-client** conservative byte pacer plus a measured, modest accepted-socket
send-buffer cap. The cap can improve the timeliness of local backpressure,
while the pacer limits blind overshoot; neither can bound sshd's downstream
reservoir. No such socket change is implemented or accepted yet. The
installed 0.1.9 is untouched.

This is a protocol and source decision, not a claim about the actual Android,
Win98, or Mac RDC capability bytes: no Confirm Active capture or client frame
ACK trace from those physical sessions was available for this investigation.
Capability flags set in server source are offers, not proof of negotiation.

## Mechanisms and exact source path

| Mechanism | Negotiation and wire path | What it reports | Classic path? |
| --- | --- | --- | --- |
| Surface frame marker and `TS_FRAME_ACKNOWLEDGE_PDU` | Surface Commands capability (`0x001c`, `SURFCMDS_FRAME_MARKER`) plus Frame Acknowledge capability (`0x001e`); fast-path `SURFCMDS` BEGIN/END with a 32-bit frame ID; client-to-server Share Data `FRAME_ACKNOWLEDGE` carries that ID | The spec says the client has finished processing the *surface bits* in that frame. FreeRDP GDI sends the ACK when it handles END, before Android's queued Java UI redraw is known to have appeared. Thus it is client processing feedback, not a measured photon/display timestamp. It is much stronger than local TCP acceptance. | No specified association with classic `BitmapUpdate` or `ScrBlt`. The surface marker groups surface commands, not arbitrary preceding graphics updates. |
| Alternate-secondary Frame Marker drawing order | Order capability `ORDERFLAGS_EX_ALTSEC_FRAME_MARKER_SUPPORT`; order START/END has **no frame ID** | A logical group of drawing orders for cohesive rendering. The FreeRDP GDI callback returns without sending an ACK. | May group orders if negotiated, but supplies no frame ACK and does not turn bitmap updates into acknowledged frames. |
| RDPGFX Start/End Frame and Frame Acknowledge | `rdpgfx` dynamic channel with its own caps and `RDPGFX_FRAME_ACKNOWLEDGE_PDU` | Decoded RDPGFX frame ID, `totalFramesDecoded`, and optionally buffered graphics bytes in `queueDepth`. `0xffffffff` suspends future ACKs. It is not a client display timestamp. | No. It requires the RDPGFX graphics path. |
| RDP Auto-Detect RTT/bandwidth response | Auto-Detect request/response on the main connection, when supported | Transport path RTT or transfer rate, not graphics decode or presentation. | Potentially a path estimate, never a per-frame completion signal. |

Source trace in this checkout:

- `server/shadow/shadow_client.c`: `shadow_client_context_new()` enables both
  marker settings as server offers; `shadow_client_capabilities()` delegates
  backend capabilities. `shadow_client_bitmap_scheduler()` selects the Mac
  newest-state path, and `shadow_client_flush_bitmap()` emits each 16-bit
  interleaved or 32-bit planar tile through `BitmapUpdateProxy`, or a valid
  `ScrBlt` through BeginPaint/EndPaint. It does not call `SurfaceFrameMarker`,
  `SurfaceFrameBits`, or `shadow_encoder_create_frame_id()`.
- `shadow_client_send_surface_bits()` creates an ID only when `encoder->frameAck`
  is true and sends it through `SurfaceFrameBits`; `shadow_client_send_surface_gfx()`
  creates an ID for RDPGFX Start/End Frame. Both are outside the Mac bitmap
  scheduler. `shadow_encoder_reset()` sets `frameAck` from the negotiated
  `SurfaceFrameMarkerEnabled` setting; `shadow_encoder_create_frame_id()`
  increments its counter and adjusts a preferred FPS, but the Mac bitmap
  scheduler never consumes that FPS suggestion.
- `libfreerdp/core/update.c`: `update_send_surface_frame_bits()` writes
  SURFCMDS BEGIN, bits, END; `update_send_surface_frame_marker()` can write a
  standalone SURFCMDS marker; `update_send_bitmap_update()` uses the separate
  bitmap update type. An isolated marker sent after a classic update would
  acknowledge the marker's surface frame, not expressly that classic bitmap.
  `libfreerdp/core/peer.c` dispatches incoming Share Data frame ACK IDs to
  `update->SurfaceFrameAcknowledge`.
- `libfreerdp/core/capabilities.c`: reading `0x001c` sets
  `SurfaceFrameMarkerEnabled` from `SURFCMDS_FRAME_MARKER`; absent `0x001c`
  clears it. Reading `0x001e` supplies `FrameAcknowledge`; absent `0x001e`
  clears the count. Confirm Active includes the server's frame-ACK capability
  only when the client sent it and the count remains nonzero. The two
  capabilities must be checked separately after negotiation. In particular,
  `shadow_encoder_reset()` uses the surface-marker flag alone: that boolean
  does not prove that the peer offered the separate frame-ACK capability.
- `server/shadow/shadow_client.c`: `shadow_client_surface_frame_acknowledge()`
  records `lastAckframeId` and marks `queueDepth` unavailable;
  `shadow_client_rdpgfx_frame_acknowledge()` records the ID and RDPGFX depth.
  `shadow_encoder_inflight_frames()` subtracts IDs. The current callback
  assigns any incoming ID without validating order or range; a future window
  must track submitted IDs and ignore duplicate, old, or impossible ACKs.
- `libfreerdp/gdi/gdi.c`: `gdi_surface_frame_marker()` sends the Share Data ACK
  on END if `FrameAcknowledge > 0`; `gdi_frame_marker()` for the alternate
  order does nothing. The Android client source initializes GDI in
  `android_post_connect()` and queues a Java UI refresh from
  `android_end_paint()` / `SessionActivity.OnGraphicsUpdate()`. An ACK in this
  source tree cannot prove Android UI presentation. The installed
  `aFreeRDP-14a80a` binary has not been matched to this source revision.
- `channels/rdpgfx/client/rdpgfx_main.c` ACKs RDPGFX END after `EndFrame` unless
  ACKs are disabled or suspended. That ACK does not report classic updates.

The surface ACK specification defines a response to a surface-command END
marker after that frame's surface bits have been processed. It recommends a
window of multiple outstanding frames rather than waiting after every frame.
TCP orders the bytes to one client, but the spec does not promise that a
surface frame marker delimits a mixture of classic bitmap updates and drawing
orders. Using an empty surface frame as a cross-type barrier would depend on
client implementation behavior; it is not a justified compatibility design.
Likewise, a real surface-bits path would change the graphics codec/path and
must pass its own Android/Win98/Mac tests before replacing the current path.

## Compatibility status

| Client | Source/protocol inference | Physical negotiation proof |
| --- | --- | --- |
| `aFreeRDP-14a80a` | The repository's Android client uses FreeRDP GDI; client defaults include `SurfaceFrameMarkerEnabled=TRUE`, surface command flags, and `FrameAcknowledge=2`. It can ACK a negotiated surface marker in this source revision. It cannot supply a standard ACK for the current classic scheduler. | **Unknown** for that APK: obtain its Confirm Active `0x001c`/`0x001e` and an actual marker/ACK trace on an isolated run. |
| Microsoft RDP 5.2 on Win98 | Classic 16-bit bitmap path remains required. RemoteFX/RDPGFX cannot be presumed available; neither classic output nor alternate order markers provide the surface ACK. | **Unknown** capability bytes; capture Confirm Active to verify. Do not enable surface/RDPGFX merely to gain ACKs. |
| Legacy Mac RDC profile | Preserve the full fingerprint in `DEVELOPMENT.md`. It has the current classic compatibility path; no frame-ACK support is established. | **Unknown** capability bytes; capture Confirm Active to verify. |

Even if Android advertises both capabilities, a positive negotiation only makes
ACK useful for correctly framed **surface** output. It does not make the
existing `BitmapUpdate`/`ScrBlt` stream eligible for an ACK window.

## Controller alternatives and modeled limits

| Option | Benefit | Limitation and decision |
| --- | --- | --- |
| ACK window of 1 | At most one *complete, correctly framed surface* generation in flight; newest unsent state can replace intermediate captures. | At RTT `R`, frame bytes `B`, and window `W`, ACK-limited throughput is at most `W*B/R` before decode time. With `R=50 ms`, `B=8 KiB`: W=1 gives about 160 KiB/s, W=2 about 320 KiB/s. A one-frame window can create a bubble between ACK and the next send. Not usable for classic path. |
| ACK window of 2 | Reduces RTT bubbles and follows the protocol recommendation to allow multiple outstanding frames. | Twice as much already serialized work can become stale; at 150 KiB/s, two 16 KiB frames represent at least about 213 ms of transmission, excluding earlier queues, decode, and display. Not usable for classic path. |
| ACK + local pressure | ACK would be primary for a proven surface/RDPGFX path; `SO_NWRITE`, BIO blocked state, and bounded output buffer would stop local overrun. | Useful if an alternate path earns acceptance; local signals alone cannot observe sshd's hidden queue. |
| Existing local byte-credit pacer | Reversible fallback experiment for clients without valid ACKs; preserves newest-state replacement and input servicing. | Hidden 716 KiB backlog in the supplied model disproves an end-to-end bound. Leave experimental, do not promote as primary. |
| Per-client `SO_SNDBUF` cap | Can make *this* accepted socket exert backpressure earlier. | sshd's local receive buffer and encrypted/public-socket queue remain downstream. Smaller buffers may slow the first full refresh or healthy LAN. Do not change global transport. On this Mac, an isolated `getsockopt`/`setsockopt` probe returned 16/32/64/128 KiB for the same requests, not double; measure the actual accepted socket before drawing a Darwin-wide conclusion. |
| Auto-Detect RTT/bandwidth | Could give a path-capacity prior if negotiated. | No graphics decode/display progress, no bound on sshd's queued bytes. |

For a 2 MiB static full refresh divided into 8 KiB acknowledged frames,
256 frames would need at least about 12.8 seconds with W=1 or 6.4 seconds
with W=2 at a 50 ms ACK cycle, before transmission and decode costs. Larger
frames or an explicit full-refresh policy would be necessary for acceptable
first-frame completion; they would also increase the amount of unreplaceable
work. This calculation is a model, not a measured client result.

Any future ACK-window implementation must keep serialized PDUs ordered, stage
and replace only unsent captures, preserve ScrBlt source dependencies, and
allow a full refresh to finish in bounded chunks. An ACK is not proof that
every screen pixel is now current. Duplicate/old/out-of-range IDs must not
advance a window. TCP loss causes a connection failure or delay rather than
an individually retransmittable ACK; after an ACK deadline, retain damage,
stop trusting that feedback for the connection, and resume only under a
defined conservative fallback or reconnect. Never synthesize delivery from a
timeout. An idle desktop sends no probe frames merely to keep ACKs arriving.

## Physical evidence needed before revisiting ACK pacing

Do this only in a separately authorized, isolated run: keep installed 0.1.9
and port 3390 intact; use a separately signed executable on loopback 3391 and
the same SSH route. Record executable path/hash, profile, negotiated depth,
actual graphics mode, and client APK build/hash. Capture or decode Demand
Active and Confirm Active capability sets, specifically `0x001c` command flags,
`0x001e` maximum unacknowledged count, order capability frame-marker bit, and
RDPGFX dynamic-channel caps. Record whether the negotiated classic stream has
any `SURFCMDS` BEGIN/END, `FRAME_ACKNOWLEDGE` Share Data PDU, or RDPGFX
Start/End/ACK. No `SURFCMDS` marker in a classic trace means no standard
surface-frame ACK can be expected from that stream. Repeat on Android, Win98,
and Mac RDC. Packet captures can contain credentials and screen content;
decode privately, then retain only capability flags, PDU kinds/IDs, sizes,
monotonic timestamps, and anonymized queue samples.

The candidate build/start and queue-sampling commands in
`docs/mac-shadow-graphics-pacing.md` are the isolated-run starting point.
Before any connection, record the executable with `shasum -a 256` and
`codesign --verify --deep --strict --verbose=2`; use `lsof -nP -iTCP
-sTCP:ESTABLISHED` to map its accepted `127.0.0.1:3391` socket to the local
`sshd` socket. For each client, start a short, private loopback capture before
the handshake with `sudo tcpdump -i lo0 -s 0 -w
/tmp/mac-shadow-ack-CLIENT.pcap 'tcp port 3391'`, stop it after a bounded
motion/idle trial, and inspect only the capability and graphics/ACK PDU types.
Legacy RDP security can encrypt the capture, so raw `tcpdump` alone is not
proof of negotiated values. If the PDUs cannot be decoded, add an opt-in
one-line DEBUG summary at `shadow_client_activate()` of
`ReceivedCapabilities[0x001c]`, `SurfaceCommandsSupported`,
`SurfaceFrameMarkerEnabled`, `ReceivedCapabilities[0x001e]`,
`FrameAcknowledge`, `OrderSupportFlagsEx`, depth and selected graphics path;
do not infer these values from the server's initial offers. Add a five-second
counter summary to the two server ACK callbacks only if a real ACK appears.
Keep those logs private and exclude usernames, pixels, clipboard data and
keystrokes. An ACK for a standalone marker after classic updates would require
a separate client-specific rendering experiment and is not a protocol-level
guarantee.

If a separate surface-path experiment becomes justified, use explicit DEBUG
or opt-in counters limited to one summary per five seconds: connection and
publication IDs, submitted frame IDs/bytes/times, ACK IDs/times/RTT histogram,
outstanding window, pending newest generation, replaced generation count,
pending dirty tiles, `SO_NWRITE`, and BIO blocked duration. Correlate this
with `sshd` local and public socket queues and 120/240-fps input-to-visible
video. A client ACK alone is not a measured display latency. Test static
reconnect/full refresh, motion and stopped-motion recovery, clipboard/audio,
Win98 16-bit, Mac RDC, and aged Android sessions. For surface-path ACKs,
compare W=1 and W=2 using identical motion and a 150 KiB/s shaped route;
measure p50/p95/max video latency, useful updates, ACK RTT, and queue age.

No controller source change or ACK test was warranted at this gate. Existing
Release and pacing regressions remain the validation set for this
documentation-only decision.

## Protocol references

- [MS-RDPRFX frame ACK capability](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdprfx/e4d498fd-822b-408d-b8b3-1c216f21265b)
  and [frame ACK PDU](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdprfx/24364aa2-9a7f-4d86-bcfb-67f5a6c19064)
- [MS-RDPBCGR surface frame marker](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/7a4d7c0c-06e3-4802-aaa8-92c8f1d86f6e)
  and [fast-path surface command update](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/cc14f53d-8303-4f0b-b0dc-5c865410c381)
- [MS-RDPEGDI alternate-secondary frame marker](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegdi/9ba0e3d2-04ae-4c73-a35e-cb854f587018)
- [MS-RDPEGFX frame ACK PDU](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/0241e258-77ef-4a58-b426-5039ed6296ce)
- [MS-RDPBCGR network characteristics detection](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/b716fd9a-5a0b-4970-b5d0-f1be451408b7)
