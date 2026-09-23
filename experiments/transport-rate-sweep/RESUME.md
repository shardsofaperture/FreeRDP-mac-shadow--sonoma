# RateSweep preparation handoff — 2026-09-22 22:50 EDT

This is the historical preparation checkpoint. The 150 KiB/s physical run has
since completed; [CURRENT.md](CURRENT.md) records the installed app, result,
and next boundary. The hashes below describe artifacts at preparation time;
the offline parser and its test changed after the first capture.

## Exact stop state

Preparation is complete; the physical campaign has **not** begun. The repo is
`/Users/zach/gitr/RDP/FreeRDP-mac-shadow--sonoma`, branch `master`, HEAD
`c72de69d5066a5a48346385484f3becf8f424483`, with preexisting uncommitted
latency work preserved and the RateSweep changes uncommitted. The production
build directory is `build-macos-shadow-rate-sweep-package` (Release,
`WITH_JSONC_REQUIRED=ON`, `BUILD_TESTING=OFF`). The regression build is
`build-macos-shadow-release-checks` (Release/json-c with testing enabled).

The **installed** app remains `/Users/zach/Applications/FreeRDP Shadow.app`,
BuildLabel `0.2.0N — Coverage scheduler / Fixed 150 KiB/s`. At stop, its menu
PID was 66205 and its server PID was 66212, listening only on
`127.0.0.1:3390`. Those PIDs are observations, not future assumptions. The
active tunnel's sshd PID was 69522 at the read-only discovery pass; the later
scripts discover it afresh. The installed app and listener were not stopped,
swapped, installed, or restarted. No RateSweep bundle was launched. No
`nettop`, netstat capture, or other foreground logging process is running.
Nothing was committed, tagged, pushed, or released.

## Prepared bundle and exact hashes

Single diagnostic bundle:
`dist/FreeRDP Shadow 0.2.0-RateSweep.app`. Signed static BuildLabel:
`0.2.0-RateSweep — select fixed rate at launch`. Bundle ID:
`io.freerdp.shadow.sonoma.menu`. Authority:
`Apple Development: shardsofaperture (H7V72A5WH6)`. Team:
`66GMSP473V`. Deep/strict signing and all recorded SHA-256 checks pass.

| Artifact | SHA-256 |
| --- | --- |
| bundle `Contents/Info.plist` | `6102da9ee4be6387d7098110fd807b301357df437850c6eae4b8dfdaec11c09c` |
| bundle `Contents/Resources/ShadowConfig.plist` | `b33590662409b1d46dd0c3d658cc1f768c537cc70fe2e092b0c19b4b8f061b68` |
| bundle `Contents/Resources/SourceProvenance.json` | `59fe8f54c3b55ac85227ae21f3ffb85f9c69a303ace625bc52ccf3d51f3c629b` |
| bundle `Contents/MacOS/FreeRDPShadowMenu` | `7a677565d577d05b531de47d08289383233778e90fddb7feb45673531748732b` |
| bundle `Contents/MacOS/freerdp-shadow-cli` | `9acc37182e787b6f5e8588b92650c5a6be373ee1da6d9b66bdec0b140bdf4c54` |
| `dist/FreeRDP Shadow 0.2.0-RateSweep.source.patch` | `6776391f5ac666ef5728c985d5d57093fc7784505b53ea9e923e2928ff483262` |
| `dist/FreeRDP Shadow 0.2.0-RateSweep.sha256.txt` | `ee9be069b0413ac90adc0de7b00d6d8f6118b49721ca8866b2afa7a3ec846873` |
| `experiments/transport-rate-sweep/select-rate.zsh` | `1274177b822b839d4432ffa6c6d443dad54c101dcb1c657f2034110802fba701` |
| `experiments/transport-rate-sweep/discover.py` | `fe1403557883535ca1e24368879120583777bdabecb89724a7bf24dcefef30f4` |
| `experiments/transport-rate-sweep/capture.py` | `6e8d54f9f42a1886faf0c87d8b0db6583e0edfda51391d51f697da482eb6c7a0` |
| `experiments/transport-rate-sweep/analyze.py` | `16e23e5b942c8f40652f148f1e4f48d1993c1b773d640f05256e2aa0874765eb` |
| `experiments/transport-rate-sweep/README.md` | `171690e172948b7597b0c78f858b4fd488e26d9fe4de1a1f22cbf9da3cc0c31f` |
| `experiments/transport-rate-sweep/test_harness.py` | `292cf50e9b17fe4d3cc7cdbc601632a2b955d1ccdd4b9ac1a713cc3a1af15f7d` |

The signed source provenance hash matched the live candidate source snapshot
before this handoff file was written. The separate SHA-256 sidecar records the
bundle component paths and source patch. The signed bundle's static config is
`FixedRuntime`, rate `0` as a required-selection sentinel, `LargeRefreshBurst`
false, `CoverageScheduler` false, `SocketSendBufferKiB` zero (system default),
and listener `127.0.0.1:3390`. The launch argument supplies exactly one rate:
150, 200, 250, 300, or 400 KiB/s, which are 153600, 204800, 256000, 307200,
and 409600 B/s. The running menu label shows the chosen rate. No adaptive
probing or large-refresh burst is enabled. The fixed pacer's ordinary bounded
credit formula follows its selected rate; no separate burst policy is enabled.

## Checks completed

- Production packager verified `-O3 -DNDEBUG` and disabled debug/sanitizer
  instrumentation across 388 compiler commands; signed Release/json-c app
  built successfully. Bundle ID, authority, team, loopback config, fixed-rate
  sentinel, burst false, coverage false, and system-default socket setting
  were inspected in the actual package.
- Targeted Release/json-c CTest suite passed **19/19**, including bitmap,
  pacer, M, publication pacing, workload, coverage, socket, Mac publication,
  clipboard, region, codec, and WinPR synchronization tests. The socket test
  used an isolated ephemeral loopback port.
- Standalone selector test passed all five exact B/s mappings and ten invalid
  argument cases. Offline harness tests passed **2/2** (discovery row parsing,
  nettop/queue parsing with synthetic data). A read-only live discovery pass
  correctly identified the active sshd and external connection.
- Python compile checks, `zsh -n` for the swap script, `git diff --check`,
  package SHA-256 sidecar, and deep/strict codesign verification passed.
- Actual `nettop` CSV and physical queue/display measurements remain
  unvalidated because capture was explicitly prohibited during preparation.

## Prepared scripts

- [select-rate.zsh](select-rate.zsh): one argument, validates/stages the signed
  bundle before stopping the current app, backs it up, swaps only the installed
  path, launches with the selected rate, and verifies the installed listener.
- [discover.py](discover.py): dynamically finds tunnel sshd and matching
  external SSH connection.
- [capture.py](capture.py): takes the same rate argument, starts `nettop` delta,
  netstat queues, and relevant server log capture in a timestamped `runs/`
  directory, prints READY, and stops only its children on Ctrl-C.
- [analyze.py](analyze.py): offline B/s, queue, RTT, retransmit, and gap report.
- [README.md](README.md): physical sequence and interpretation limits.
- [test_harness.py](test_harness.py): offline parser/discovery fixtures.

## Resume at the physical boundary

The first command Codex should execute after resume is:

```zsh
cat '/Users/zach/gitr/RDP/FreeRDP-mac-shadow--sonoma/experiments/transport-rate-sweep/RESUME.md'
```

Then make **one** read-only verification pass of installed BuildLabel, current
listener PID/path, bundle SHA-256 sidecar and signing, and absence of capture
processes. PIDs may have changed. With local console access and the user's
physical participation, use [README.md](README.md) to begin at 150 KiB/s.
Each capture prints `READY: perform one large-window promotion now.` The user
must perform that one matched window promotion, wait for convergence and a few
more seconds, then Ctrl-C. Parse and preserve each result. Advance through
200, 250, 300, and 400 only after explicit instruction at each rate. No
rollback/checkpoint ceremony is needed; the swap script already preserves the
currently installed app.

**One prompt to paste on return:**

> Read `/Users/zach/gitr/RDP/FreeRDP-mac-shadow--sonoma/experiments/transport-rate-sweep/RESUME.md`. Verify once that the installed app/runtime still match its recorded stop state and that the RateSweep bundle hashes/signature are intact. Then begin the prepared physical throughput campaign at 150 KiB/s. Stop at each `READY: perform one large-window promotion now.` point for my action; do not advance rates until I explicitly instruct you.
