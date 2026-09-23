# Mac Shadow RDP 1.0.0 production release

**2026-09-23 physical acceptance:** On the same phone/SSH path, the quiet
desktop was fresh and responsive, and the large-window promotion took less
than three seconds with no reported lag. Continuous video caused major lag
and a frozen image; after video stopped the image resumed but initially felt
slower. The user explicitly accepted 1.0.0 with this known high-motion
limitation and clarified that 0.2.1A was to be promoted without another
acceptance campaign. The video observation does not identify its root cause.

The production BuildLabel is `1.0.0 — 50 ms latest-state / Fixed 250 KiB/s`.
The signed app is built at `dist/FreeRDP Shadow 1.0.0.app` and installed at
`/Users/zach/Applications/FreeRDP Shadow.app`. The listener remains
`127.0.0.1:3390` behind the existing SSH tunnel. The bundle identifier remains
`io.freerdp.shadow.sonoma.menu`, signed by
`Apple Development: shardsofaperture (H7V72A5WH6)` / team `66GMSP473V`.

## Selected behavior and evidence

The fixed graphics budget is **250 KiB/s = 256000 B/s**. The [physical
RateSweep findings](mac-shadow-engineering-history.md#transport-rate-selection) found it
to be the highest tested low-queue rate with an estimated three-second large
foreground-window refresh and responsive icons on the phone. At 300 KiB/s,
sshd loopback Recv-Q peaked near 279 KiB and took about 3.56 seconds to
recover to the sustained 32 KiB band. At 400 KiB/s, it peaked near 315 KiB,
took about 7.64 seconds to recover, and gave no reported visible gain. Those
observations do not establish a general network or client decoding limit.

The [0.2.1A implementation](mac-shadow-engineering-history.md#publication-aggregation) added a 50 ms first-damage
publication gate to the private Mac capture surface. Capture events within a
window union damage and retain only newest pixels; the generic bitmap sender
still replaces obsolete unsent targets against its committed sent cache. First
frame, reconnect, and client-requested refresh bypass the gate. The gate
limits ordinary publication frequency; it does **not** change the 256000 B/s
transport budget, create an encoded-frame queue, or require sending a full
screen every 50 ms. The user physically accepted the signed 0.2.1A behavior
for this production freeze. The 1.0.0 package uses the same graphics behavior
and production rate; its video limitation is documented above.

A representative five-second 0.2.1A server log window during motion recorded
300 capture events and 75 publications (4:1), with publication cadence
64–70 ms and fixed graphics pacing at 256000 B/s. This confirms the gate
activated in that run. It does not measure when the phone presented the pixels.

Production selects ordinary/F bitmap scheduling, system-default SO_SNDBUF,
and no adaptive probing, large-refresh burst, or N coverage scheduler. Input
priority, channel servicing, text clipboard, system audio, compression,
negotiated capabilities, and the SSH/phone configuration remain as in the
accepted candidate. Experimental mechanisms and notes remain in source/history
but are not selected by the signed production config.

The host video model reduced 100 capture publications to 17 but did not reduce
simulated charged bytes or stale-tail time. It cannot prove client-visible
freshness. During physical testing, connecting while YouTube or other broad
high-motion content was already active could produce a badly stale session;
reconnecting with a quiet desktop restored normal performance. Start a fresh
connection on a quiet desktop before the short motion smoke check. This is a
known connection-time caveat, not a diagnosed or repaired root cause.

## Build, installation, and recovery

`python3 scripts/build-macos-shadow-app.py` builds the Release app with
`-O3 -DNDEBUG`, required json-c, the existing signing identity, a signed static
config, source provenance, and a SHA-256 manifest beside the bundle. The app
uses the existing `SMAppService mainAppService` Launch at Login registration.
There is no second startup mechanism. The installed menu app supervises the
server, and the server binds only to loopback.

The signed `dist/FreeRDP Shadow 0.2.1A.app` is the accepted immediate rollback
reference. The previous installed 0.2.0 app was preserved at
`dist/FreeRDP Shadow installed-0.2.0.backup.app`; the signed
`dist/FreeRDP Shadow 0.2.0.app` and
`dist/FreeRDP Shadow 0.1.9 Recovery.app` are retained. The 1.0.0 installation
procedure preserves one additional copy of the previously installed 0.2.1A
app before replacement. No existing tag or historical experiment evidence is
rewritten.

## Validation limits

The targeted Release/json-c suite covers publication, aggregation, bitmap
reconstruction, pacing, socket caps, clipboard, and Mac keyboard/input paths.
The installed smoke check covered a quiet first connection, large-window
promotion, and a short motion test. Continuous video lag is a known limitation
accepted by the user for this release. Windows 98, Mac RDC, aged-session
operation, and actual reboot/login autostart of this exact 1.0.0 bundle remain
separate acceptance work. An enabled SMAppService registration and a correct
installed target do not by themselves prove startup after reboot.

The release bundle member hashes are recorded in
`dist/FreeRDP Shadow 1.0.0.sha256.txt`. The adjacent `.source.patch` and
bundled `SourceProvenance.json` describe the exact source state used for the
package.
