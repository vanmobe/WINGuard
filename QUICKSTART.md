# Quick Start (5 Minutes)

This guide gets you from install to first successful channel sync.

> WINGuard: Guard every take. Faster setup, safer record(w)ing!

## 1. Prepare the WING

On the Behringer WING console:

1. Open `Setup -> Remote -> OSC`.
2. Ensure OSC remote lock is disabled.
3. Confirm the console is using the fixed WING OSC runtime port `2223` required by WINGuard.
4. Note the WING IP address.
5. If you plan to use button MIDI actions, set `External MIDI Control` to `USB`.

## 2. Install WINGuard

Use the installer for your OS from GitHub Releases:

- https://github.com/vanmobe/colab.reaper.wing/releases

See [INSTALL.md](INSTALL.md) if needed.

## 3. Launch REAPER and Connect

1. Restart REAPER after installation.
2. Open the REAPER Actions list with `?`, search `WINGuard`, then run `WINGuard: Configure Virtual Soundcheck/Recording`.
3. Run `Scan`, then select a discovered WING or enter the WING IP manually.
4. Run channel fetch / connect in the dialog.

Fastest open path:

- Main action shortcut: `Cmd+Shift+W` on macOS, `Ctrl+Shift+W` on Windows

Expected result:

- WINGuard connects successfully.
- REAPER tracks are created or refreshed from WING channel data.

This scan/select/manual-IP/connect flow is the standard WINGuard connection workflow on supported platforms.

## 4. Optional: Configure Virtual Soundcheck

Inside the WINGuard dialog:

1. Configure soundcheck output mode (`USB` or `CARD`).
2. Run recording setup for the sources you want to capture.
3. Use soundcheck mode toggle (ALT source switching) when needed.
4. If you reopen an existing WINGuard-managed project later, connect first and look at the Reaper tab status:
   - if the setup validates, you can switch `Live Mode` / `Soundcheck Mode` directly
   - if you changed `USB` or `CARD` mode, use `Rebuild Current Setup` to reuse the current managed selection
   - if validation warns about topology or unreadable managed sources, rebuild the current managed setup before switching modes

Notes:

- Channel strips support full soundcheck setup, including ALT source switching.
- Buses and matrices can be selected for recording setup, but they remain record-only and are not affected by the soundcheck toggle.
- If you open a REAPER project that was not prepared by WINGuard, use `Adopt Existing Reaper Project for Virtual Soundcheck` to review likely channel matches first. This action now lets you keep or override the proposed channel mapping, choose global `USB` or `CARD` routing, optionally override playback slots, and then adopt the imported tracks in place without creating duplicate tracks.
- The main WINGuard action opens the native tabbed window on both supported platforms.
- Windows follows the same compact layout as macOS and scales its system-derived fonts and controls with the active display DPI.
- The existing-project adoption action remains a separate review-first workflow that connects to WING before imported-track review starts.
- Existing-project adoption shortcut: `Cmd+Shift+I` on macOS, `Ctrl+Shift+I` on Windows.

Config note:

- WINGuard checks `UserPlugins/config.json` first and only falls back to `~/.wingconnector/config.json` when the `UserPlugins` copy is absent.
- If both files exist, the `UserPlugins` copy wins.

## 5. Automatic Wing CC Button Setup

Default mapping used by the plugin:

- `CC 20` -> Play
- `CC 21` -> Record
- `CC 22` -> Toggle Virtual Soundcheck
- `CC 23` -> Stop (save recorded media)
- `CC 24` -> Set Marker
- `CC 25` -> Previous Marker
- `CC 26` -> Next Marker

Use `MIDI CC` on channel 1 from WING custom controls.
When `Assign MIDI shortcuts to REAPER` is enabled in the plugin, button labels and MIDI command bindings are pushed to the selected WING layer automatically.
No manual REAPER action-list shortcut setup is required in normal use.

Detailed behavior:

- [docs/CC_BUTTONS_AND_AUTO_TRIGGER.md](docs/CC_BUTTONS_AND_AUTO_TRIGGER.md)

## 6. Optional: Auto-Record Trigger

The extension can auto-start/stop REAPER recording based on signal activity on armed + monitored tracks.

Set these in `config.json`:

- `auto_record_enabled` = `true`
- `auto_record_threshold_db` (example: `-40.0`)
- `auto_record_attack_ms`
- `auto_record_release_ms`
- `auto_record_min_record_ms`

Then reconnect to WING so the monitor loop starts.

UI support:
- Configure these directly in the main plugin window under `Auto Trigger`.
- `Mode` supports `WARNING` (no transport start) and `RECORD`.
- Signal detection is REAPER-based (armed + monitored tracks).
- `Monitor track` can target a specific REAPER track (`0` = auto across armed+monitored tracks).
- `Hold ms` keeps warning/record active briefly after drops to avoid rapid stop/start chatter.
- Live meter preview is shown as `Trigger level` in the dialog.

## 7. Optional: Route Main LR to SD (CARD 1/2)

Use the checkbox in the plugin window:

- `Route Main LR to CARD 1/2 when connected`

This requests CARD output routing for SD LR recording based on:

- `sd_lr_group`
- `sd_lr_left_input`
- `sd_lr_right_input`

If `sd_auto_record_with_reaper` is enabled, the plugin follows REAPER record start/stop and sends SD recorder OSC commands when REAPER enters/leaves recording.
The plugin sends a fallback list of SD recorder OSC paths for better firmware compatibility, but this is still best-effort OSC control and should be confirmed on WING before relying on SD capture.

## 8. Optional: OSC Notifications

Enable `OSC Out notifications` in the UI (or config) to emit trigger events:

- warning: `osc_warning_path`
- start: `osc_start_path`
- stop: `osc_stop_path`

Destination is configured by `osc_out_host` + `osc_out_port`.

## Troubleshooting Checklist

- Confirm WING and computer are on the same network.
- Verify the selected/discovered WING IP (or manual IP entry).
- Verify the console is reachable on WING OSC port `2223`.
- Check firewall rules for UDP traffic.
- Confirm REAPER loaded the extension from `UserPlugins`.
- Open the REAPER Actions list with `?` and search `WINGuard`; if the actions are missing, restart REAPER and re-check the plugin file in `UserPlugins`.
- If settings differ between a packaged install and a development install, check both `UserPlugins/config.json` and `~/.wingconnector/config.json`.
- If both config files exist, edit or remove the `UserPlugins` copy first because it takes precedence.

## Next Docs

- [docs/README.md](docs/README.md)
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md)
- [docs/CC_BUTTONS_AND_AUTO_TRIGGER.md](docs/CC_BUTTONS_AND_AUTO_TRIGGER.md)
- [SETUP.md](SETUP.md)
- [docs/WING_OSC_PROTOCOL.md](docs/WING_OSC_PROTOCOL.md)
