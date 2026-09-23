# RateSweep campaign handoff — 2026-09-23

The campaign's 250 KiB/s result has been promoted to the signed, static
[0.2.0 production app](../../docs/mac-shadow-0.2.0.md). The current installed
path is `/Users/<redacted>/Applications/FreeRDP Shadow.app`, with BuildLabel
`0.2.0 — Fixed 250 KiB/s` and no rate-selection launch argument. The
RateSweep runtime state below records the earlier campaign close, before
production promotion.

The five-rate physical campaign is **complete**. Its comparison, interpretation,
limitations, and preserved run links are in [CAMPAIGN.md](CAMPAIGN.md); exact
recomputed figures are in [campaign-metrics.json](campaign-metrics.json).
The recommended next engineering baseline is **250 KiB/s**. This is the
highest tested rate that kept loopback and external queues low in the observed
promotion. The first transient loopback pressure appeared at 300 KiB/s; 400
KiB/s held a larger queue through the active refresh without a visible speed
gain. These are a few physical actions, including a return switch at 300,
not a proven universal ceiling.

The signed RateSweep app was selected back to **250 KiB/s** at campaign close.
At that point, read-only verification
found menu PID 37721 launched with `--rate-sweep-kib=250`, child listener PID
37728 at `127.0.0.1:3390`, and an established sshd forward. Both PIDs are
historical observations. The installed static BuildLabel is
`0.2.0-RateSweep — select fixed rate at launch`; the selected runtime label is
`0.2.0-RateSweep — Fixed 250 KiB/s`. `codesign --verify --deep --strict`
passed, and its signer was `Apple Development: shardsofaperture (H7V72A5WH6)`
with team `66GMSP473V`. The installed config retains FixedRuntime, selected
rate sentinel zero, disabled burst/coverage, default SO_SNDBUF, and loopback
binding. The 250 run's server log recorded `mode=fixed baseRate=256000`,
adaptive probing disabled, zero burst entries, and `sndbufRequest=0`.

The previous installed 400 KiB/s app is preserved at
`/Users/<redacted>/Applications/FreeRDP Shadow.before-rate-20260923-074027.app`;
the earlier N backup is at
`/Users/<redacted>/Applications/FreeRDP Shadow.before-rate-20260923-071032.app`.
No campaign capture, nettop, or netstat child remained at campaign close. The
50 ms publication aggregator was not implemented. No commit, tag, push, or
release was made during the campaign. At campaign close, the repository was on
branch `master`, HEAD
`c72de69d5066a5a48346385484f3becf8f424483`, with pre-existing unrelated
worktree changes preserved.

The [original RESUME.md](RESUME.md) describes the historical prepared stop
state before the physical campaign; its installed-N state is superseded by
this file. Further engineering should start from the 250 KiB/s measured
baseline and use the preserved raw runs to choose a next isolated experiment.
