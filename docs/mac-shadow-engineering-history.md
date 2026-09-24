# Mac Shadow RDP engineering history

This is a compact record of decisions that still inform 1.0.0. It keeps the
measured conclusions without treating every candidate build as a supported
configuration. The immutable tags and release commits retain the full source
history.

## Transport rate selection

The 2026-09-23 physical sweep used fixed pacing at 150, 200, 250, 300, and
400 KiB/s with the same Android client, SSH path, and large foreground-window
promotion. The static controls were ordinary/F scheduling, no adaptive probe,
no burst, no coverage scheduler, default `SO_SNDBUF`, and the loopback listener.

| Rate | Largest observed sshd loopback Recv-Q | Drain to sustained 32 KiB | User observation |
| ---: | ---: | ---: | --- |
| 150 KiB/s | 11 KiB | immediate | About 6–8 seconds; roughly 8 black bars |
| 200 KiB/s | 31 KiB | immediate | Under 5 seconds; about 13 bars |
| 250 KiB/s | 38 KiB | about 0.51 s | About 3 seconds; fewer bars; responsive icons/menus |
| 300 KiB/s | 279 KiB | about 3.56 s | About 2–3 seconds; visual result varied during the action |
| 400 KiB/s | 315 KiB | about 7.64 s | About 3 seconds; no clear gain over 300 |

Across the full capture windows, RDP-to-sshd plaintext mean/p95 was 69.6/151.4,
97.8/200.7, 132.0/272.9, 129.7/330.8, and 140.7/446.3 KiB/s at the five
respective rates. sshd-to-phone encrypted mean/p95 was 72.8/154.2,
102.5/206.0, 131.6/255.8, 123.2/322.6, and 132.0/293.5 KiB/s. Whole-capture
windows include idle and background traffic, so these values are descriptive
of these runs rather than steady-state capacities. Mean sampled RTT ranged
from about 80 to 90 ms. Queue and client observations above are the decision
evidence; no phone-side presentation timestamps were collected.

The observed low-queue range was 150–250 KiB/s. The 300 KiB/s run was the
first with material transient queue pressure. At 400 KiB/s the loopback queue
was still elevated at the active-refresh boundary, and the higher rate gave
no reported visual gain. The test chose 250 KiB/s as the fastest measured
low-queue setting with responsive controls. These five physical actions do not
establish a universal network or client limit. Server and SSH counters cannot
separate network capacity from phone decode or presentation time. macOS
`nettop` `re-tx` values were retained in the original capture but their units
and semantics were not confirmed, so they are not interpreted as packet-loss
counts.

## Publication aggregation

The 0.2.1A implementation introduced a 50 ms first-damage aggregation window
on the private Mac capture surface. In one five-second motion log interval,
300 capture callbacks resulted in 75 publications (4:1), with publication
cadence around 64–70 ms. This reduced redundant publication work while
preserving the byte pacing budget. First frame, reconnect, and client refresh
bypass the aggregation window. The sender still distinguishes published
snapshots, unsent damage, serialized updates, and committed client cache state.

The accepted production configuration carries that behavior as 1.0.0. It uses
fixed 256000 B/s pacing, ordinary/F scheduling, default `SO_SNDBUF`, and no
adaptive, burst, or coverage behavior. The user accepted large-window
responsiveness and icons/menus. Continuous video was reported to cause major
lag and a temporarily frozen phone image; this known limitation was accepted
for 1.0.0 and is not attributed to a proven single root cause.

## Other decisions retained in the implementation

- Increasing pacing or adding large-refresh bursts did not show a sufficient
  client-visible improvement to justify the extra downstream queue pressure.
- Socket-cap candidates were measured separately. Production requests no
  socket buffer override (`SO_SNDBUF=0` in the config), leaving the OS default
  in effect.
- Spatial coverage and adaptive experiments remain off in production. The
  measured fixed-rate policy was easier to interpret and preserved control
  responsiveness in the accepted tests.
- Fine-grained pixel comparison and newest-state replacement limit repeated
  transmission of stale screen content. Replacement applies to unsent work;
  already serialized RDP updates retain protocol order.
- Warm bitmap-cache search and shadow virtual-channel service are bounded so
  graphics and control input regain service opportunities. Clipboard format
  data remains serialized because cliprdr responses do not carry request IDs;
  a late response cannot safely be matched by a local generation counter.
- The production path retains the classic FreeRDP shadow server and bitmap
  protocol. A new capture API or transport design is a separate engineering
  change requiring its own correctness and client-visible measurements.

## Protocol and scheduling repairs retained

- The Win98 RDP 5.2 clipboard profile appends a four-byte zero trailer outside
  `dataLen`. Both serialization and the matching parser exception are scoped
  to the established client fingerprint; modern clients keep standard framing.
- Clipboard text requests are serialized because cliprdr responses have no
  request ID. A timeout quarantines one late response before another request is
  issued; a local generation counter cannot disambiguate delayed wire replies.
  Text payloads are bounded, and clipboard failures must not stall graphics or
  input.
- Legacy Mac RDC keyboard compatibility is selected using its verified
  protocol/OS/hostname/product fingerprint, not client name, dimensions, or
  color depth. Its observed Left Control mapping remains profile-specific.
- Warm bitmap-copy candidate search is bounded within each operation. Shadow
  virtual-channel work is serviced in bounded ordered slices; the generic
  FreeRDP channel behavior remains unchanged for other server users.
- Preserve separate latest capture, published snapshot, unsent damage, and
  sent client-cache state. Only unsent content can be replaced. Serialized
  updates and cache commits remain ordered so ScrBlt and static convergence
  stay correct.

## Release and recovery references

The 1.0.0 release is the published commit/tag `f225ac1bb` / `1.0.0`. The
preceding stable source release is tag `0.2.0`; the earlier recovery source
release is `mac-shadow-rdp-0.1.7` (commit `59386e731`). The signed 0.2.0 and
0.1.9 Recovery bundles are local generated artifacts, not tracked source files.
See [the 1.0.0 release record](mac-shadow-1.0.0.md) for bundle identity,
signing, install path, and validation limits.
