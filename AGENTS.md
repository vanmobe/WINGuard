# AUDIOLAB Wing Connector for REAPER

## Project Summary

This repository contains a C++17 REAPER extension that connects to a Behringer WING console over OSC/UDP. The plugin handles channel discovery and sync, virtual soundcheck routing, transport and auto-record helpers, recorder integration, and a selected-channel MIDI bridge workflow.

## What Matters Most

- Preserve REAPER extension stability first.
- Keep WING OSC behavior aligned with the implemented protocol and current docs.
- Prefer small, targeted changes over broad refactors.
- Maintain cross-platform behavior:
  - macOS uses native Objective-C++ dialogs.
  - Windows uses native Win32 dialogs routed through the dialog bridge.
- Do not migrate away from `config.json` unless explicitly asked. The runtime currently loads and saves JSON config through `src/utilities/wing_config.cpp`.

## Code Map

- `src/extension/`
  - REAPER lifecycle, command registration, runtime orchestration
- `src/core/`
  - OSC transport, routing, parsing, WING protocol behavior
- `src/track/`
  - REAPER track creation and synchronization
- `src/utilities/`
  - config, logging, platform helpers, string helpers
- `src/ui/`
  - platform dialog bridge plus native macOS and Windows dialogs

## Key Files

- `src/extension/reaper_extension.cpp`
- `src/core/wing_osc.cpp`
- `src/utilities/wing_config.cpp`
- `include/wingconnector/wing_config.h`
- `src/ui/wing_connector_dialog_macos.mm`
- `src/ui/dialog_bridge.cpp`
- `docs/ARCHITECTURE.md`
- `docs/WING_OSC_PROTOCOL.md`

## Build And Validation

- Setup dependencies: `./setup_dependencies.sh`
- Build on macOS: `./build.sh`
- Build on Windows: `build.bat`
- Minimum validation bar after non-trivial changes: successful build
- After code changes that produce a new plugin binary, rebuild and install the resulting plugin into `~/Library/Application Support/REAPER/UserPlugins/reaper_wingconnector.dylib` by default so the latest build is ready for manual REAPER testing unless the user explicitly says not to install it.
- When CI, packaging, release assets, or GitHub Actions behavior matter, treat `.github/workflows/*.yml` as the canonical source of truth before relying on README text or issue phrasing.

Hard prerequisites expected by the build:

- `lib/reaper-sdk/reaper_plugin.h`
- `lib/reaper-sdk/reaper_plugin_functions.h`
- `lib/oscpack/osc/OscOutboundPacketStream.h`

## Working Rules

- Keep module boundaries intact; do not flatten extension/core/ui/config concerns together.
- Avoid new dependencies unless there is a strong reason.
- Keep comments sparse and high-signal.
- When behavior changes, update the docs that users actually read:
  - `README.md`
  - `QUICKSTART.md`
  - `docs/USER_GUIDE.md`
  - `docs/ARCHITECTURE.md` if the structure or flow changed
- Be cautious with protocol assumptions. If changing WING OSC behavior, confirm against the implemented code and `docs/WING_OSC_PROTOCOL.md`.

## Domain Notes

- The current implementation treats the WING OSC port as fixed to `2223`.
- Virtual soundcheck and selected-channel bridge are intentionally separated workflows.
- Buses and matrices may be record-only in some flows; do not treat them as full soundcheck-capable channels without verifying code and docs.
- There is no strong automated test suite here, so avoid speculative changes that cannot at least be build-verified.
