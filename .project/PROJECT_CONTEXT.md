# Project Context

## System Purpose

- WINGuard is a C++17 REAPER extension that connects to a Behringer WING console over OSC/UDP.
- Primary outcomes are stable REAPER extension behavior, reliable channel sync, virtual soundcheck routing, recorder helpers, and selected-channel bridge support.
- The product is embedded in REAPER, so host stability and predictable operator workflow matter more than broad feature expansion.

## Major Subsystems

- `src/extension/`: REAPER lifecycle, command registration, runtime orchestration, managed-source monitoring.
- `src/core/`: WING OSC transport, query/reply handling, routing, and protocol-specific behavior.
- `src/track/`: REAPER track creation, updates, and sync from WING data.
- `src/utilities/`: config, logging, platform helpers, string helpers.
- `src/ui/`: native macOS dialogs and Windows dialog/bridge surfaces.

## Architectural Constraints

- Keep extension, core, track, config, and UI boundaries intact; prefer small targeted changes over refactors.
- Do not migrate away from JSON config persistence unless explicitly requested; runtime config is loaded and saved through `src/utilities/wing_config.cpp`.
- Keep WING protocol behavior aligned with implementation truth and `docs/WING_OSC_PROTOCOL.md`.
- Preserve REAPER extension stability first; do not let optional integrations break transport or connection flows.

## Product And Workflow Constraints

- The main user flow is scan/select/manual-IP, connect, fetch/sync tracks, then optionally configure soundcheck, recorder helpers, MIDI actions, and auto-trigger.
- Virtual soundcheck and selected-channel bridge are intentionally separate workflows.
- Buses and matrices may participate in record-only flows; do not assume they are full soundcheck-capable sources without verifying implementation and docs.
- Managed-channel source monitoring and soundcheck-mode sync are expected to preserve existing prepared projects rather than rebuild them implicitly.

## Platform And Host Fit

- This product is a REAPER desktop plugin, not a standalone app or web service.
- macOS UI uses native Objective-C++ dialogs.
- Windows uses the native Win32 main dialog routed from the dialog bridge.
- Prefer host-native platform behavior over introducing a custom cross-platform UI framework.

## Supported Platforms And Parity Rules

- Officially supported platforms are macOS and Windows.
- `CMakeLists.txt` explicitly limits builds to macOS and Windows.
- Cross-platform parity is expected for the main connection and REAPER setup workflow.
- Platform-specific UI implementations are allowed, but feature behavior should stay aligned unless a difference is explicitly documented as intentional.
- Validation should include successful build on the changed platform and manual REAPER verification when runtime behavior changes.

## Native UI Decisions

- The current macOS main-dialog implementation is the visual and information-architecture reference. Older checked-in screenshots may show the retired single-page UI and are not authoritative.
- Cross-platform parity means the same four visible tabs and order (`Console`, `Reaper`, `Wing`, `Control Integration`), form hierarchy, operator copy, spacing rhythm, staged states, and interaction semantics. Pixel-identical platform chrome is not a goal.
- The optional selected-channel Bridge tab remains hidden in the main window. Bridge behavior stays a separate workflow unless a future product decision explicitly changes that boundary.
- Windows intentionally retains a compact 112-DIP header rather than copying the macOS 152-point header literally. Its composition still follows macOS: 40-DIP mark, title/subtitle, and four vertically stacked status rows.
- The shared main-window geometry contract is an 860×780 preferred size, an 820×560 monitor-clamped minimum, a compact 760-unit content surface, 20-unit page margins, a 180-unit label column, and controls beginning at x=220.
- Keep interactive UI native: AppKit controls on macOS and Win32 common controls on Windows. On Windows, use `WC_TABCONTROL` and push-like auto-radio buttons for segmented choices; limit custom painting to decorative header, status-card, callout, and divider surfaces.
- The Windows-only pinned footer is an intentional operational-feedback surface even though macOS has no exact counterpart. Do not remove it without first relocating transient progress and error feedback.
- Header and tab statuses are projections of applied configuration, staged edits, asynchronous work, and validation state. Pending recorder, Auto Trigger, and control-integration edits must remain visibly distinguishable from applied state.

## Windows DPI And Layout Contract

- REAPER owns process DPI awareness. Extension UI must not change process-wide or persistent thread-wide DPI awareness.
- Every Windows top-level dialog owns its current DPI and font handles, handles `WM_DPICHANGED` and `WM_SETTINGCHANGE`, applies the suggested DPI rectangle, and rebuilds its fonts before relayout. Never share a DPI-specific font handle between top-level windows.
- Win32 messages provide physical pixels; layout and scroll state use 96-DPI device-independent units. Convert at the boundary and avoid mixing physical rectangles with logical document coordinates.
- Font roles derive from Windows `NONCLIENTMETRICS` rather than a hard-coded face or point size. Decorative dimensions scale with display DPI, and long pages scroll vertically instead of compressing the form grid until text clips.
- Windows Accessibility **Text size** is separate from Display scaling. Current system-font/DPI handling must not be described as verified 100–225% accessibility text scaling until it is explicitly implemented or validated in REAPER on Windows.
- Automated coverage lives in `windows_ui_layout_tests` and runs in Windows CI. Native release validation still requires REAPER checks at 100%, 125%, 150%, and 200%, including small work areas, long text, auxiliary dialogs, and a mixed-DPI monitor move; 150% is the regression gate for clipped text.

## API And Data Conventions

- WING OSC is fixed at port `2223` in current runtime behavior.
- Discovery still uses the WING handshake probe on UDP `2222`.
- Config compatibility is centered on `config.json`; legacy listener-port `2224` is migrated in memory and rewritten to `2223`.
- Persistent REAPER track metadata keys such as `P_EXT:WINGCONNECTOR_SOURCE_ID` and `P_EXT:WINGCONNECTOR_ADOPTED_IN_PLACE` are compatibility-sensitive and should be treated as durable contracts.

## Security, Compliance, And Observability Expectations

- The plugin communicates with WING over local-network UDP and stores operator settings in local JSON config only.
- There is no evident special compliance regime in repo context, but changes should avoid expanding trust boundaries casually.
- Preserve operator diagnosability through REAPER-visible status/log messaging, especially for connection and routing failures.

## Release And Rollout Norms

- Keep compatibility with existing prepared REAPER projects and existing `config.json` content when possible.
- Tagged releases are packaged through GitHub Actions and platform packaging scripts.
- Non-trivial changes should meet the repo minimum validation bar of a successful build.
- After changes that produce a new plugin binary, rebuild and install the plugin into the REAPER `UserPlugins` path unless the user explicitly says not to.

## GitHub Delivery Workflow

- Repository: `vanmobe/AUDIOLAB-Virtual-Soundcheck-for-WING-Reaper`.
- GitHub Actions are used for CI and tagged release packaging.
- GitHub Project delivery metadata lives in `.project/github-project-config.json`.
- The active delivery project is `Wing Reaper Integration` (project number `4` under `vanmobe`).

## Testing And Validation Norms

- There is no strong automated regression suite; avoid speculative changes that cannot at least be build-verified.
- Minimum validation after non-trivial changes is a successful platform build.
- Runtime-sensitive changes should also be checked manually inside REAPER against a reachable WING or a realistic operator setup.
- When compatibility-sensitive behavior changes, verify existing config loading, managed track metadata reuse, and cross-platform dialog flow expectations.

## Known Non-Goals

- Do not flatten the module structure into a single mixed layer.
- Do not replace `config.json` persistence with a new storage model without explicit direction.
- Do not treat unverified WING OSC paths as supported product behavior just because they exist in reference material.
- Do not expand platform support based on older fallback-language in historical material; current build support is macOS and Windows only.
