# Shadow RDP: clipboard and sustained-latency source review

Historical pre-implementation review. Current behavior and validation are
recorded in `mac-shadow-latency.md` and `../DEVELOPMENT.md`; the findings below
describe the checkout at the review date and are not current defect claims.

Reviewed September 22, 2026 against
`8ded81d898ca2c8b5432fec73f2d00a68c3bd6eb` (`mac-shadow-rdp-0.1.8`).

**Status:** static source review and a replacement agent guide, not a deployed
repair. No Mac runtime sampling, Android reproduction, CTest execution, signing,
installation, restart, or remote repository modification was performed here.
The missing Jansson library is the user's reported local test-launch blocker;
it was not independently reproduced on the Mac in this review.

## What the symptoms establish

Phone-to-session paste stalls for more than 30 seconds and the session becomes
nearly unresponsive. Disconnect/reconnect restores usability but cancels the
paste. Copy/paste entirely within the remote Mac works. Separately, a connection
gets slower with use; reconnect helps temporarily.

These observations prioritize clipboard redirection and connection-lifetime
state. They do **not** prove a leak, a network fault, a server-wide deadlock, or
that both problems share one cause. Internal Mac paste bypasses the redirected
phone-to-Mac data exchange. Reconnecting can reset several server/client states
at once, so it is not a sufficiently selective diagnostic by itself.

## Findings, ranked for investigation

### 1. An unanswered client clipboard request can occupy the request slot indefinitely

Source: `server/shadow/Mac/mac_shadow_clipboard.m`,
`request_client_text`, `client_format_list`, and `client_format_data_response`.

The code deliberately permits one outstanding request, using
`requestedClientFormat`. Later offers replace a pending format but do not start
another request while that field is nonzero. The reviewed implementation has no
request deadline. A data response clears the slot; a send error also clears it.
Without either event, another text offer cannot progress through this path.

**Established:** a recovery gap for a missing response. **Not established:**
that the failing phone exchange reaches this state, or that it explains the
session-wide sluggishness. Instrument offer receipt, request enqueue, actual
channel submission, response receipt, pasteboard entry/exit, and subsequent
recovery as distinct events.

**Repair constraint:** the format-data response contains no request identifier
or format ID. Merely clearing the flag on timeout and requesting newer content
can associate a late old response with the new request. A local generation
counter protects local callbacks, not identification on the wire. Define a safe
quarantine/drain/reinitialization policy, preserving graphics/input when clipboard
cannot safely resume. Do not fabricate a protocol response to a request this
server itself sent.

### 2. One serial clipboard queue couples protocol progress to pasteboard calls

Source: the same file, `client_format_list`, `client_format_data_request`,
`client_format_data_response`, `poll_pasteboard`, and
`mac_shadow_clipboard_uninit`; also
`channels/cliprdr/server/cliprdr_main.c`, `cliprdr_server_thread`.

Clipboard callbacks enter the same serial queue with `dispatch_sync`.
`poll_pasteboard` calls `stringForType:` there. The outbound data-request handler
reads the pasteboard on that queue; the inbound data-response handler clears and
writes it there. Shutdown synchronously enters the queue, waits for timer
cancellation with `DISPATCH_TIME_FOREVER`, and drains it before stopping the
channel worker.

**Established:** a slow/blocking operation in this queue delays other clipboard
callbacks and can delay cleanup. **Important qualification:** `cliprdr` already
has its own worker thread. These synchronous calls do not, by themselves, prove
that the graphics/input thread is directly waiting on the pasteboard. A blocked
worker, blocked Mac destination app, Android UI wait, and shared output congestion
must be distinguished with thread samples and timings.

Avoid a blanket replacement of `dispatch_sync` with `dispatch_async`: callback
payloads can cease to exist after return, context lifetime matters at disconnect,
and FIFO backlog can simply move to a different queue. A timeout does not cancel
a synchronous OS operation. Keep protocol state independently serviceable and
bound outstanding OS work; consider helper-process isolation only if observed
non-cancellable blocking justifies its extra lifecycle/signing complexity.

Additional checks: payload bounds before decoding, clipboard ownership changing
during a request, text versus no-text changes, feedback suppression, and the
existing handler that logs a failed format-list acknowledgement without using
that failure to gate later responses. These are audit targets, not diagnosed
causes of the reported stall.

### 3. Warm-cache copy search can do substantial work outside the scheduler's deadline

Source: `server/shadow/shadow_bitmap.c`, `shadow_bitmap_stage`, `find_copy`,
`trim_damage`, `shadow_bitmap_next`; and `server/shadow/shadow_client.c`,
`shadow_client_flush_bitmap`.

The outer flush loop checks an eight-millisecond deadline between operations.
Inside an operation, copy search scans a coordinate radius of 128 pixels.
For an interior tile, the coordinate range can contain `257 × 257` locations,
excluding the unchanged position: up to 66,048 candidate positions in one search.
Its 32-comparison cutoff counts candidates only after signature matches; it is
not a limit on all signature probes. Direction selection can search additional
probe tiles. No time deadline is passed into these inner searches.

The cold-cache path skips copy search and damage trimming for unknown tiles.
Once tiles are known, those operations become eligible. Each new staged image
resets direction-selection/motion-hint state; staging also copies/converts the
whole framebuffer and compares tiles before the bounded flush.

**Established:** cold and warm cache paths have different work, and the outer
budget does not cap one expensive inner operation. **Inference to test:** this
could match “fresh reconnect is faster, then quickly becomes slow,” even with
stable allocated memory. It is not evidence of indefinite cache growth. The
bitmap state's principal allocations are dimension-bounded `latest`, `sent`,
`known`, and `dirty` buffers, rather than an appended frame history.

First measure per-stage/search/trim/encode time and all candidate probes.
Compare identical known-cache workloads with copy search enabled and disabled
in a test-only experiment. Then consider candidate/time caps, verified reuse of
recent motion vectors, or indexed signatures. Exact pixel/source verification
and overlap-safe order must survive. Disabling ScrBlt permanently is not the
acceptance criterion; neither is trusting a hash as proof of equal pixels.

### 4. The graphics budget does not cover the entire virtual-channel path

Source: `libfreerdp/core/server.c`,
`WTSVirtualChannelManagerCheckFileDescriptorEx`; `server/shadow/shadow_client.c`,
`shadow_client_thread` and audio-message processing; and
`channels/cliprdr/server/cliprdr_main.c`, `cliprdr_server_packet_send`.

The channel manager drains its queue with a `while (MessageQueue_Peek(...))`
loop and submits each message through `SendChannelData`. This path has no
explicit message/byte/time scheduling budget in the reviewed function. The
shadow client invokes the manager from the session loop. Clipboard output is
submitted through `WTSVirtualChannelWrite`, not through the bitmap-tile budget.

The separate shadow message queue processes at most 256 messages per pass,
coalescing audio to the newest block **within that batch**. When more messages
remain, that is not necessarily the newest audio in the entire backlog. The
shown coalescing is not an enqueue-time capacity or age bound, and it does not
bound messages already passed into the channel manager or transport.

**Established:** application graphics backpressure is not a complete
all-channel latency bound. **Not established:** that these queues grow in the
failing session. Record depth, bytes, oldest age, send/flush duration, and
production/consumption rates at each boundary. Inspect audio with it both enabled
and disabled, without treating disabled audio as the final fix.

A repair may require bounded servicing at an appropriate shared-code boundary,
with Mac-specific opt-in/default compatibility preserved. Complete protocol
ordering, fragments, and ownership still matter; arbitrary deletion of queued
wire packets is unsafe.

### 5. Cache failure changes the session's scheduling path

Source: `server/shadow/shadow_client.c`, `shadow_client_send_surface_update`
and `shadow_client_bitmap_scheduler`; `server/shadow/shadow_encoder.c`,
`shadow_encoder_reset` and construction/destruction.

A failed cache stage/allocation sets `encoder->bitmapFallback = TRUE`; the
scheduler predicate rejects that encoder afterward. The shown encoder-reset
routine frees bitmap state but does not explicitly clear that flag. A new
encoder is zero-initialized. Review every writer/reset in the local checkout
before finalizing its exact lifetime.

The fallback is intentionally a correctness mechanism: it preserves damage and
forces full correction using the standard encoder. Do not remove it. Log the
reason and duration, test failure injection, and determine whether an unexpected
transition explains the slowdown. The existing warning is:

```text
Bitmap cache unavailable; using the standard encoder
```

A justified recovery design needs backoff and a full cache re-synchronization,
not repeated allocation thrashing or pretend-valid client pixels.

### 6. Current deterministic clipboard coverage does not exercise the reported failure

Source: `server/shadow/test/TestMacShadowClipboard.m`.

The test intentionally avoids modifying the system pasteboard. It covers callback
direction, negotiation/trailers, format selection, overlapping offers followed
by failed responses, and encoding helpers. It does not reproduce a real blocking
pasteboard, successful inbound application to the Mac pasteboard, a never-arriving
response, long-running queue congestion, or disconnect during blocked OS work.

Those tests remain valuable. Add injected pasteboard/transport/clock seams for
the missing behaviors; retain existing compatibility cases. The historical
13/13 acceptance report is not proof of long-session recovery. The user's
current Jansson loader failure means those local binaries do not presently
execute at all, rather than proving a source regression.

## What is already implemented and should not be reinvented

The inspected code already has newest-state staging, a separate ordered `sent`
cache, sparse damage, negotiated tile sizing, 16-bit/32-bit paths, transport-block
checks, explicit buffered-output draining, and input checks before graphics.
The documented budgets are eight operations, 16 KiB, and eight milliseconds,
checked between operations. Clipboard has its own channel worker; audio startup
is already documented as asynchronous.

The task is to close measured holes in those boundaries, not add another
“latest-frame queue” or claim all slowness must be the VPN. The server writes to
a loopback SSH endpoint in the intended deployment; upstream socket progress is
not client presentation evidence. Actual Android version/build, negotiated
capabilities, network path, and client rendering state are still missing from
this review.

## Full-path investigation coverage

| Area | Question to answer before choosing a fix |
| --- | --- |
| Runtime/build | Is the tested process the intended signed build with the expected profile and libraries? |
| Clipboard | Which exact transition stops; is the request sent, answered, applied, and consumed by paste? |
| Input | Does input reach the server promptly, get injected promptly, and produce a fresh capture? |
| Capture/scaling | Are frame callbacks, conversion, publication waits, or full-frame staging expensive under the actual resolution? |
| Cache/encoder | Does warming enable expensive searches; are inner budgets honored; did fallback activate? |
| Audio/channels | Is enqueue age bounded at every layer; can large or frequent channel messages monopolize the loop? |
| Transport/SSH | Where do bytes accumulate; are local accepted bytes being confused with delivered/displayed bytes? |
| Network | Does the same workload differ on LAN, direct cellular, and corporate VPN, controlling other settings? |
| Android | Are UI, decode/draw, GC/memory, foreground clipboard access, or thermal behavior changing with session age? |
| Lifecycle/resources | Do workers, timers, event handles, retained objects, memory, or caches grow across reconnects? |
| Correctness | After motion stops, does the entire desktop converge without losing input or violating ScrBlt dependencies? |

## External protocol cross-check

Microsoft Open Specifications, MS-RDPECLIP §2.2.5.2 defines the format-data
response as its header plus requested data, without a request ID or format ID.
§3.1.5.4.3 specifies an unsuccessful data response when requested local data
cannot be retrieved. These support safe response matching and explicit failure
handling; they do not diagnose this app's observed stall.

No matching installed-version Android defect, current upstream fix, packet trace,
or hardware performance result is claimed. Inspect the exact client build and
upstream change before importing a client workaround or backport.
