# Releases

This folder is for local release artifacts when packaging manually.

Primary distribution is via GitHub Releases:

- https://github.com/vanmobe/colab.reaper.wing/releases

## Installer Types

- macOS: `WINGuard-MAC-v<version>.pkg`
- Windows: `WINGuard-WIN-v<version>.exe`

## How Releases Are Built

Tagged pushes (`v*`) trigger `.github/workflows/release.yml`, which:

1. Builds the plugin on macOS and Windows
2. Creates installer packages per platform
3. Publishes assets to the GitHub release for that tag

## Local Packaging (Maintainers)

- macOS: `packaging/create_installer_macos.sh`
- Windows: `packaging/create_installer_windows.ps1`

See [SETUP.md](../SETUP.md) for build prerequisites.
