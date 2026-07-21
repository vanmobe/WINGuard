# WINGuard Architecture

> Guard every take. Faster setup, safer record(w)ing!

## Overview

This plugin is a modular C++ REAPER extension with clear separation between:

- REAPER extension lifecycle
- OSC communication with WING
- Track and routing logic
- Configuration/state management
- Platform-dependent UI integration

## Source Layout

- `src/extension/`
  - REAPER entrypoint and command registration
  - extension lifecycle and command handling
- `src/core/`
  - OSC transport, message build/parse, routing
- `src/track/`
  - track creation/update and related synchronization logic
- `src/utilities/`
  - config loading, logging, platform helpers, string utils
- `src/ui/`
  - dialog bridge plus native platform dialog implementations

Headers are split between:

- `include/wingconnector/` (public-facing module interfaces)
- `include/internal/` (internal implementation headers)

## Runtime Flow

1. REAPER loads plugin via `REAPER_PLUGIN_ENTRYPOINT`.
2. API function pointers are resolved.
3. Extension singleton initializes configuration and runtime components.
4. REAPER actions are registered for the main WINGuard flow and the standalone selected-channel bridge setup flow.
5. User triggers an action; the matching UI or bridge entry point starts.
6. OSC queries fetch channel data from WING.
7. Track manager creates/updates REAPER tracks.
8. Optional auto-trigger and virtual soundcheck actions operate from dialog controls.
9. Optional MIDI CC transport/marker control is handled directly by plugin MIDI hooks/capture (with WING custom-button command syncing).
10. After a channel-based setup is applied, a managed-source watcher polls only the managed WING channels, tolerates brief all-channel or per-channel read glitches, and reapplies routing when compatible source changes are detected.
11. The main-thread timer also polls managed channels for ALT-source state so Soundcheck Mode stays synchronized when it is changed directly on WING.

## UI Strategy

- macOS: native Objective-C++ dialogs in `src/ui/*_macos.mm`
- Windows: native Win32 main dialog in `src/ui/wing_connector_dialog_windows.cpp`, routed from `dialog_bridge`

This keeps the product on native UI surfaces for both supported platforms while shared extension/core logic remains platform-neutral.

## Naming Conventions

- Operator-facing product copy, action labels, dialog titles, log prefixes, and undo labels should use `WINGuard`.
- Compatibility-sensitive technical identifiers should stay stable unless there is an explicit migration plan. This includes REAPER custom action `idStr` values such as `_AUDIOLAB_VIRTUALSOUNDCHECK_MAIN_DIALOG`.
- Legacy `AUDIOLAB...` identifiers may remain where REAPER bindings or persisted integration points depend on them, but they should not be reused for new visible copy.

## Build and Packaging

Build system: `CMakeLists.txt`

- Targets plugin as shared library:
  - `.dylib` on macOS
  - `.dll` on Windows
- Links platform dependencies conditionally.
- Builds the Windows release as x64 with a statically linked MSVC runtime, and validates the staged DLL before installer creation.
- Uses `oscpack` and REAPER SDK headers.

CI/CD:

- `.github/workflows/ci.yml`: build validation on macOS/Windows
- `.github/workflows/release.yml`: tagged release packaging + GitHub release assets

Packaging scripts:

- `packaging/create_installer_macos.sh`
- `packaging/create_installer_windows.ps1`

## Design Notes

- OSC parsing/routing is centralized in `src/core/`.
- Track operations are kept separate from transport/protocol concerns.
- Dialog and platform utilities isolate UI/platform complexity from core behavior.
- Configuration is file-based (`config.json`) for predictable deployment.
- WING shortcut command assignment is plugin-managed (not dependent on REAPER action-list shortcuts being reloaded at runtime).
- Managed source monitoring is orchestrated in `src/extension/` and reuses the existing `src/core/` routing/allocation code instead of maintaining a second routing engine.
- Selected-channel bridge work is intentionally isolated from live soundcheck and recorder flows until the WING event source is protocol-confirmed.
