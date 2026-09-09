# Build 0.1.8 regression and hardware acceptance

Subsequent hardware testing confirmed old Mac RDC stability and bidirectional
clipboard. Win98 keyboard compatibility is also hardware-passed: its Control
shortcut chord maps to macOS Command. The labeled key tests and unavoidable
limitations are documented in [DEVELOPMENT.md](../DEVELOPMENT.md). The current
artifact adds only Win98 clipboard framing and diagnostics to that keyboard-passed
baseline.

Build **Mac Shadow RDP — Build 0.1.8** is release-qualified. Hardware acceptance
is complete on the target Sonoma host with Microsoft Remote Desktop for Mac,
Android/aRDP, and Microsoft RDP 5.2 on Windows 98. Clipboard transfer is accepted
in both directions on Win98. The 0.1.7 tag (`mac-shadow-rdp-0.1.7`, `59386e731`)
remains the prior recovery baseline.

## Established crash cause

Four local macOS crash reports (`freerdp-shadow-cli-2026-09-09-000639.ips`,
`000642.ips`, `000649.ips`, `000810.ips`, in `~/Library/Logs/DiagnosticReports`)
report `EXC_BAD_INSTRUCTION / SIGILL` on queue `mac.shadow.clipboard`, with:

> BUG IN CLIENT OF LIBDISPATCH: dispatch_sync called on queue already owned by current thread

The original production subsystem binary UUID matched the crash image:
`C41CAA2A-6C7C-3B1F-9235-F8C2609783B8`. Before rebuilding, `atos` resolved
image-relative offsets `0x105cb`, `0x10a44`, `0x10413`, `0x73de`, `0x94f6` to:

1. `server_format_data_request + 139`
2. `__client_format_list_block_invoke + 356`
3. `client_format_list + 147`
4. `cliprdr_server_check_event_handle + 4078`
5. `cliprdr_server_thread + 566`

The uncommitted clipboard implementation assigned an incoming request handler to
`context->ServerFormatDataRequest`, which FreeRDP defines as the outgoing sender.
On receiving a text Format List, the callback entered the serial clipboard queue
and called this sender. Instead it invoked the incoming handler, which tried to
synchronously enter the same queue. Libdispatch deliberately trapped. This is
independent of desktop depth/resolution and does not implicate 1280x800 at 15 bpp.
The same callback-direction mistake exists in commit `cb6479dff`; its original
implementation lacked the queue and thus broke request direction without this
specific dispatch trap.

`channels/cliprdr/server/cliprdr_main.c` dispatches incoming data requests to
`ClientFormatDataRequest`. The repair installs the handler there and leaves
`ServerFormatDataRequest` intact. No queue-reentrancy workaround masks the error.

The capabilities “4 bytes not parsed” warning uses the remaining length of the
whole channel stream, independently of the header's declared body length. The
parser logs it and returns the handler status; it is not a fatal branch. A test
feeds a valid 16-byte capabilities body with four trailing bytes and then a text
Format List through the actual server PDU parser. The warning occurs and the
request succeeds. This explains why the warning does not establish the crash
site; the exact trailing bytes from the historical client were not captured.

## Legacy four-byte clipboard trailer

Historical evidence supports the connection-local RDP 5.x framing compatibility
mode required by the hardware-accepted Microsoft RDP 5.2-on-Win98 path:

- Microsoft's [MS-RDPECLIP product behavior appendix](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/bfa8a3bf-cb24-4489-8650-7c152f84aea4)
  says multiple Windows implementations append four bytes to clipboard PDUs and
  exclude those bytes from `dataLen`.
- [rdesktop's legacy `cliprdr_send_packet`](https://github.com/rdesktop/rdesktop/blob/master/cliprdr.c)
  allocates `payload + 12`, writes the eight-byte clipboard header and logical
  payload, then writes a zero DWORD.
- xrdp's October 2007 [legacy clipboard wire notes](https://xrdp.sourceforge.net/documents/channels/cliprdr/clipboard.html)
  show a zero-body Monitor Ready arriving as 12 channel bytes and other clipboard
  messages carrying the same four trailing zeros.

Modern FreeRDP server serialization previously sent exactly the bytes written by
the PDU constructor: eight-byte header plus logical payload. Build 0.1.8 now has an
off-by-default server-context switch which appends the historical zero DWORD after
fixing `dataLen`; only the existing Win98 build-3790 clipboard fingerprint enables
it before channel startup. Modern Mac RDC and Android contexts retain exact modern
framing. **Do not remove this trailer.** When the same fingerprint receives exactly
one all-zero DWORD beyond the declared body, the receiver recognizes it and narrows
only its in-memory parser view to the declared body. This suppresses the known benign
“4 bytes not parsed” warning without changing the received or transmitted wire bytes.
Unexpected trailing data is still diagnosed normally.

## Other protocol defects and repairs

- Original `cb6479dff` replaced the outbound Monitor Ready sender with a callback
  that resent capabilities instead. The extra capability struct also omitted its
  common header. Historical server logs contain “called with invalid type
  00000000”. The pre-existing worktree corrected this; the repair retains the
  actual FreeRDP initialization senders and their diagnostics.
- The worktree gated pasteboard publication on receiving Client Capabilities.
  Win98 sends only server capabilities and Monitor Ready. Publication now
  begins after successful Monitor Ready; a legacy client need not send capabilities.
  Short format names remain selected, as advertised by the server. File clipboard
  flags remain off.
- Numeric-only text selection rejected registered plain-text formats. This
  repository's Android implementation advertises registered names, including
  `text/plain`. Explicit `text/plain`, `UTF8_STRING`, and
  `text/plain;charset=utf-8` names are recognized as UTF-8; the client's original
  format ID is used for the request. The old Android log did not include IDs or
  names, so its exact rejected format remains unproven. New per-format diagnostics
  resolve this without guessing IDs or accepting arbitrary registered data.
- Valid empty/non-text Format Lists now receive OK, with no data request. FAIL is
  not used to mean “this valid list has no text format we support”.
- Only one text request is outstanding: responses carry no format ID. Later lists
  are coalesced until that response arrives, preserving the response's encoding.
- Existing worktree UTF-16LE, Windows-1252, and CP437 conversion and newline/NUL
  handling are retained. ANSI/OEM are Western fallbacks; other client code pages
  are not negotiated. Invalid text is logged and ignored without terminating the
  RDP session.
- Teardown marks state stopped on its queue, waits for timer cancellation and the
  channel worker, then releases state. Incoming callbacks check stopped state on
  that same queue. Clipboard startup failure remains nonfatal to the session.

The pre-existing removal of the speculative Microsoft-Mac left-Control remap is
retained. The keyboard mapping follows 0.1.7. Per-key scan-code and per-PDU clipboard
tracing now use DEBUG level; INFO retains the selected client profile and Win98 legacy
clipboard selection, while failures remain WARN/ERROR.
No capture, graphics scheduling, audio, transport/security, Android mouse,
resolution selection, or intentional VAIO physical 1024x768 switching is changed.

## Automated validation

`TestMacShadowClipboard` compiles the actual Mac adapter and server PDU parser into
a test executable. It verifies modern 8-byte Monitor Ready framing, legacy 12-byte
Monitor Ready framing, the 48-byte legacy CF_TEXT Format List with logical
`dataLen=36`, peer-trailer detection, callback direction, short-name wire parsing,
capability trailing-byte tolerance, Monitor Ready without client capabilities,
Unicode preference, named UTF-8 selection with client-local IDs, overlapping
requests, failed responses, non-text/empty list acknowledgement, and text encoding,
terminators, and line endings. It does not access the system pasteboard or open a
listener. Existing publication/input, bitmap, codec, region, and synchronization
regressions also remain required; commands are in `DEVELOPMENT.md`.

Automated checks complement, rather than replace, the completed hardware matrix.

## Completed hardware acceptance

- Microsoft Remote Desktop for Mac: established display, input, audio, reconnect,
  and bidirectional clipboard behavior accepted.
- Android/aRDP: established display, input, audio where supported, reconnect, and
  bidirectional clipboard behavior accepted.
- Windows 98 / Microsoft RDP 5.2: accepted at 1024×768, 16-bit with the established
  legacy security/tunnel profile; keyboard, mouse buttons, dragging, wheel direction,
  local cursor, audio where supported, reconnect, and bidirectional clipboard passed.
- Sonoma permission diagnostics, first frame, idle/resume, disconnect/reconnect,
  display restoration, and clean shutdown were accepted. Graphics scheduling, latency,
  audio, resolution handling, security fallback, keyboard mappings, and clipboard
  behavior are unchanged by release finalization.

## Results of this repair run

- Release configuration: `-O3 -DNDEBUG`, warnings enabled, macOS shadow subsystem
  enabled; packaging verifies every production compiler command. Existing macOS SDK
  deprecation warnings remain nonfatal; the final production build has no errors.
- Final targeted CTest run: **13/13 passed**, including the actual-parser clipboard
  and legacy-framing regressions; 2.81 seconds. `git diff --check` passed.
- Production workflow: `python3 scripts/build-macos-shadow-app.py` completed.
  Bundled `/version` smoke test passed. No test listener or app was launched.
- Artifact: `dist/FreeRDP Shadow.app`, x86_64, **Build 0.1.8**.
- Certificate: **Apple Development: shardsofaperture (H7V72A5WH6)**;
  identifier: **io.freerdp.shadow.sonoma.menu**. Deep/strict verification passed
  with system keychain access, including nested code. Sandbox-only verification
  could not access certificate trust; verification outside that sandbox confirmed
  “valid on disk” and “satisfies its Designated Requirement”. No ad-hoc fallback.
- SHA-256, bundled `freerdp-shadow-cli`:
  `757873208d67ec7e948c86f464fb86043b130f42ffd757e2c8f81a3997858cde`.
- SHA-256, bundled `libfreerdp-shadow-subsystem3.3.30.1.dylib`:
  `db398d91d8a69ae7ad16e2a923a73122a01c66aa69c21206f86c20187dbab7db`.
- Graphics scheduling/encoders, audio, core transport/info/server, and menu launcher
  behavior is unchanged by the legacy trailer patch. The hardware-passed Win98
  Control-to-Command compatibility mapping is retained without modification.
- Hardware acceptance is complete. The release retains the historical Win98 RDP 5.2
  trailer exactly as validated and does not start 0.1.9 work.
