# Fixed-rate transport campaign

The 150–400 KiB/s physical campaign completed on 2026-09-23. See the
[comparison and recommendation](CAMPAIGN.md) and [current installed state](CURRENT.md).
The procedure below records the original preparation and manual operating
sequence; the 200–400 KiB/s runs used the later `capture.py --auto-stop`
harness with bounded queue recovery detection.

This is a manual, one-action-at-a-time comparison of F-style fixed graphics
pacing at 150, 200, 250, 300, and 400 KiB/s. A later operator must have local
console access before using `select-rate.zsh`, because each swap stops the
currently installed RDP server and therefore the RDP tunnel. Preparation did
not execute the swap or capture scripts.

The single signed `dist/FreeRDP Shadow 0.2.0-RateSweep.app` accepts exactly one
launch argument, `--rate-sweep-kib={150|200|250|300|400}`. A missing or invalid
argument exits before server startup. The signed bundle's static BuildLabel is
`0.2.0-RateSweep — select fixed rate at launch`; the running menu displays
`0.2.0-RateSweep — Fixed N KiB/s`. The static plist records `FixedRuntime`, rate
zero as a required-selection sentinel, system-default socket buffer, burst
false, coverage false, and `127.0.0.1:3390`. The menu sets only the fixed-rate
environment variable after parsing its launch argument. The server's existing
fixed pacer then uses N×1024 B/s, with its ordinary bounded credit formula and
no adaptive probing or large-refresh burst. Ordinary F bitmap traversal,
compression, publication, input/channel priority, clipboard, audio, and SSH
settings are unchanged.

## Historical prepared sequence (superseded)

From the repo directory, with the phone connected through SSH and a local
console available:

```zsh
zsh experiments/transport-rate-sweep/select-rate.zsh 150
python3 experiments/transport-rate-sweep/capture.py 150
```

When capture prints **`READY: perform one large-window promotion now.`**, the
user performs exactly one matched large-window promotion. Wait for visible
convergence plus several seconds, then press Ctrl-C in the capture terminal.
Run the parser on the printed evidence path:

```zsh
python3 experiments/transport-rate-sweep/analyze.py 'experiments/transport-rate-sweep/runs/<timestamp>-150KiB'
```

Record the visible time/banding, text readiness, and mouse/menu response by
hand next to the parser output. Only after explicit user instruction, repeat
the same three commands for 200, then 250, 300, and 400 KiB/s. Do not
automatically advance rates. A candidate is sustainable only if its external
Send-Q and loopback queue drain promptly after the action, RTT/retransmits do
not degrade, and input and display experience remain acceptable. The parser
does not claim display timing from transport counters.

`select-rate.zsh` validates the signed source and a staged copy before it
quits the old app. It waits for the old listener to stop, preserves the prior
installed app under `~/Applications/FreeRDP Shadow.before-rate-*.app`, installs
only at `~/Applications/FreeRDP Shadow.app`, prints and verifies the installed
static BuildLabel/config/signature, launches only that installed path with the
explicit rate argument, and checks both listener executable path and menu
argument. Invalid rates exit before any staging or process action.

`discover.py` identifies the active sshd process from its
`127.0.0.1:<ephemeral>→127.0.0.1:3390` TCP connection, then identifies the
same process's external SSH connection. It fails if either match is ambiguous.
`capture.py` creates a new timestamped `runs/` directory, records selected
rate/BuildLabel/start/stop UTC and both endpoints, retains raw `nettop -d -x`
CSV, sampled `netstat -anv -p tcp` rows, relevant server log lines, and nettop
stderr. Ctrl-C terminates its own child processes; it never changes app/rate.
`analyze.py` reads those files offline. Transport rates are B/s (and selected
rate is also printed in KiB/s). RDP loopback bytes are plaintext; external
SSH bytes include encryption and protocol overhead. Queue drain is a
peak-to-near-empty proxy from sampled queue rows; nettop CSV availability is
reported honestly if a field is absent. The exact macOS nettop CSV emitted in
the later physical run was intentionally not collected during preparation.

Use [CURRENT.md](CURRENT.md) for the installed/runtime state after the campaign.
[RESUME.md](RESUME.md) retains the preparation-time artifact hashes and tests;
its recorded installed/runtime state is historical.
