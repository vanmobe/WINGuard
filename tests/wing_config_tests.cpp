#include "wingconnector/wing_config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using WingConnector::BridgeMapping;
using WingConnector::SourceKind;
using WingConnector::WingConfig;
using json = nlohmann::json;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::filesystem::path MakeTempPath(const std::string& name) {
    const auto base = std::filesystem::temp_directory_path() / "wing_config_tests";
    std::filesystem::create_directories(base);
    return base / name;
}

void WriteJsonFile(const std::filesystem::path& path, const json& value) {
    std::ofstream file(path);
    Expect(file.is_open(), "temp config file should open for writing");
    file << value.dump(2) << std::endl;
}

json ReadJsonFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    Expect(file.is_open(), "temp config file should open for reading");
    json value;
    file >> value;
    return value;
}

void TestLoadForcesFixedPortsAndConvertsTimeout() {
    const auto path = MakeTempPath("load_fixed_ports.json");
    WriteJsonFile(path, {
        {"wing_ip", "10.0.0.55"},
        {"timeout", 5},
        {"wing_port", 9999},
        {"listen_port", 9998},
        {"soundcheck_output_mode", "CARD"}
    });

    WingConfig config;
    Expect(config.LoadFromFile(path.string()), "config load should succeed");
    Expect(config.wing_ip == "10.0.0.55", "wing_ip should load");
    Expect(config.timeout_ms == 5000, "timeout seconds should convert to milliseconds");
    Expect(config.wing_port == 2223, "wing port should stay fixed to 2223");
    Expect(config.listen_port == 2223, "listen port should stay fixed to 2223");
    Expect(config.soundcheck_output_mode == "CARD", "output mode should load");
    Expect(config.auto_record_hold_ms == 120000, "missing auto-record hold should use the safe 120-second default");
}

void TestLoadPreservesExplicitAutoRecordHold() {
    const auto path = MakeTempPath("load_explicit_auto_record_hold.json");
    WriteJsonFile(path, {{"auto_record_hold_ms", 45000}});

    WingConfig config;
    Expect(config.LoadFromFile(path.string()), "config with explicit auto-record hold should load");
    Expect(config.auto_record_hold_ms == 45000, "explicit auto-record hold should be preserved");

    WriteJsonFile(path, {{"auto_record_hold_ms", 0}});
    Expect(config.LoadFromFile(path.string()), "config with unsafe zero hold should load");
    Expect(config.auto_record_hold_ms == 120000, "unsafe zero hold should migrate to the 120-second default");
}

void TestLoadReadsBridgeMappingsAndManagedSelection() {
    const auto path = MakeTempPath("load_bridge_mappings.json");
    WriteJsonFile(path, {
        {"last_selected_source_ids", {"CH:1", "BUS:3"}},
        {"bridge_mappings", json::array({
            {
                {"kind", "bus"},
                {"source_number", 3},
                {"midi_value", 17},
                {"enabled", false}
            },
            {
                {"kind", "matrix"},
                {"source_number", 2},
                {"midi_value", 99},
                {"enabled", true}
            }
        })}
    });

    WingConfig config;
    Expect(config.LoadFromFile(path.string()), "config load should succeed");
    Expect(config.last_selected_source_ids.size() == 2, "managed selection ids should load");
    Expect(config.last_selected_source_ids[0] == "CH:1", "first managed source id should match");
    Expect(config.bridge_mappings.size() == 2, "bridge mappings should load");
    Expect(config.bridge_mappings[0].kind == SourceKind::Bus, "bus mapping kind should decode");
    Expect(config.bridge_mappings[0].source_number == 3, "bus mapping source number should decode");
    Expect(config.bridge_mappings[0].midi_value == 17, "bus mapping midi value should decode");
    Expect(!config.bridge_mappings[0].enabled, "bus mapping enabled flag should decode");
    Expect(config.bridge_mappings[1].kind == SourceKind::Matrix, "matrix mapping kind should decode");
}

void TestLoadFallsBackForUnknownBridgeKindAndMissingArrays() {
    const auto path = MakeTempPath("load_unknown_bridge_kind.json");
    WriteJsonFile(path, {
        {"bridge_mappings", json::array({
            {
                {"kind", "not-a-real-kind"},
                {"source_number", 11},
                {"midi_value", 45},
                {"enabled", true}
            }
        })}
    });

    WingConfig config;
    config.last_selected_source_ids = {"CH:99"};
    Expect(config.LoadFromFile(path.string()), "config load should succeed");
    Expect(config.bridge_mappings.size() == 1, "unknown bridge kind input should still load one mapping");
    Expect(config.bridge_mappings[0].kind == SourceKind::Channel,
           "unknown bridge kind should fall back to channel");
    Expect(config.last_selected_source_ids.empty(),
           "missing managed selection array should reset to an empty default");
}

void TestLoadClampsAndDefaultsTrackColor() {
    const auto path = MakeTempPath("load_track_color_defaults.json");
    WriteJsonFile(path, {
        {"default_track_color", {
            {"r", 999},
            {"b", -5}
        }}
    });

    WingConfig config;
    config.default_color.r = 1;
    config.default_color.g = 2;
    config.default_color.b = 3;
    Expect(config.LoadFromFile(path.string()), "config load should succeed");
    Expect(config.default_color.r == 255, "red should clamp down to 255");
    Expect(config.default_color.g == 150, "missing green should use the default value");
    Expect(config.default_color.b == 0, "blue should clamp up to 0");
}

void TestSaveWritesRoundTrippableFields() {
    const auto path = MakeTempPath("save_round_trip.json");

    WingConfig config;
    config.wing_ip = "192.168.50.20";
    config.timeout_ms = 7000;
    config.soundcheck_output_mode = "CARD";
    config.last_selected_source_ids = {"CH:8", "MTX:1"};
    config.bridge_enabled = true;
    config.bridge_poll_ms = 120;
    config.bridge_debounce_ms = 240;
    config.bridge_midi_output_device = 5;
    config.bridge_midi_message_type = "PROGRAM";
    config.bridge_midi_channel = 12;
    config.default_color.r = 12;
    config.default_color.g = 34;
    config.default_color.b = 56;
    config.bridge_mappings = {
        BridgeMapping{SourceKind::Main, 1, 64, true},
        BridgeMapping{SourceKind::Channel, 9, 40, false},
    };

    Expect(config.SaveToFile(path.string()), "config save should succeed");

    const json saved = ReadJsonFile(path);
    Expect(saved.at("wing_ip") == "192.168.50.20", "saved wing ip should match");
    Expect(saved.at("timeout") == 7, "saved timeout should be stored in seconds");
    Expect(saved.at("soundcheck_output_mode") == "CARD", "saved output mode should match");
    Expect(saved.at("last_selected_source_ids").size() == 2, "saved managed selections should match");
    Expect(saved.at("bridge_mappings").size() == 2, "saved bridge mappings should match");
    Expect(saved.at("bridge_mappings")[0].at("kind") == "main", "main mapping should serialize");
    Expect(saved.at("bridge_mappings")[1].at("kind") == "channel", "channel mapping should serialize");
    Expect(saved.at("default_track_color").at("r") == 12, "saved red color should match");

    WingConfig reloaded;
    Expect(reloaded.LoadFromFile(path.string()), "reloaded config should load");
    Expect(reloaded.timeout_ms == 7000, "reloaded timeout should round-trip");
    Expect(reloaded.bridge_mappings.size() == 2, "reloaded bridge mappings should round-trip");
    Expect(reloaded.bridge_mappings[0].kind == SourceKind::Main, "reloaded main mapping kind should match");
    Expect(reloaded.bridge_mappings[1].kind == SourceKind::Channel, "reloaded channel mapping kind should match");
    Expect(reloaded.last_selected_source_ids[1] == "MTX:1", "reloaded managed source ids should match");
    Expect(reloaded.wing_port == 2223 && reloaded.listen_port == 2223,
           "reloaded fixed ports should remain enforced");
}

}  // namespace

int main() {
    TestLoadForcesFixedPortsAndConvertsTimeout();
    TestLoadPreservesExplicitAutoRecordHold();
    TestLoadReadsBridgeMappingsAndManagedSelection();
    TestLoadFallsBackForUnknownBridgeKindAndMissingArrays();
    TestLoadClampsAndDefaultsTrackColor();
    TestSaveWritesRoundTrippableFields();

    std::cout << "wing_config_tests: OK\n";
    return 0;
}
