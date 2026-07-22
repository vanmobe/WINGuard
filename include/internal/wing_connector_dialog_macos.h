/*
 * macOS Native AUDIOLAB.wing.reaper.virtualsoundcheck Dialog Header
 * Consolidated dialog for all AUDIOLAB.wing.reaper.virtualsoundcheck operations
 */

#ifndef WING_CONNECTOR_DIALOG_MACOS_H
#define WING_CONNECTOR_DIALOG_MACOS_H

#ifdef __APPLE__

#include "internal/adoption_editor.h"
#include <vector>
#include <string>
#include "reaper_extension.h"  // For ChannelSelectionInfo (from wingconnector/)

// Dialog result enum
enum class DialogAction {
    None,
    Connect,
    Disconnect,
    GetChannels,
    SetupSoundcheck,
    SaveSettings,
    Close
};

// Main AUDIOLAB.wing.reaper.virtualsoundcheck Dialog
// This is a modeless window that stays open
extern "C" {
    // Show the main AUDIOLAB.wing.reaper.virtualsoundcheck dialog
    // Returns the action the user wants to perform
    void ShowWingConnectorDialog();
    void ShowWingConnectorDialogAtTab(const char* tab_identifier);
}

// Channel Selection Dialog
extern "C" {
    // Show channel selection dialog
    // channels: List of available channels (will be modified with selection state)
    // setup_soundcheck: Output parameter - whether to configure soundcheck/ALT mode
    // Returns true if user confirmed, false if cancelled
    bool ShowChannelSelectionDialog(std::vector<WingConnector::ChannelSelectionInfo>& channels,
                                   const char* title,
                                   const char* description,
                                   bool& setup_soundcheck,
                                   bool& overwrite_existing);
}

extern "C" {
    bool ShowExistingProjectAdoptionEditor(const std::vector<AdoptionEditorRow>& rows,
                                           const std::vector<int>& available_channels,
                                           const char* initial_output_mode,
                                           std::string& output_mode_out,
                                           std::string& channel_overrides_spec_out,
                                           std::string& slot_overrides_spec_out,
                                           bool& apply_now_out);
}

#endif // __APPLE__

#endif // WING_CONNECTOR_DIALOG_MACOS_H
