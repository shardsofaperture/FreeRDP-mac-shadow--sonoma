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
> This project is not ready for installation or operational use. The capture
> and dirty-region repairs are implemented in source, but they have **not yet
> been compiled or runtime-tested on Sonoma**. The hardware baseline is the
> immediate blocker, and no installation or usage claims should be inferred
> until the validation matrix is complete.

## Status

The first two source repairs address `CGDisplayStream` callback correctness,
dirty-region lifetime, surface locking, and frame publication. Both remain
unvalidated on the target hardware.

The work still to be resolved includes:

- a reproducible warnings-enabled Sonoma build and hardware baseline;
- immediate first-frame delivery and reliable reconnect behavior;
- ordered capture/thread shutdown and repeatable lifecycle handling;
- correct keyboard, mouse, wheel, and modifier injection;
- explicit Screen Recording and Accessibility permission diagnostics;
- an isolated Windows 98 compatibility profile and security verification;
- latency and bandwidth measurement followed by performance tuning; and
- packaging, modernization, and optional later features only after correctness
  has been demonstrated.

See the [Sonoma and Windows 98 roadmap](docs/mac-shadow-sonoma-plan.md) for the
complete patch status, dependencies, defect analysis, exit criteria, and test
matrix.

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
- [FreeRDP project website and acknowledgments](https://www.freerdp.com/)

## Upstream attribution and license

This repository is a fork of
[FreeRDP](https://github.com/FreeRDP/FreeRDP), a free implementation of the
Remote Desktop Protocol. FreeRDP and this fork are distributed under the
[Apache License 2.0](LICENSE). The FreeRDP contributors, Microsoft Open
Specifications, and the wider interoperability community made the underlying
server, protocol, and codec work possible.
