<!-- markdownlint-disable MD013 -->

# Mobile and external-display resolution research

## Purpose

This note records what the current Sonoma shadow server can already do for an
Android RDP client, what must be measured on real devices, and the smallest
likely follow-up for phone rotation, foldables, tablets, and USB-C displays. It
is a design and test plan, not an implementation claim.

The security boundary does not change: run the Standard RDP Security listener
on `127.0.0.1:3390` and reach it through an SSH tunnel. Mobile testing must not
expose port 3390 directly to Wi-Fi, a mobile network, or the Internet.

## What the current repository already supports

The automatic macOS profile reads the initial `DesktopWidth` and
`DesktopHeight` advertised in the Client Core Data during connection. It tries,
in order:

1. an exact public macOS display mode;
2. an exact or closest compatible Sonoma display mode;
3. a bounded, aspect-preserving scaled RDP surface with reverse-mapped pointer
   coordinates.

This is device-independent. An Android client that advertises its selected
phone, tablet, or external-display size at connection time should therefore
receive that size without an Android-specific server profile. The fallback does
not upscale beyond the active Mac source display. The pre-connection Mac mode
is restored after disconnect or shutdown.

The current profile also composites the Mac cursor for every client except the
positively identified Windows 98 1024x768x16 compatibility profile. That is a
reasonable default for touch use and preserves the validated Win98 cursor path.

## What is not implemented yet

The initial desktop dimensions are a session request, not reliable device
identity. RDP Client Core Data can also contain hostname, product ID, build, OS
type, physical dimensions, orientation, and scale metadata depending on the
client, but applications are inconsistent about populating these fields. A
server must not assume that a hostname or product string uniquely means
"phone", "tablet", or "USB-C display".

The shadow server currently handles the size advertised while connecting, but
does not create a Display Control (`Microsoft::Windows::RDS::DisplayControl`)
server context. Consequently, it cannot yet consume a modern client's
mid-session Monitor Layout PDU. Rotating a phone, resizing an Android client
window, unfolding a device, or attaching/removing a USB-C display may therefore
remain at the connection-time size until reconnect, even if the client supports
dynamic resolution.

FreeRDP already contains the reusable server-side Display Control parser and
callback interface. Its monitor layout carries width, height, physical width,
physical height, orientation, desktop scale factor, and device scale factor.
The missing work is narrow integration with the shadow client and the macOS
display/surface transition lifecycle; the protocol parser should not be
rewritten.

## Recommended user-visible policy

Resolution selection should be based on explicit policy and advertised
capabilities, not guessed device models:

1. **Client requested (default):** accept a valid connection-time or dynamic
   single-monitor layout and feed it through the existing exact/closest/scaled
   macOS selector.
2. **Custom fixed size:** allow a menu choice or stored width and height that
   overrides the client's request for the next connection and optionally locks
   the session against rotation. This is essential when an Android client uses
   native pixels, reports an uncomfortable density, or mirrors a phone canvas
   to USB-C.
3. **Named presets:** offer user-owned presets such as `Phone portrait`,
   `Phone landscape`, `Tablet`, `External 1080p`, and `External 1440p`. Presets
   are conveniences, not hidden vendor/device detection.
4. **Remember by client:** only as an opt-in mapping keyed by logged client
   metadata plus a user label. Always provide editing and deletion because
   hostnames and product strings can collide or change.

Validate custom dimensions before allocation. Reuse the Display Control
protocol bounds (200 through 8192 pixels per dimension), cap total pixel area
to a conservative server policy, reject overflow, and retain the current
no-upscale/performance guard unless the user explicitly chooses otherwise.

For the first mobile implementation, support one primary monitor only. A USB-C
display can be represented as the one requested desktop; multi-monitor layouts
and moving the session between two simultaneous Android displays should remain
separate work.

## Proposed implementation sequence

### Phase 1: diagnostic-only Android trial

Do not change display behavior. Add or enable logs that capture, without
credentials:

- client hostname, product ID, build, OS major/minor, and RDP version;
- initial width, height, color depth, physical dimensions, orientation, and
  scale factors when available;
- negotiated `drdynvc` and Display Control availability;
- every Monitor Layout PDU during rotation, window resizing, folding, and
  USB-C attachment;
- selected physical source mode, scaled surface, and input transform.

Test the intended Microsoft Android RDP client and one independent Android RDP
client. Client names, UI labels, and behavior change over time, so record the
application name and version with each result rather than baking either into
server logic.

### Phase 2: Display Control plumbing

Create one `DispServerContext` per shadow client, advertise one-monitor limits,
and route a validated primary-monitor layout to a new subsystem callback. The
callback must serialize with connect/disconnect and capture reconfiguration.
Coalesce rapid resize events and apply only the newest request after a short
settling interval so rotation does not repeatedly tear down capture.

The transition should:

1. stop publication and release the current capture stream;
2. select the exact/closest/scaled source using the existing macOS code;
3. resize the shadow surface and update that client's RDP desktop dimensions;
4. restart capture and force one clean full frame;
5. atomically install the matching pointer/input transform;
6. restore the previous stable layout if any step fails.

Legacy clients without `drdynvc`/Display Control, including Windows 98 RDP
5.2 and RDC 2.1.1, must stay on the existing connection-time path.

### Phase 3: custom-resolution controls

Add a menu item with `Client requested`, presets, and `Custom...`. Persist the
policy under the menu application's stable bundle identity. The selected policy
must be visible in the menu and server log. A fixed policy should override both
the initial request and later dynamic requests; an automatic policy should
accept both.

Do not add a global security exception or device-specific network behavior.

## Test matrix

| Scenario | Evidence to record | Expected result |
| --- | --- | --- |
| Small phone, portrait | Initial request and physical/scale metadata | Legible portrait surface, correct taps and pointer |
| Same phone, landscape | Initial request; then rotate during session | Correct reconnect today; dynamic resize after Phase 2 |
| Tall/notched phone | Requested size and any client-side letterboxing | No server distortion; client safe-area behavior documented |
| Foldable closed/open | Monitor Layout sequence | Latest stable layout wins; no resize loop |
| Android tablet | Native and custom sizes | Sharp output at bounded size; correct input edges |
| USB-C mirror mode | Android request before/after attachment | Document whether client still reports phone canvas |
| USB-C desktop/extended mode | External display size, orientation, density | External size used when client advertises it |
| USB-C hot-plug in session | Display Control PDUs and disconnects | Resize safely or retain last good layout |
| Custom 1280x720/1920x1080 | Policy and selected Mac source | Exact or aspect-correct scaled result |
| Oversized/malformed request | Dimensions and rejection reason | No allocation spike; session retains last good layout |
| Windows 98 regression | 1024x768x16 profile | Unchanged local cursor and legacy bitmap path |
| RDC 2.1.1 regression | 1280x800 connection request | Existing 1440x900 source/scaled result remains stable |
| Modern mstsc regression | Initial and dynamic sizes | Existing connection behavior preserved |

For each usable mobile size, separately record idle, typing, scrolling, window
movement, and full-screen-motion bandwidth and latency. High-density native
phone resolutions may waste bandwidth without improving usability, so compare
native pixels with one or two lower custom resolutions.

## Decision before coding

The next useful action is a diagnostic Android connection, not another display
heuristic. Capture the initial settings and determine whether the chosen client
sends Display Control updates for rotation and USB-C changes. If it does, Phase
2 is justified and largely supported by FreeRDP's existing channel server. If
it does not, the custom/preset menu policy still provides deterministic sizes,
but rotation or USB-C changes will require reconnecting or choosing a different
client.
