# Selected Channel Bridge

The selected-channel bridge converts the currently selected WING strip into a configured MIDI message for integrations such as SuperRack. It is independent from virtual soundcheck, recorder control, and the WING CC transport controls.

## How It Works

1. WINGuard polls `/$ctl/$stat/selidx` through the active WING connection.
2. The selected strip is resolved as a channel, bus, main, or matrix.
3. After the configured debounce interval, WINGuard looks up the matching bridge mapping.
4. A mapped selection emits MIDI through the configured REAPER MIDI output.

Polling is used because live desk validation showed that selection changes were readable but were not reliably emitted through the tested subscription path.

## Configuration

On macOS, open the main `WINGuard: Configure Virtual Soundcheck/Recording` action and use the bridge controls in `Integration`. The current Windows dialog reports the applied integration state but does not yet expose the full bridge mapping editor; Windows bridge settings are loaded from `config.json`.

Bridge settings include:

- MIDI output device
- message behavior: `NOTE_ON`, `NOTE_ON_OFF`, or `PROGRAM`
- MIDI channel `1..16`
- source-to-MIDI mappings
- enable/disable state

Mapping entries use `SOURCE=MIDI_VALUE` and are separated by semicolons. Supported source families and ranges are:

- `CH1..CH48`
- `BUS1..BUS16`
- `MAIN1..MAIN4`
- `MTX1..MTX8`

Example:

```text
CH1=10;BUS1=20;MAIN1=30;MTX1=40
```

MIDI values must be in the range `0..127`. Settings are persisted through the normal `config.json` path.

## Runtime Behavior

- The bridge starts only when it is enabled, WINGuard is connected, a MIDI output is selected, and at least one mapping exists.
- Repeated polls of the same selected strip do not resend MIDI.
- `NOTE_ON_OFF` releases the previous mapped note when selection changes or the bridge stops.
- Disconnecting or unloading the plugin stops the polling thread and clears bridge MIDI state.
- An unmapped selection is reported in bridge status without emitting MIDI.

Use the bridge test control to confirm the selected MIDI output before relying on live selection changes.
