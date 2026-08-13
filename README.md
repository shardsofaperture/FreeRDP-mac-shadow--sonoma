# FreeRDP macOS Shadow Server for Sonoma

FreeRDP-mac-shadow--sonoma is a focused FreeRDP fork for sharing the physical
desktop of a macOS 14 Sonoma machine with Microsoft Remote Desktop 5.2 on
Windows 98 SE.

The target is a pixel-sharp **1024×768 desktop at 16-bit color**, with low
interactive latency. The server is intended to listen only on
`127.0.0.1:3390`; access from the Windows 98 machine is carried through an SSH
tunnel. Legacy RDP security is required for this client, but must not weaken
FreeRDP's defaults or expose the listener to a LAN or the Internet.

> [!CAUTION]
> This project is an alpha-quality, machine-specific development build. It has
> been compiled and exercised on the target Intel Sonoma iMac with Microsoft
> RDP 5.2 on Windows 98 SE, but lifecycle, automatic display-mode switching,
> permissions, reconnect stress, and broader client compatibility are not yet
> complete. Keep the listener on loopback and reach it only through SSH.

## Status

The shadow CLI now compiles and links on macOS 14.7.1 on Intel. A physical
Windows 98 SE system running Microsoft RDP Client 5.2 has connected at
1024×768 and 16-bit color through an SSH tunnel. Desktop capture, classic
bitmap delivery, typing, modifiers, arrow keys, Caps Lock, mouse movement, and
clicking have received an initial hardware test.

Fine-grained pixel comparison prevents coarse Core Graphics damage rectangles
from building a multi-second queue in the legacy bitmap encoder. The macOS
input backend maintains private keyboard state, and the RDP client displays a
local system pointer so pointer movement is not tied to framebuffer latency.

Known alpha limitations include:

- automatic display-mode switching relies on explicitly configured local
  helper executables and has completed only an initial connect/disconnect test;
- the local Windows pointer can overlap the captured macOS pointer;
- macOS pointer-shape changes are not sent to the client;
- ordered capture/thread shutdown and repeatable lifecycle handling remain
  incomplete;
- Screen Recording and Accessibility permission failures lack dedicated
  preflight diagnostics; and
- reconnect, sleep/wake, input, latency, and bandwidth validation matrices are
  not complete.

See the [Sonoma and Windows 98 roadmap](docs/mac-shadow-sonoma-plan.md) for the
complete patch status, dependencies, defect analysis, exit criteria, and test
matrix.

## Alpha operation on the validated iMac

Grant Screen Recording and Accessibility permission to the Terminal used to
launch the server. On the validated machine, start the server at the normal
1920×1080 desktop with the two local display-mode helpers configured:

```zsh
cd /Users/zach/gitr/RDP/FreeRDP-mac-shadow--sonoma

FREERDP_MAC_SHADOW_CONNECT_DISPLAY_COMMAND=/usr/local/bin/w981 \
FREERDP_MAC_SHADOW_DISCONNECT_DISPLAY_COMMAND=/usr/local/bin/mac1080 \
  ./build-sonoma-shadow-p0-channels/server/shadow/cli/freerdp-shadow-cli \
  /bind-address:127.0.0.1 \
  /port:3390 \
  /sec:rdp \
  -auth \
  -gfx \
  -rfx \
  -nsc
```

`w981` and `mac1080` are local wrappers around `displayplacer`, not commands
installed by this repository. Both environment variables are required, and
each value must be an executable absolute path without arguments. The first
RDP client runs `w981`, resizes the shadow surfaces, and recreates capture at
1024×768. The last disconnect runs `mac1080` and recreates idle capture at
1920×1080. Do not add `+server-side-cursor` when using the low-latency
client-side pointer.

In Tera Term on Windows 98, connect SSH to the Mac's LAN address and configure
a local forwarding rule with these values:

| Setting | Value |
| --- | --- |
| Local port | `3390` |
| Remote host | `127.0.0.1` |
| Remote port | `3390` |

Keep that SSH session open. Microsoft RDP Client 5.2 then connects to
`127.0.0.1:3390` on Windows 98. The Mac's LAN address belongs in Tera Term,
not in the RDP client. Existing VNC forwarding on ports 5900–5902 is unrelated
and can remain configured but is not used by this RDP session.

When finished, disconnect the RDP client. The physical display returns to
1920×1080 automatically; the loopback server remains available for another
connection. Stop it with `Ctrl+C` in its Mac Terminal when it is no longer
needed.

The listener uses legacy RDP security for client compatibility and has no
authentication in this alpha command. It must remain bound to `127.0.0.1` and
must not be exposed directly to a LAN or the Internet.

## Menu-bar service on the validated iMac

The repository includes a small native menu-bar controller for macOS 13 and
later. It installs `FreeRDP Shadow.app` in the current user's Applications
folder, registers the app with macOS Service Management, and starts the
validated loopback server command after graphical login:

```zsh
cd /Users/zach/gitr/RDP/FreeRDP-mac-shadow--sonoma
./scripts/install-macos-shadow-menu.sh
```

The menu-bar item reads `RDP ●` while the server is running and `RDP ○` while
it is stopped. Its menu provides:

- **Start RDP Server** and **Stop RDP Server**;
- the current server status and fixed `127.0.0.1:3390` listener;
- access to the server log and required macOS Privacy settings; and
- a **Launch at Login** control.

The server starts by default whenever the menu app starts. Stopping it from
the menu leaves the controller available but keeps the server off for the
remainder of that login session. The next login starts it again. macOS cannot
capture a user's desktop before that user has logged into the graphical
session, so “start after restart” means immediately after login, not at the
pre-login/FileVault screen.

While the server is on but no RDP client is connected, it keeps only the
loopback listener running. Display capture starts for the first connected
client and stops after the last client disconnects. System-audio capture follows
the same connection lifetime, so macOS should show its recording indicator only
during an active RDP session.

System audio is available as a beta feature on macOS 14.2 and later. The Sonoma
backend uses a private Core Audio process tap, leaves local Mac playback
unmuted, converts the captured output to 44.1 kHz stereo 16-bit PCM, and sends
it through the standard static `rdpsnd` channel. This path has been validated
with iTunes and Microsoft Remote Desktop 5.2 on Windows 98. The current beta
requires the Mac output tap to provide 44.1 kHz stereo 32-bit float audio; if
that format is unavailable, video and input continue while the log reports
that audio capture could not start.

For an end-to-end channel diagnostic that does not depend on an application
playing audio, launch the server with `FREERDP_MAC_SHADOW_TEST_TONE=1`. This
replaces system-audio capture with a 440 Hz tone for that server run and is not
enabled by the menu app by default.

If the server process exits unexpectedly, the menu controller restarts it
automatically with a bounded delay that increases from 1 to 30 seconds for
repeated failures. The menu displays `RDP ↻` while recovery is pending. An
intentional **Stop RDP Server** or **Quit FreeRDP Shadow** cancels automatic
recovery.

On first installation, approve **FreeRDP Shadow** under both **System Settings
> Privacy & Security > Screen & System Audio Recording** and **Accessibility**.
Use **Request Capture and Input Permissions** or the two Privacy shortcuts in
the menu if the prompts were dismissed. macOS may require the app to be quit
and reopened after Screen Recording is enabled; then select **Start RDP
Server**.

The service continues to use `/usr/local/bin/w981` for first-client 1024×768
and `/usr/local/bin/mac1080` for last-disconnect 1920×1080 restoration. Its
append-only log is stored at:

```text
~/Library/Logs/FreeRDPShadow/server.log
```

The installed app references the executable in this repository's validated
build directory. Quit the menu app before rebuilding or reinstalling it, and
keep the repository at its current path. To disable automatic startup without
removing the app, turn off **Launch at Login** in its menu. To unregister the
login item and move the app to the Trash while preserving its log, run:

```zsh
./scripts/install-macos-shadow-menu.sh --uninstall
```

## Architecture

This fork retains FreeRDP's existing shadow-server architecture:

1. the macOS shadow backend captures the physical display and publishes changed
   pixels through an `rdpShadowSurface`;
2. the generic FreeRDP shadow server handles sessions, input, and transport;
3. at client color depths below 32 bits, the existing shadow client selects
   FreeRDP's interleaved classic bitmap encoder and divides updates into 64×64
   rectangles; and
4. the established command-line and security negotiation remain intact, with
   any legacy behavior confined to an explicit profile.

This path already matches the capabilities of Microsoft RDP 5.2. Repairing it
avoids inventing a protocol, avoids coupling the server to one client, and
preserves compatibility with current FreeRDP clients. A ScreenCaptureKit
adapter is deliberately deferred as a separate modernization phase; it can
replace capture later while preserving the same shadow-surface and classic
bitmap publication path.

## Project references

- [Detailed project roadmap](docs/mac-shadow-sonoma-plan.md)
- [Upstream FreeRDP source](https://github.com/FreeRDP/FreeRDP)
- [FreeRDP build documentation](docs/README.building)
- [Upstream compilation guide](https://github.com/FreeRDP/FreeRDP/wiki/Compilation)
- [Apache License 2.0](LICENSE)
- [FreeRDP project website](https://www.freerdp.com/)
- [Upstream FreeRDP contributors](https://github.com/FreeRDP/FreeRDP/graphs/contributors)

## Upstream attribution and license

This repository is a fork of
[FreeRDP](https://github.com/FreeRDP/FreeRDP), a free implementation of the
Remote Desktop Protocol. FreeRDP and this fork are distributed under the
[Apache License 2.0](LICENSE). The FreeRDP contributors, Microsoft Open
Specifications, and the wider interoperability community made the underlying
server, protocol, and codec work possible.
