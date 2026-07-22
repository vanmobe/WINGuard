#ifndef REAPER_EXTENSION_H
#define REAPER_EXTENSION_H

// Forward declaration to avoid include ordering issues
struct reaper_plugin_info_t;

#include "wing_config.h"
#include "wing_osc.h"
#include "track_manager.h"
#include <memory>
#include <atomic>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
class midi_Input;

namespace WingConnector {

using ChannelSelectionInfo = SourceSelectionInfo;

enum class ValidationState {
    NotReady,
    Warning,
    Ready
};

class ReaperExtension {
public:
    static ReaperExtension& Instance();
    
    // Extension lifecycle
    bool Initialize(reaper_plugin_info_t* rec = nullptr);
    void Shutdown();
    
    // Connection (called by dialog)
    bool ConnectToWing();  // Returns true if successful
    void DisconnectFromWing();

    // Network discovery: scan for Wing consoles on the LAN
    std::vector<WingInfo> DiscoverWings(int timeout_ms = 1500);
    
    // Channel operations (called by dialog)
    std::vector<SourceSelectionInfo> GetAvailableSources();
    void CreateTracksFromSelection(const std::vector<SourceSelectionInfo>& channels);
    bool PrepareSoundcheckPlan(const std::vector<SourceSelectionInfo>& channels,
                               std::vector<SourceSelectionInfo>& prepared_channels,
                               std::vector<PlaybackAllocation>& requested_allocations,
                               std::string& error_detail);
    bool SetupSoundcheckFromSelection(const std::vector<SourceSelectionInfo>& channels, bool setup_soundcheck = true, bool replace_existing = true);
    bool SetupSoundcheckFromPlan(const std::vector<SourceSelectionInfo>& channels,
                                 const std::vector<PlaybackAllocation>& requested_allocations,
                                 const std::string& output_mode,
                                 bool setup_soundcheck = true,
                                 bool replace_existing = true);
    bool CheckOutputModeAvailability(const std::string& output_mode, std::string& details) const;
    ValidationState ValidateLiveRecordingSetup(std::string& details);
    ValidationState ValidateMidiActionSetup(std::string& details);
    void RouteMainLRToCardForSDRecording();
    void ApplyRecorderRoutingNoDialog();
    double ReadCurrentTriggerLevel();
    void ApplyAutoRecordSettings();
    void SyncMidiActionsToWing();
    void PauseAutoRecordForSetup();
    void ApplyBridgeSettings();
    std::vector<std::string> GetMidiOutputDevices() const;
    std::string GetBridgeStatusSummary() const;
    bool SendBridgeTestMessage(int midi_value, std::string& detail_out);
    
    // MIDI action mapping
    void EnableMidiActions(bool enable);
    bool IsMidiActionsEnabled() const { return midi_actions_enabled_; }
    void EnableWingMidiDevice();
    
    // Legacy actions (keep for backward compatibility)
    void RefreshTracks();
    void ShowSettings();
    void ConfigureVirtualSoundcheck();
    bool SetSoundcheckModeEnabled(bool enable, std::string* error_detail = nullptr);
    void ToggleSoundcheckMode();
    bool IsSoundcheckModeEnabled() const { return soundcheck_mode_enabled_; }
    
    // Real-time monitoring (deprecated but kept for compatibility)
    void EnableMonitoring(bool enable);
    bool IsMonitoring() const { return monitoring_enabled_; }
    int GetProjectTrackCount() const;
    
    // Status
    bool IsConnected() const { return connected_; }
    std::string GetStatusMessage() const { return status_message_; }
    std::string GetLastConnectionFailureDetail() const { return last_connection_failure_detail_; }
    
    // Configuration
    WingConfig& GetConfig() { return config_; }
    const WingConfig& GetConfig() const { return config_; }
    
    // Access to OSC handler for dialog logging
    WingOSC* GetOSCHandler() { return osc_handler_.get(); }
    
    // Logging callback
    using LogCallback = std::function<void(const std::string&)>;
    void SetLogCallback(LogCallback callback) { log_callback_ = callback; }
    void Log(const std::string& message);
    
    // MIDI input hook (needed by extern wrapper function)
    static bool MidiInputHook(bool is_midi, const unsigned char* data, int len, int dev_id);
    static void MainThreadTimerTick();

private:
    ReaperExtension();
    ~ReaperExtension();
    
    // Singleton - delete copy/move
    ReaperExtension(const ReaperExtension&) = delete;
    ReaperExtension& operator=(const ReaperExtension&) = delete;
    
    // Members
    WingConfig config_;
    std::unique_ptr<WingOSC> osc_handler_;
    std::unique_ptr<TrackManager> track_manager_;
    
    std::atomic<bool> connected_;
    std::atomic<bool> monitoring_enabled_;
    std::atomic<bool> soundcheck_mode_enabled_;
    std::atomic<bool> midi_actions_enabled_;
    std::string status_message_;
    std::string last_connection_failure_detail_;
    LogCallback log_callback_;
    
    // Static REAPER plugin context (set in Initialize)
    static reaper_plugin_info_t* g_rec_;
    
    // MIDI action registration
    struct MidiAction {
        int command_id;
        const char* description;
        int cc_number;
    };
    static constexpr MidiAction MIDI_ACTIONS[] = {
        {1007,  "AUDIOLAB.wing.reaper.virtualsoundcheck: Play", 20},
        {1013,  "AUDIOLAB.wing.reaper.virtualsoundcheck: Record", 21},
        {0,     "AUDIOLAB.wing.reaper.virtualsoundcheck: Toggle Virtual Soundcheck", 22},
        {40667, "AUDIOLAB.wing.reaper.virtualsoundcheck: Stop", 23},
        {40157, "AUDIOLAB.wing.reaper.virtualsoundcheck: Set Marker", 24},
        {40172, "AUDIOLAB.wing.reaper.virtualsoundcheck: Previous Marker", 25},
        {40173, "AUDIOLAB.wing.reaper.virtualsoundcheck: Next Marker", 26}
    };
    
    void UnregisterMidiShortcuts();
    void StartMidiCapture();
    void StopMidiCapture();
    void MidiCaptureLoop();
    
    // MIDI input handling  
    void ProcessMidiInput(const unsigned char* data, int len);
    void MonitorAutoRecordLoop();
    bool RefreshSoundcheckModeFromWing();
    void StartAutoRecordMonitor();
    void StopAutoRecordMonitor();
    void QueueManagedSourceMonitorWarning(const std::string& warning);
    double GetMaxArmedTrackPeak() const;
    void StartWarningFlash();
    void StopWarningFlash(bool force = false);
    void WarningFlashLoop();
    void SetWarningLayerState();
    void SetRecordingLayerState();
    void ClearLayerState();
    void ApplyMidiShortcutButtonLabels();
    void ClearMidiShortcutButtonLabels();
    void ApplyMidiShortcutButtonCommands();
    void ClearMidiShortcutButtonCommands();
    void TriggerManualTransportFlash(int color_index);
    void StopManualTransportFlash();
    void SyncExternalRecorderWithReaperState(bool is_recording_now);
    void StartExternalRecorderFollow();
    void StopExternalRecorderFollow();
    void StartManagedSourceMonitor();
    void StopManagedSourceMonitor();
    void ManagedSourceMonitorLoop();
    void RefreshManagedSourceMonitorScope();
    std::vector<int> GetManagedMonitorChannelNumbers() const;
    std::map<int, ManagedChannelDisplayState> BuildManagedEffectiveDisplayStates(const std::map<int, ManagedChannelInputState>& latest_states) const;
    bool ReapplyManagedChannelRouting(const std::map<int, ManagedChannelInputState>& latest_states, const std::string& reason);
    void ApplyManagedTrackMetadataUpdate(const std::map<int, ManagedChannelDisplayState>& latest_display_states);
    void ApplyManagedTrackRoutingUpdate(const std::vector<SourceSelectionInfo>& selected_sources,
                                        const std::vector<PlaybackAllocation>& allocations);
    void StartSelectedChannelBridge();
    void StopSelectedChannelBridge();
    void SelectedChannelBridgeLoop();
    void ClearBridgeMidiState();
    void DispatchBridgeSelection(const BridgeMapping& mapping, const std::string& selection_id, const std::string& source_name);
    bool ResolveSelectedStrip(SourceKind& kind_out, int& source_number_out, std::string& source_name_out) const;
    bool FindBridgeMapping(SourceKind kind, int source_number, BridgeMapping& mapping_out) const;
    void SendBridgeMidiMessage(int status, int data1, int data2) const;
    bool BridgeMessageNeedsRelease() const;
    void InvalidateAvailableSourcesCache();
    void InvalidateValidationCache();
    std::vector<SourceSelectionInfo> QueryAvailableSourcesSnapshot();
    bool SetupSoundcheckInternal(const std::vector<SourceSelectionInfo>& channels,
                                 const std::vector<PlaybackAllocation>* requested_allocations,
                                 const std::string* output_mode_override,
                                 bool setup_soundcheck,
                                 bool replace_existing);
    
    // Callbacks
    void OnChannelDataReceived(const ChannelInfo& channel);
    void OnConnectionStatusChanged(bool connected);

    std::atomic<bool> auto_record_monitor_running_{false};
    std::unique_ptr<std::thread> auto_record_monitor_thread_;
    std::atomic<bool> auto_record_started_by_plugin_{false};
    std::atomic<bool> warning_flash_running_{false};
    std::unique_ptr<std::thread> warning_flash_thread_;
    std::atomic<bool> pending_record_start_{false};
    std::atomic<bool> pending_record_stop_{false};
    std::atomic<bool> pending_toggle_soundcheck_mode_{false};
    std::atomic<int> pending_midi_command_{0};
    std::atomic<bool> midi_capture_running_{false};
    std::unique_ptr<std::thread> midi_capture_thread_;
    std::vector<midi_Input*> midi_inputs_;
    std::atomic<bool> manual_transport_flash_running_{false};
    std::atomic<long long> manual_record_suppress_until_ms_{0};
    std::atomic<long long> suppress_play_cc_until_ms_{0};
    std::atomic<long long> suppress_record_cc_until_ms_{0};
    std::atomic<long long> suppress_all_cc_until_ms_{0};
    std::atomic<bool> suppress_midi_processing_{false};
    std::atomic<bool> external_recorder_started_by_plugin_{false};
    std::atomic<bool> last_known_reaper_recording_state_{false};
    std::atomic<bool> managed_source_monitor_running_{false};
    std::atomic<bool> suppress_managed_source_monitor_{false};
    std::unique_ptr<std::thread> managed_source_monitor_thread_;
    mutable std::mutex managed_source_monitor_mutex_;
    std::string pending_source_monitor_warning_;
    std::vector<int> managed_monitor_channel_numbers_;
    std::map<int, ManagedChannelInputState> managed_monitor_snapshot_;
    std::map<int, ManagedChannelDisplayState> managed_monitor_display_snapshot_;
    std::map<int, int> managed_monitor_unreadable_counts_;
    int managed_monitor_degraded_cycle_count_{0};
    std::unique_ptr<std::thread> manual_transport_flash_thread_;
    std::mutex manual_transport_flash_mutex_;
    std::atomic<long long> transport_guard_until_ms_{0};
    std::atomic<bool> transport_guard_from_stopped_state_{false};
    std::atomic<double> transport_guard_restore_pos_{0.0};
    std::atomic<int> layer_state_mode_{0}; // 0=idle, 1=warning, 2=recording
    std::atomic<bool> bridge_running_{false};
    std::unique_ptr<std::thread> bridge_thread_;
    mutable std::mutex bridge_state_mutex_;
    std::string bridge_status_summary_{"Bridge idle"};
    std::string last_bridge_selection_id_;
    std::string pending_bridge_selection_id_;
    long long pending_bridge_selection_since_ms_{0};
    int last_bridge_midi_number_{-1};
    mutable std::mutex available_sources_cache_mutex_;
    std::vector<SourceSelectionInfo> available_sources_cache_;
    std::string available_sources_cache_ip_;
    long long available_sources_cache_until_ms_{0};
    mutable std::mutex validation_cache_mutex_;
    ValidationState validation_cache_state_{ValidationState::NotReady};
    std::string validation_cache_details_;
    std::string validation_cache_ip_;
    std::string validation_cache_output_mode_;
    long long validation_cache_until_ms_{0};
};

// Reaper action command IDs
extern "C" {
    void CommandConnectToWing(int command_id);
    void CommandRefreshTracks(int command_id);
    void CommandToggleMonitoring(int command_id);
    void CommandShowSettings(int command_id);
    void CommandConfigureVirtualSoundcheck(int command_id);
    void CommandToggleSoundcheckMode(int command_id);
}

} // namespace WingConnector

#endif // REAPER_EXTENSION_H
