# Mac Shadow RDP 0.2.0 production freeze

The production app is `dist/FreeRDP Shadow 0.2.0.app`, installed at
`/Users/zach/Applications/FreeRDP Shadow.app`. Its static BuildLabel is
`0.2.0 — Fixed 250 KiB/s`; `.source_tag`, the bundled CLI version, and both
app version fields are `0.2.0`. The bundle ID remains
`io.freerdp.shadow.sonoma.menu`, signed by
`Apple Development: shardsofaperture (H7V72A5WH6)` / team `66GMSP473V`.

The normal app launch uses its signed `ShadowConfig.plist` with fixed
250 KiB/s (256000 B/s) ordinary/F bitmap pacing. No launch-selected rate is
required. The accepted TCP socket keeps the system-default SO_SNDBUF;
adaptive probing, large-refresh burst, and the N coverage scheduler are
disabled. Input-before-graphics and channel servicing remain as in the tested
RateSweep binary. The listener stays on `127.0.0.1:3390` behind SSH. The
50 ms publication aggregator was not included.

The [physical RateSweep comparison](../experiments/transport-rate-sweep/CAMPAIGN.md)
selected 250 KiB/s as the highest measured low-queue rate with an estimated
three-second foreground-window refresh and responsive icons on the phone.
At 300 KiB/s sshd loopback Recv-Q peaked around 279 KiB and took 3.56 seconds
to return to the sustained 32 KiB band; at 400 KiB/s it peaked around 315 KiB,
took 7.64 seconds, and produced no reported visible gain. This is a short
phone/SSH physical comparison, not a claim of a general network ceiling or
client display latency. The raw nettop `re-tx` field remains uninterpreted.

The production code retains the opt-in L/M/RC3 burst, N coverage, diagnostic
socket-cap, and RateSweep source and tests as experimental history. The signed
production config selects none of those behaviors. The complete RateSweep
captures, metrics, and operator notes are preserved under
`experiments/transport-rate-sweep/`. The signed 0.1.9 recovery artifact at
`dist/FreeRDP Shadow 0.1.9 Recovery.app` was not overwritten.

## Validation and operation

- The production packager checks `Release`, `-O3 -DNDEBUG`, disabled development
  instrumentation, `.source_tag`/bundle version agreement, and the exact signed
  production plist policy before packaging.
- The full targeted Release/json-c regression suite passed 19/19 after the
  version change. The RateSweep harness passed 5/5 Python tests, and its C
  argument-selection test passed. `git diff --check` passed.
- The final bundle and installed copy passed `codesign --verify --deep --strict`;
  bundle ID, authority, team, BuildLabel, fixed-rate config, and hashes were
  checked. The installed server process exposed fixed rate `250`, socket cap
  `0`, and no burst, coverage, or pacing-diagnostics variables. The phone's
  SSH forward established a loopback connection to the installed server, and
  the user confirmed a fresh, responsive desktop after reconnection.
  Exact artifact hashes are recorded below.
- The existing `SMAppService mainAppService` Launch at Login item is enabled
  for the signed app at the installed path. No second startup mechanism was
  added. Full post-reboot/login startup remains to be observed after a later
  reboot; no reboot was performed for this release.
- The prior installed RateSweep 250 KiB/s app is retained as a rollback at
  `/Users/zach/Applications/FreeRDP Shadow.before-0.2.0-20260923.app`. It
  requires `--rate-sweep-kib=250` when launched manually.

The exact 0.2.0 bundle is not yet retested on Windows 98, Mac RDC, or an aged
session. Its fixed-rate behavior was physically validated on the phone with
the RateSweep package. The production change moves that selection into signed
static config and changes version metadata; the targeted regressions and
installed runtime checks cover the rebuilt binary's behavior. Automatic
startup after reboot also remains to be observed.

## Artifact hashes

Recorded after final signed build and installed-copy comparison:

| Signed bundle member | SHA-256 |
| --- | --- |
| `Contents/Info.plist` | `07051cb5d920ee1549f4febe8b778ba37ae52fb08880b4931baf37114a1e71cd` |
| `Contents/Resources/ShadowConfig.plist` | `94db85a95c1ea237d253187c22498cb374ae783848d92e88244a402b0fe00cf4` |
| `Contents/MacOS/FreeRDPShadowMenu` | `8f5a36776de8216d79eb98a608e5066c8cf30334c9551503c527bb937d1a6c45` |
| `Contents/MacOS/freerdp-shadow-cli` | `cdd492d6b6414af3c3486fd54e8e74a46440cfe4112ae8d907c007bac7464c6c` |
