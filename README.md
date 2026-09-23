# Mac Shadow RDP

Mac Shadow RDP is a machine-specific FreeRDP shadow server for sharing the
physical desktop of a macOS Sonoma Mac with remote desktop clients. Release
1.0.0 is tuned for Microsoft Remote Desktop 5.2 on Windows 98 SE at 1024×768
and 16-bit color. Android and Microsoft Remote Desktop for Mac are additional
clients.

The server has no authentication in its legacy RDP compatibility mode. Keep it
bound to loopback and reach it through an authenticated SSH tunnel. Never expose
the listener to a LAN or the Internet.

## Production configuration

The signed 1.0.0 app uses:

- fixed graphics pacing at **250 KiB/s (256000 B/s)**;
- the system-default `SO_SNDBUF`;
- **50 ms latest-state publication aggregation** on ordinary Mac capture;
- ordinary/F bitmap scheduling, with input serviced ahead of graphics; and
- no adaptive probing, large-refresh burst, or coverage scheduler.

The aggregation coalesces capture damage and publishes the newest state. It
does not impose a 50 ms transport queue or change the byte pacing rate. First
frame, reconnect, and client refresh retain their bypass behavior. The
[architecture note](docs/mac-shadow-architecture.md) describes the path and
correctness boundaries; the [engineering history](docs/mac-shadow-engineering-history.md)
summarizes the measurements behind these settings.

## Build, test, and sign

Build the signed Release app on the target Mac:

```zsh
python3 scripts/build-macos-shadow-app.py
```

The output is `dist/FreeRDP Shadow 1.0.0.app`. The production build uses
Release optimization, required json-c, and the configured Apple Development
identity `Apple Development: shardsofaperture (H7V72A5WH6)`. Do not use
ad-hoc signing. The build writes source provenance and a SHA-256 manifest
beside the app and verifies the nested signature.

Configure a clean Release regression build and run the relevant tests with the
commands in [DEVELOPMENT.md](DEVELOPMENT.md). Keep test products outside the
production app. The [1.0.0 release record](docs/mac-shadow-1.0.0.md) lists the
tested configuration and current validation limits.

## Install and connect

Install the generated app at `~/Applications/FreeRDP Shadow.app`:

```zsh
./scripts/install-macos-shadow-menu.sh
```

The native menu app registers with macOS **SMAppService Launch at Login**. It
supervises the server and binds it to `127.0.0.1:3390`. Grant the app Screen
Recording and Accessibility permissions when prompted. Use the menu's
**Launch at Login** control to manage automatic startup. Capture begins when a
client connects; macOS must reach the user's graphical login before desktop
capture can start.

Forward the Mac's loopback listener through SSH. For example, configure the
client-side SSH tunnel to forward local port `3390` to `127.0.0.1:3390` on the
Mac, then connect the RDP client to its own `127.0.0.1:3390`. Keep the tunnel
open for the RDP session.

## Recovery

The repository keeps the signed 0.2.0 source tag and the earlier 0.1.9 recovery
tag. On the target Mac, signed 0.2.0, 0.1.9 Recovery, and 0.2.1A rollback app
bundles may also be retained under `dist/`; generated bundles are local
artifacts and are not source-controlled. To rebuild a prior release, use a
separate worktree at its tag and run the same signed build/install workflow.
The 1.0.0 release record identifies the preserved rollback bundle and signing
identity.

## Project documentation

- [Current architecture](docs/mac-shadow-architecture.md)
- [Engineering history and measured decisions](docs/mac-shadow-engineering-history.md)
- [1.0.0 release record](docs/mac-shadow-1.0.0.md)
- [Build and regression test instructions](DEVELOPMENT.md)
- [Upstream FreeRDP source](https://github.com/FreeRDP/FreeRDP)
- [Apache License 2.0](LICENSE)

This project is a FreeRDP fork distributed under the Apache License 2.0.
