/*
 * Configuration Management
 * Load/save extension configuration
 */

#include "wingconnector/wing_config.h"
#include "internal/platform_util.h"
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace WingConnector {

namespace {

std::string BridgeKindToConfigValue(SourceKind kind) {
    switch (kind) {
        case SourceKind::Channel: return "channel";
        case SourceKind::Bus: return "bus";
        case SourceKind::Main: return "main";
        case SourceKind::Matrix: return "matrix";
    }
    return "channel";
}

SourceKind BridgeKindFromConfigValue(const std::string& value) {
    if (value == "bus") {
        return SourceKind::Bus;
    }
    if (value == "main") {
        return SourceKind::Main;
    }
    if (value == "matrix") {
        return SourceKind::Matrix;
    }
    return SourceKind::Channel;
}

}  // namespace

std::string WingConfig::GetConfigPath() {
    return Platform::GetConfigFilePath();
}

bool WingConfig::LoadFromFile(const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return false;
        }
        
        json config;
        file >> config;
        file.close();
        
        // Extract values with defaults
        wing_ip = config.value("wing_ip", "192.168.1.100");
        // Wing OSC port is fixed to 2223; keep ports constant in-memory.
        wing_port = 2223;
        listen_port = 2223;
        channel_count = config.value("channel_count", 48);
        timeout_ms = config.value("timeout", 2) * 1000;  // Convert to ms
        track_prefix = config.value("track_prefix", "Wing");
        include_channels = config.value("include_channels", "");
        exclude_channels = config.value("exclude_channels", "");
        soundcheck_output_mode = config.value("soundcheck_output_mode", "USB");
        create_stereo_pairs = config.value("create_stereo_pairs", false);
        color_tracks = config.value("color_tracks", true);
        configure_midi_actions = config.value("configure_midi_actions", false);
        auto_record_enabled = config.value("auto_record_enabled", false);
        auto_record_warning_only = config.value("auto_record_warning_only", false);
        auto_record_threshold_db = config.value("auto_record_threshold_db", -40.0);
        auto_record_attack_ms = config.value("auto_record_attack_ms", 250);
        auto_record_hold_ms = config.value("auto_record_hold_ms", 120000);
        if (auto_record_hold_ms <= 0) {
            auto_record_hold_ms = 120000;
        }
        auto_record_release_ms = config.value("auto_record_release_ms", 2000);
        auto_record_min_record_ms = config.value("auto_record_min_record_ms", 5000);
        auto_record_poll_ms = config.value("auto_record_poll_ms", 50);
        auto_record_monitor_track = config.value("auto_record_monitor_track", 0);
        recorder_coordination_enabled = config.value("recorder_coordination_enabled", false);
        sd_lr_route_enabled = config.value("sd_lr_route_enabled", false);
        recorder_target = config.value("recorder_target", "WLIVE");
        sd_lr_group = config.value("sd_lr_group", "MAIN");
        sd_lr_left_input = config.value("sd_lr_left_input", 1);
        sd_lr_right_input = config.value("sd_lr_right_input", 2);
        sd_auto_record_with_reaper = config.value("sd_auto_record_with_reaper", false);
        show_debug_log = config.value("show_debug_log", false);
        osc_warning_path = config.value("osc_warning_path", "/wing/record/warning");
        osc_start_path = config.value("osc_start_path", "/wing/record/start");
        osc_stop_path = config.value("osc_stop_path", "/wing/record/stop");
        warning_flash_cc_enabled = config.value("warning_flash_cc_enabled", true);
        warning_flash_cc_layer = config.value("warning_flash_cc_layer", 1);
        warning_flash_cc_color = config.value("warning_flash_cc_color", 9);
        last_selected_source_ids = config.value("last_selected_source_ids", std::vector<std::string>{});
        bridge_enabled = config.value("bridge_enabled", false);
        bridge_poll_ms = config.value("bridge_poll_ms", 75);
        bridge_debounce_ms = config.value("bridge_debounce_ms", 120);
        bridge_midi_output_device = config.value("bridge_midi_output_device", -1);
        bridge_midi_message_type = config.value("bridge_midi_message_type", "NOTE_ON_OFF");
        bridge_midi_channel = config.value("bridge_midi_channel", 1);
        bridge_mappings.clear();
        if (config.contains("bridge_mappings") && config["bridge_mappings"].is_array()) {
            for (const auto& item : config["bridge_mappings"]) {
                if (!item.is_object()) {
                    continue;
                }
                BridgeMapping mapping;
                mapping.kind = BridgeKindFromConfigValue(item.value("kind", "channel"));
                mapping.source_number = std::max(1, item.value("source_number", 1));
                mapping.midi_value = std::clamp(item.value("midi_value", 0), 0, 127);
                mapping.enabled = item.value("enabled", true);
                bridge_mappings.push_back(mapping);
            }
        }
        
        // Extract color if present
        if (config.contains("default_track_color") && config["default_track_color"].is_object()) {
            const auto& color_obj = config["default_track_color"];
            const int default_r = 100;
            const int default_g = 150;
            const int default_b = 200;
            const int r = std::clamp(color_obj.value("r", default_r), 0, 255);
            const int g = std::clamp(color_obj.value("g", default_g), 0, 255);
            const int b = std::clamp(color_obj.value("b", default_b), 0, 255);
            default_color.r = static_cast<uint8_t>(r);
            default_color.g = static_cast<uint8_t>(g);
            default_color.b = static_cast<uint8_t>(b);
        }
        
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

bool WingConfig::SaveToFile(const std::string& filepath) {
    namespace fs = std::filesystem;
    fs::path config_path(filepath);

    try {
        fs::path parent = config_path.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            fs::create_directories(parent);
        }
        
        // Create JSON object
        json config;
        config["wing_ip"] = wing_ip;
        config["channel_count"] = channel_count;
        config["timeout"] = timeout_ms / 1000;  // Convert to seconds
        config["track_prefix"] = track_prefix;
        config["color_tracks"] = color_tracks;
        config["create_stereo_pairs"] = create_stereo_pairs;
        config["soundcheck_output_mode"] = soundcheck_output_mode;
        config["include_channels"] = include_channels;
        config["exclude_channels"] = exclude_channels;
        config["configure_midi_actions"] = configure_midi_actions;
        config["auto_record_enabled"] = auto_record_enabled;
        config["auto_record_warning_only"] = auto_record_warning_only;
        config["auto_record_threshold_db"] = auto_record_threshold_db;
        config["auto_record_attack_ms"] = auto_record_attack_ms;
        config["auto_record_hold_ms"] = auto_record_hold_ms;
        config["auto_record_release_ms"] = auto_record_release_ms;
        config["auto_record_min_record_ms"] = auto_record_min_record_ms;
        config["auto_record_poll_ms"] = auto_record_poll_ms;
        config["auto_record_monitor_track"] = auto_record_monitor_track;
        config["recorder_coordination_enabled"] = recorder_coordination_enabled;
        config["sd_lr_route_enabled"] = sd_lr_route_enabled;
        config["recorder_target"] = recorder_target;
        config["sd_lr_group"] = sd_lr_group;
        config["sd_lr_left_input"] = sd_lr_left_input;
        config["sd_lr_right_input"] = sd_lr_right_input;
        config["sd_auto_record_with_reaper"] = sd_auto_record_with_reaper;
        config["show_debug_log"] = show_debug_log;
        config["osc_warning_path"] = osc_warning_path;
        config["osc_start_path"] = osc_start_path;
        config["osc_stop_path"] = osc_stop_path;
        config["warning_flash_cc_enabled"] = warning_flash_cc_enabled;
        config["warning_flash_cc_layer"] = warning_flash_cc_layer;
        config["warning_flash_cc_color"] = warning_flash_cc_color;
        config["last_selected_source_ids"] = last_selected_source_ids;
        config["bridge_enabled"] = bridge_enabled;
        config["bridge_poll_ms"] = bridge_poll_ms;
        config["bridge_debounce_ms"] = bridge_debounce_ms;
        config["bridge_midi_output_device"] = bridge_midi_output_device;
        config["bridge_midi_message_type"] = bridge_midi_message_type;
        config["bridge_midi_channel"] = bridge_midi_channel;
        config["bridge_mappings"] = json::array();
        for (const auto& mapping : bridge_mappings) {
            config["bridge_mappings"].push_back({
                {"kind", BridgeKindToConfigValue(mapping.kind)},
                {"source_number", mapping.source_number},
                {"midi_value", mapping.midi_value},
                {"enabled", mapping.enabled}
            });
        }
        config["default_track_color"] = {
            {"r", (int)default_color.r},
            {"g", (int)default_color.g},
            {"b", (int)default_color.b}
        };
        
        // Write with nice formatting (2-space indent)
        std::ofstream file(config_path);
        if (!file.is_open()) {
            return false;
        }
        
        file << config.dump(2) << std::endl;
        file.close();
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

} // namespace WingConnector
