# AUDIOLAB Wing Connector for REAPER

AUDIOLAB Wing Connector for REAPER is a C++ REAPER extension that connects to a Behringer WING console over OSC/UDP and automates track setup, channel sync, virtual soundcheck routing, and broader WING-driven workflows.

- Status: Production-ready
- Platforms: macOS, Windows
- Installers: `.pkg` (macOS), `.exe` (Windows)
- License: MIT

> Disclaimer: This software is provided as-is for use at your own risk. No guarantees or official support are provided.

## Install

For end users, use the installers from GitHub Releases:

- https://github.com/vanmobe/colab.reaper.wing/releases

Platform-specific steps are in [INSTALL.md](INSTALL.md).

## System Requirements

- REAPER 6.0+
- Behringer WING (Compact, Rack, or Full)
- Same-network connectivity between REAPER host and WING
- OS support:
  - macOS 10.13+
  - Windows 10+

## Quick Start

1. Install the plugin for your platform.
2. Restart REAPER.
3. Open the REAPER Actions list with `?`, search `WINGuard`, then run `WINGuard: Configure Virtual Soundcheck/Recording`.
4. Scan for a WING, select a discovered console or enter the WING IP manually, then connect and fetch channels.
5. Confirm the console is reachable on the fixed WING OSC runtime port `2223`.
6. Confirm tracks are created/updated in REAPER.

See [QUICKSTART.md](QUICKSTART.md) for the 5-minute flow.

## Key Features

- Automatic track creation from WING channel data
- Channel metadata sync (name, color, source-related info)
- Optional real-time monitoring for updates
- Virtual soundcheck setup for channels (USB/CARD routing + staged apply flow + validation status)
- Existing-project adoption with reviewable channel and playback-slot mapping
- Record-source selection for channels, buses, and matrices
- Optional WING MIDI CC control (Play/Record/Stop/Markers/Virtual Soundcheck) with automatic button command assignment
- Selected-channel-to-MIDI bridge configuration for SuperRack-style integration
- Cross-platform dialog behavior:
  - macOS: native Cocoa dialogs
  - Windows: native Win32 dialogs
  - the main window keeps the same tabs, visual hierarchy, and compact content grid on both platforms while retaining native controls

## User Documentation

- [docs/README.md](docs/README.md) - documentation map and reading order
- [INSTALL.md](INSTALL.md) - installer-first setup by platform
- [QUICKSTART.md](QUICKSTART.md) - shortest path to first connection
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md) - day-to-day operation in REAPER
- [docs/WING_SELECTED_CHANNEL_BRIDGE.md](docs/WING_SELECTED_CHANNEL_BRIDGE.md) - selected-channel bridge setup and behavior
- [docs/CC_BUTTONS_AND_AUTO_TRIGGER.md](docs/CC_BUTTONS_AND_AUTO_TRIGGER.md) - CC mapping, auto-trigger, and SD recording notes

## Developer Documentation

- [SETUP.md](SETUP.md) - local dev/build environment
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - code structure and runtime flow
- [docs/WING_OSC_PROTOCOL.md](docs/WING_OSC_PROTOCOL.md) - implemented OSC subset + reference notes

## Build From Source

Prerequisites:

- CMake 3.15+
- C++17 compiler
- REAPER SDK headers in `lib/reaper-sdk/`
- `oscpack` sources in `lib/oscpack/`

Build:

```bash
./build.sh
```

Windows:

```bat
build.bat
```

Then copy the plugin binary + `config.json` into your REAPER `UserPlugins` folder.
Details are in [SETUP.md](SETUP.md).

## CI and Release

- CI build matrix: `.github/workflows/ci.yml`
- Tagged release packaging: `.github/workflows/release.yml`
- Tag pattern: `v*` publishes installers as release assets

## Support

When opening an issue, include:

- OS and version
- REAPER version
- WING model + firmware
- Relevant log output from REAPER
- Steps to reproduce
