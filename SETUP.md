# Developer Setup Guide

This guide covers building AUDIOLAB.wing.reaper.virtualsoundcheck from source and installing it into REAPER for development.

## Prerequisites

- CMake `3.15+`
- C++17 compiler
  - macOS: Xcode Command Line Tools
  - Windows: Visual Studio 2022 or Visual Studio 2022 Build Tools with the
    **Desktop development with C++** workload. This must include MSVC and a
    Windows 10 or Windows 11 SDK.
- Git (used to clone `oscpack`; a ZIP download also works)
- REAPER SDK headers:
  - `lib/reaper-sdk/reaper_plugin.h`
  - `lib/reaper-sdk/reaper_plugin_functions.h`
- `oscpack` source in `lib/oscpack`

## Dependency Setup

### macOS/Linux shell

```bash
./setup_dependencies.sh
```

The script prompts for the two REAPER SDK headers and clones `oscpack` with
Git.

### Windows

Install CMake and the Visual C++ build toolchain. For example, from an
Administrator PowerShell prompt with Windows Package Manager available:

```powershell
winget install --id Kitware.CMake --exact
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Open a new terminal after installation so `cmake` is on `PATH`. Alternatively,
invoke it as `C:\Program Files\CMake\bin\cmake.exe`.

The dependency setup script is a Bash script, so on native Windows place the
dependencies manually:

1. Download `reaper_plugin.h` and `reaper_plugin_functions.h` from the
   [REAPER extension SDK](https://www.reaper.fm/sdk/plugin/) into
   `lib/reaper-sdk/`.
2. Clone [oscpack](https://github.com/RossBencina/oscpack) into
   `lib/oscpack/`, or download and extract its source ZIP there. The directory
   must contain `lib/oscpack/osc/` and `lib/oscpack/ip/` directly, without an
   extra `oscpack-master` directory level.

Then verify:

- `lib/reaper-sdk/reaper_plugin.h`
- `lib/reaper-sdk/reaper_plugin_functions.h`
- `lib/oscpack/osc/OscOutboundPacketStream.h`

## Build

macOS:

```bash
./build.sh
```

Windows:

```bat
build.bat
```

## Install Built Plugin to REAPER

macOS:

```bash
mkdir -p ~/Library/Application\ Support/REAPER/UserPlugins
cp install/reaper_wingconnector.dylib ~/Library/Application\ Support/REAPER/UserPlugins/
cp config.json ~/Library/Application\ Support/REAPER/UserPlugins/
```

Windows:

```bat
mkdir "%APPDATA%\REAPER\UserPlugins"
copy install\reaper_wingconnector.dll "%APPDATA%\REAPER\UserPlugins\"
copy config.json "%APPDATA%\REAPER\UserPlugins\"
```

Runtime config precedence:

1. `UserPlugins/config.json`
2. `~/.wingconnector/config.json`

Development implication:

- If you copy a development `config.json` into `UserPlugins`, that copy overrides any fallback config in `~/.wingconnector/`.
- If you want to test fallback behavior, remove or rename the `UserPlugins` copy first.

## Verify in REAPER

1. Restart REAPER.
2. Confirm `Extensions -> AUDIOLAB.wing.reaper.virtualsoundcheck` is present.
3. Run connect flow and verify channels/tracks sync.

## Packaging and Release

- CI build matrix: `.github/workflows/ci.yml`
- Release packaging: `.github/workflows/release.yml`
- Packaging scripts:
  - `packaging/create_installer_macos.sh`
  - `packaging/create_installer_windows.ps1`

Release tags matching `v*` trigger installer build + publish.

## Common Build Failures

- Missing REAPER SDK headers in `lib/reaper-sdk/`
- Missing `oscpack` checkout in `lib/oscpack/`
- Compiler toolchain not installed or not on PATH
- CMake not found after installation: open a new terminal or use the full
  `C:\Program Files\CMake\bin\cmake.exe` path
- Windows CMake reports no C++ compiler: modify Visual Studio Build Tools and
  add the **Desktop development with C++** workload, including MSVC and a
  Windows SDK
- Platform-specific packaging tools missing (`pkgbuild`, Inno Setup)

## Config Troubleshooting

- If a packaged install and a local development build appear to use different settings, compare both `UserPlugins/config.json` and `~/.wingconnector/config.json`.
- If both files exist, WINGuard loads the `UserPlugins` copy.
- When neither file exists yet, new saves default to the `~/.wingconnector/config.json` path.

## Related Documentation

- [README.md](README.md) for the project overview
- [docs/README.md](docs/README.md) for the documentation map
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for code structure and runtime flow
- [docs/WING_OSC_PROTOCOL.md](docs/WING_OSC_PROTOCOL.md) for protocol details
