#ifndef WING_OSC_H
#define WING_OSC_H

#include <string>
#include <map>
#include <memory>
#include <functional>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstddef>
#include <set>
#include <vector>

class UdpListeningReceiveSocket;

namespace WingConnector {

class WingOscListener;

enum class SourceKind {
    Channel,
    Bus,
    Main,
    Matrix,
};

struct SourceSelectionInfo {
    SourceKind kind = SourceKind::Channel;
    int source_number = 0;
    std::string name;
    int color_id = -1;
    std::string source_group;
    int source_input = 0;
    std::string partner_name;
    std::string partner_source_group;
    int partner_source_input = 0;
    bool stereo_linked = false;
    bool stereo_intent_override = false;
    bool selected = false;
    bool soundcheck_capable = false;
};

struct ManagedChannelInputState {
    int channel_number = 0;
    std::string source_group;
    int source_input = 0;
    bool stereo_linked = false;
    bool stereo_readable = false;
    bool readable = false;
};

struct ManagedChannelDisplayState {
    int channel_number = 0;
    std::string name;
    int color_id = -1;
    bool customization_linked = false;
    bool readable = false;
};

// Channel data structure
struct ChannelInfo {
    int channel_number;
    std::string name;
    int color;
    std::string source;
    std::string icon;
    bool stereo_linked = false;
    bool muted = false;
    int scribble_color = 0;
    
    // Primary routing (current input)
    std::string primary_source_group;  // "LCL", "A", "B", "C", "SC", "USB", etc.
    int primary_source_input;          // 1-48 within the group
    
    // ALT routing (for virtual soundcheck)
    std::string alt_source_group;      // "USB", "OFF", etc.
    int alt_source_input;              // 1-48 within the group
    bool alt_source_enabled;           // Is ALT currently active?
    
    // USB allocation (calculated)
    int usb_output_start = 0;          // USB output number (1-48)
    int usb_output_end = 0;            // For stereo: start+1, for mono: same as start
};

// Playback allocation result shared by USB and CARD.
struct PlaybackAllocation {
    SourceKind source_kind = SourceKind::Channel;
    int source_number = 0;
    bool is_stereo;
    int usb_start;  // For stereo: odd number (1,3,5...), for mono: any
    int usb_end;    // For stereo: usb_start+1, for mono: same as start
    std::string allocation_note;
};

using USBAllocation = PlaybackAllocation;

// Basic metadata returned by the Wing handshake reply
struct WingInfo {
    std::string console_ip;
    std::string name;
    std::string model;
    std::string serial;
    std::string firmware;
};

// Callback for when channel data is received
using ChannelDataCallback = std::function<void(const ChannelInfo&)>;
using ProgressCallback = std::function<void(const std::string&)>;  // For real-time status updates

class WingOSC {
public:
    WingOSC(const std::string& wing_ip, uint16_t wing_port, uint16_t listen_port);
    ~WingOSC();
    
    // Start/stop the OSC listener
    bool Start();
    void Stop();
    
    // Test connection to Wing console
    bool TestConnection();
    
    // Query channel information
    void QueryChannel(int channel_num);
    void QueryAllChannels(int count);
    
    // Subscribe to real-time updates
    void SubscribeToChannel(int channel_num);
    void UnsubscribeFromChannel(int channel_num);
    
    // Get channel data
    const std::map<int, ChannelInfo>& GetChannelData() const { return channel_data_; }
    bool GetChannelInfo(int channel_num, ChannelInfo& info) const;
    
    // Set callback for when channel data is received
    void SetChannelCallback(ChannelDataCallback callback) { channel_callback_ = callback; }
    
    // Wing-specific OSC commands (based on Patrick-Gilles Maillot's manual)
    void GetChannelName(int channel_num);
    void SetChannelName(int channel_num, const std::string& name);
    void SetChannelColor(int channel_num, int color_index);
    void SetChannelCustomizationLinked(int channel_num, bool enable);
    void GetChannelColor(int channel_num);
    void GetChannelConfig(int channel_num);
    void GetChannelIcon(int channel_num);
    void GetChannelScribbleColor(int channel_num);
    
    // Channel routing queries (for virtual soundcheck)
    void GetChannelSourceRouting(int channel_num);     // Query primary source (grp, in)
    void GetChannelAltRouting(int channel_num);        // Query ALT source (altgrp, altin, altsrc)
    void GetChannelStereoLink(int channel_num);        // Query stereo link status (legacy /ch/N/clink)
    void QueryChannelSourceStereo(int channel_num);    // Query source stereo via /io/in/{grp}/{num}/mode
    
    // Channel routing configuration (for virtual soundcheck)
    void SetChannelPrimarySource(int channel_num, const std::string& grp, int in);
    void SetChannelAltSource(int channel_num, const std::string& grp, int in);
    void EnableChannelAltSource(int channel_num, bool enable);
    void SetAllChannelsAltEnabled(bool enable);  // Batch toggle for soundcheck mode
    
    // USB output routing configuration (for recording from Wing to REAPER)
    void SetUSBOutputSource(int usb_num, const std::string& grp, int in);
    void SetUSBOutputName(int usb_num, const std::string& name);   // Name USB output (source side)
    void SetUSBInputName(int usb_num, const std::string& name);    // Name USB input (REAPER receives from)
    void UnlockUSBOutputs();   // Unlock USB outputs before configuration
    void LockUSBOutputs();     // Lock USB outputs after configuration (optional)
    void ClearUSBOutput(int usb_num);  // Clear USB output routing (set to OFF)
    
    // CARD output routing configuration (alternative to USB)
    void SetCardOutputSource(int card_num, const std::string& grp, int in);
    void SetCardOutputName(int card_num, const std::string& name);
    void SetCardInputName(int card_num, const std::string& name);    // Name CARD input (REAPER sends back to)
    void SetSourceInputName(const std::string& group, int input_num, const std::string& name);  // Name a generic source input
    void ClearCardOutput(int card_num);  // Clear CARD output routing (set to OFF)
    void SetWLiveRecordTrackCount(int slot, int tracks);
    void SetRecorderOutputSource(int recorder_num, const std::string& grp, int in);
    void SetRecorderOutputName(int recorder_num, const std::string& name);
    void ClearRecorderOutput(int recorder_num);
    
    // USB/CARD input mode configuration (stereo/mono)
    void SetUSBInputMode(int usb_num, const std::string& mode);  // Set USB input mode: "ST" = stereo, "M" = mono
    void SetCardInputMode(int card_num, const std::string& mode);  // Set CARD input mode: "ST" = stereo, "M" = mono
    void SetSourceInputMode(const std::string& group, int input_num, const std::string& mode);  // Set a generic input source mode
    void ClearUSBInput(int usb_num);  // Clear USB input (reset to mono mode)
    void ClearCardInput(int card_num);  // Clear CARD input (reset to mono mode)
    
    // Playback allocation utilities shared by USB and CARD routing.
    std::vector<PlaybackAllocation> CalculatePlaybackAllocation(const std::vector<SourceSelectionInfo>& channels);
    void ApplyPlaybackAllocationAsAlt(const std::vector<PlaybackAllocation>& allocations,
                                      const std::vector<SourceSelectionInfo>& channels,
                                      const std::string& output_mode = "USB",
                                      bool setup_soundcheck = true);
    std::vector<PlaybackAllocation> CalculateUSBAllocation(const std::vector<SourceSelectionInfo>& channels);
    void ApplyUSBAllocationAsAlt(const std::vector<PlaybackAllocation>& allocations,
                                 const std::vector<SourceSelectionInfo>& channels,
                                 const std::string& output_mode = "USB",
                                 bool setup_soundcheck = true);
    
    // User Signal input routing (for resolving indirection through USR inputs)
    void QueryUserSignalInputs(int count);  // Query all USR input sources
    void QueryUserSignalStereo(int count);  // No-op: Wing doesn't expose this via OSC
    std::pair<std::string, int> ResolveRoutingChain(const std::string& grp, int in);  // Follow routing chain
    bool IsUserSignalStereo(int usr_num) const;  // Check if USR input is stereo (based on odd/even fallback)
    void QueryInputSourceNames(const std::set<std::pair<std::string, int>>& sources);
    std::string GetInputSourceName(const std::string& grp, int in) const;
    std::string QueryInputSourceNameDirect(const std::string& grp, int in) const;
    std::string QueryInputModeDirect(const std::string& grp, int in) const;
    std::string QueryConsoleSourceNameDirect(SourceKind kind, int number) const;
    std::map<std::string, std::string> QueryStringAddressesDirect(const std::vector<std::string>& addresses,
                                                                  int total_timeout_ms = 150,
                                                                  int idle_timeout_ms = 25) const;
    std::map<std::string, int> QueryIntAddressesDirect(const std::vector<std::string>& addresses,
                                                       int total_timeout_ms = 150,
                                                       int idle_timeout_ms = 25) const;
    bool GetSelectedStripIndex(int& strip_index_one_based) const;
    std::map<int, ManagedChannelInputState> QueryManagedChannelInputStatesDirect(const std::vector<int>& channel_numbers) const;
    std::map<int, ManagedChannelDisplayState> QueryManagedChannelDisplayStatesDirect(const std::vector<int>& channel_numbers) const;
    
    // Additional Wing commands
    void GetConsoleInfo();
    void GetShowName();
    void RequestMeterValue(const std::string& address_template, int channel_num);
    double GetLastMeterLinearValue(int value_index = 0) const;
    std::vector<double> GetLastMeterValues() const;
    void StartSDRecorder();
    void StopSDRecorder();
    void StartUSBRecorder();
    void StopUSBRecorder();
    bool GetUSBRecorderStatus(std::string& active_state, std::string& action_state) const;
    bool GetWLiveRecorderStatus(int slot,
                                std::string& state,
                                std::string& media_state,
                                std::string& error_message,
                                std::string& error_code) const;
    void SetUserControlLed(int layer, int button, bool on);
    void SetUserControlColor(int layer, int button, int color_index);
    void SetUserControlButtonLed(int layer, int button, bool on, bool lower_row = false);
    void SetActiveUserControlLayer(int layer);
    void SetUserControlRotaryName(int layer, int rotary, const std::string& name);
    void SetUserControlButtonName(int layer, int button, const std::string& name, bool lower_row = false);
    void SetUserControlButtonMidiCCToggle(int layer, int button, int midi_channel, int cc_number, int value = 0, bool lower_row = false, bool toggle_mode = false);
    void ClearUserControlButtonCommand(int layer, int button, bool lower_row = false);
    void SetUserControlButtonValue(int layer, int button, int value, bool lower_row = false);
    void QueryUserControlColor(int layer, int button);
    void QueryUserControlRotaryText(int layer, int rotary);
    int GetCachedUserControlColor(int layer, int button, int fallback = 2) const;
    
    // Handle OSC messages (public for listener callback)
    void HandleOscMessage(const std::string& address, const void* data, size_t size);

    // Wing metadata collected during handshake
    const WingInfo& GetWingInfo() const { return wing_info_; }
    const std::string& GetLastConnectionDiagnostic() const { return last_connection_diagnostic_; }

    // Network discovery: broadcast "WING?" and collect all responding consoles.
    // Returns a list of WingInfo structs (one per responding device).
    // timeout_ms controls how long to wait for responses (default 1500 ms).
    static std::vector<WingInfo> DiscoverWings(int timeout_ms = 1500);
    
private:
    friend class WingOscListener;

    std::string wing_ip_;
    uint16_t wing_port_;
    uint16_t listen_port_;
    
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> listener_thread_;
    
    std::map<int, ChannelInfo> channel_data_;
    mutable std::mutex data_mutex_;
    
    // User Signal input routing: maps USR input number → (source_group, source_input)
    std::map<int, std::pair<std::string, int>> usr_routing_data_;
    std::map<int, bool> usr_stereo_data_;  // maps USR input number → is_stereo
    std::map<std::string, std::string> input_source_names_;  // key: "GROUP:IN" -> display name
    
    // Config-based USR routing fallback: maps "USR:N" → "GROUP:M"
    // Used when Wing doesn't respond to /usr/* queries
    std::map<std::string, std::string> usr_routing_config_;
    
    ChannelDataCallback channel_callback_;
    WingInfo wing_info_{};
    bool handshake_complete_ = false;
    std::string last_connection_diagnostic_;
    mutable std::mutex log_mutex_;
    std::string last_meter_address_;
    std::vector<double> last_meter_values_;
    
    // OSC socket handles (oscpack)
    UdpListeningReceiveSocket* osc_socket_;
    WingOscListener* osc_listener_;
    std::mutex send_mutex_;
    std::map<std::pair<int, int>, int> user_control_color_cache_;
    
    // Internal methods
    void ListenerThread();
    void ParseChannelName(int channel_num, const std::string& value);
    void ParseChannelColor(int channel_num, int value);
    void ParseChannelConfig(int channel_num, const std::string& value);
    bool PerformHandshake();
    bool SendRawPacket(const char* data, std::size_t size);
    void SendQueryBurst(const std::vector<std::string>& addresses);
    void Log(const std::string& message) const;
    std::pair<std::string, int> ResolveRoutingChainLocked(const std::string& grp, int in) const;
    
    // Format channel number for OSC address (e.g., "01", "02", etc.)
    static std::string FormatChannelNum(int num);
};

} // namespace WingConnector

#endif // WING_OSC_H
