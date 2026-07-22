#include "internal/managed_source_monitor.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using WingConnector::ManagedChannelInputState;
using WingConnector::ManagedSourceMonitor::Action;
using WingConnector::ManagedSourceMonitor::ClassifyChange;

namespace {

ManagedChannelInputState MakeState(int channel_number,
                                   const std::string& group,
                                   int input,
                                   bool stereo,
                                   bool readable = true) {
    ManagedChannelInputState state;
    state.channel_number = channel_number;
    state.source_group = group;
    state.source_input = input;
    state.stereo_linked = stereo;
    state.stereo_readable = readable;
    state.readable = readable;
    return state;
}

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void ExpectChannels(const std::vector<int>& actual,
                    const std::vector<int>& expected,
                    const std::string& message) {
    Expect(actual == expected, message);
}

void TestNoChange() {
    std::map<int, ManagedChannelInputState> previous{{1, MakeState(1, "A", 1, false)}};
    auto decision = ClassifyChange(previous, previous);
    Expect(decision.action == Action::None, "unchanged state should not trigger a decision");
    Expect(decision.changed_channels.empty(), "unchanged state should have no changed channels");
}

void TestMonoSourceChangeReapplies() {
    std::map<int, ManagedChannelInputState> previous{{1, MakeState(1, "A", 1, false)}};
    std::map<int, ManagedChannelInputState> current{{1, MakeState(1, "B", 5, false)}};
    auto decision = ClassifyChange(previous, current);
    Expect(decision.action == Action::ReapplyRouting, "mono source change should trigger routing reapply");
    ExpectChannels(decision.changed_channels, {1}, "mono source change should identify the changed channel");
}

void TestStereoSourceChangeReapplies() {
    std::map<int, ManagedChannelInputState> previous{{2, MakeState(2, "USB", 9, true)}};
    std::map<int, ManagedChannelInputState> current{{2, MakeState(2, "USB", 17, true)}};
    auto decision = ClassifyChange(previous, current);
    Expect(decision.action == Action::ReapplyRouting, "stereo source change should trigger routing reapply");
    ExpectChannels(decision.changed_channels, {2}, "stereo source change should identify the changed channel");
}

void TestTopologyChangeWarns() {
    std::map<int, ManagedChannelInputState> previous{{3, MakeState(3, "A", 3, false)}};
    std::map<int, ManagedChannelInputState> current{{3, MakeState(3, "A", 3, true)}};
    auto decision = ClassifyChange(previous, current);
    Expect(decision.action == Action::WarnTopologyChange, "mono/stereo topology change should warn");
    ExpectChannels(decision.changed_channels, {3}, "topology change should identify the changed channel");
}

void TestInvalidSourceWarns() {
    std::map<int, ManagedChannelInputState> previous{{4, MakeState(4, "A", 4, false)}};
    std::map<int, ManagedChannelInputState> current{{4, MakeState(4, "OFF", 0, false)}};
    auto decision = ClassifyChange(previous, current);
    Expect(decision.action == Action::WarnInvalidSource, "OFF source should warn instead of rerouting");
    ExpectChannels(decision.changed_channels, {4}, "invalid source should identify the changed channel");
}

void TestUnreadableSourceWarns() {
    std::map<int, ManagedChannelInputState> previous{{5, MakeState(5, "A", 5, false)}};
    std::map<int, ManagedChannelInputState> current{{5, MakeState(5, "", 0, false, false)}};
    auto decision = ClassifyChange(previous, current);
    Expect(decision.action == Action::WarnInvalidSource, "unreadable source should warn");
    ExpectChannels(decision.changed_channels, {5}, "unreadable source should identify the changed channel");
}

void TestMixedChangesPreferWarning() {
    std::map<int, ManagedChannelInputState> previous{
        {1, MakeState(1, "A", 1, false)},
        {2, MakeState(2, "A", 2, false)},
    };
    std::map<int, ManagedChannelInputState> current{
        {1, MakeState(1, "B", 7, false)},
        {2, MakeState(2, "A", 2, true)},
    };
    auto decision = ClassifyChange(previous, current);
    Expect(decision.action == Action::WarnTopologyChange, "topology warning should win over reroute");
    ExpectChannels(decision.changed_channels, {1, 2}, "mixed change set should report all changed channels");
}

void TestBootstrapDoesNotTrigger() {
    std::map<int, ManagedChannelInputState> previous;
    std::map<int, ManagedChannelInputState> current{{6, MakeState(6, "USB", 12, false)}};
    auto decision = ClassifyChange(previous, current);
    Expect(decision.action == Action::None, "bootstrap snapshot should not trigger reroute");
    Expect(decision.changed_channels.empty(), "bootstrap snapshot should not report changes");
}

void TestUnreadablePollIsMaskedBeforeThreshold() {
    const std::map<int, ManagedChannelInputState> previous{{7, MakeState(7, "A", 7, false)}};
    const std::map<int, ManagedChannelInputState> current{{7, MakeState(7, "", 0, false, false)}};
    const auto filtered = WingConnector::ManagedSourceMonitor::ApplyTransientReadabilityFilter(
        previous,
        current,
        {},
        0,
        3,
        2);
    auto filtered_it = filtered.snapshot.find(7);
    Expect(filtered_it != filtered.snapshot.end(), "filtered snapshot should keep the channel");
    Expect(filtered_it->second.readable, "first unreadable poll should keep the previous readable state");
    Expect(filtered.unreadable_counts.at(7) == 1, "first unreadable poll should increment the failure count");
    Expect(filtered.cycle_degraded, "single managed channel miss should count as a degraded cycle");
}

void TestUnreadablePollKeepsLastConfirmedStateAfterThreshold() {
    const std::map<int, ManagedChannelInputState> previous{{8, MakeState(8, "A", 8, false)}};
    const std::map<int, ManagedChannelInputState> current{{8, MakeState(8, "", 0, false, false)}};
    const auto filtered = WingConnector::ManagedSourceMonitor::ApplyTransientReadabilityFilter(
        previous,
        current,
        {{8, 2}},
        2,
        3,
        2);
    auto filtered_it = filtered.snapshot.find(8);
    Expect(filtered_it != filtered.snapshot.end(), "threshold snapshot should still contain the channel");
    Expect(filtered_it->second.readable, "missing UDP replies must not replace the last confirmed state");
    const auto decision = ClassifyChange(previous, filtered.snapshot);
    Expect(decision.action == Action::None, "missing replies must not produce an invalid-source warning");
}

void TestTransientExplicitOffIsMasked() {
    const std::map<int, ManagedChannelInputState> previous{{4, MakeState(4, "A", 4, false)}};
    const std::map<int, ManagedChannelInputState> current{{4, MakeState(4, "OFF", 0, false)}};
    const auto filtered = WingConnector::ManagedSourceMonitor::ApplyTransientReadabilityFilter(
        previous, current, {}, 0, 3, 2);
    Expect(filtered.snapshot.at(4).source_group == "A",
           "first explicit OFF poll should preserve the last confirmed source");
    Expect(ClassifyChange(previous, filtered.snapshot).action == Action::None,
           "first explicit OFF poll should not warn");
}

void TestConfirmedExplicitOffWarns() {
    const std::map<int, ManagedChannelInputState> previous{{4, MakeState(4, "A", 4, false)}};
    const std::map<int, ManagedChannelInputState> current{{4, MakeState(4, "OFF", 0, false)}};
    const auto filtered = WingConnector::ManagedSourceMonitor::ApplyTransientReadabilityFilter(
        previous, current, {{4, 2}}, 0, 3, 2);
    Expect(filtered.snapshot.at(4).source_group == "OFF",
           "third explicit OFF poll should expose the confirmed state");
    Expect(ClassifyChange(previous, filtered.snapshot).action == Action::WarnInvalidSource,
           "confirmed explicit OFF state should warn");
}

void TestMissingStereoModeKeepsLastConfirmedTopology() {
    const std::map<int, ManagedChannelInputState> previous{{12, MakeState(12, "A", 12, true)}};
    auto current_state = MakeState(12, "A", 12, false);
    current_state.stereo_readable = false;
    const std::map<int, ManagedChannelInputState> current{{12, current_state}};
    const auto filtered = WingConnector::ManagedSourceMonitor::ApplyTransientReadabilityFilter(
        previous, current, {}, 0, 3, 2);
    Expect(filtered.snapshot.at(12).stereo_linked,
           "missing mode reply should preserve the last confirmed stereo topology");
    Expect(ClassifyChange(previous, filtered.snapshot).action == Action::None,
           "missing mode reply should not produce a topology warning");
}

void TestFullCycleGlitchUsesCycleGrace() {
    const std::map<int, ManagedChannelInputState> previous{
        {9, MakeState(9, "A", 9, false)},
        {10, MakeState(10, "A", 10, false)},
    };
    const std::map<int, ManagedChannelInputState> current{
        {9, MakeState(9, "", 0, false, false)},
        {10, MakeState(10, "", 0, false, false)},
    };
    const auto filtered = WingConnector::ManagedSourceMonitor::ApplyTransientReadabilityFilter(
        previous,
        current,
        {},
        0,
        3,
        2);
    Expect(filtered.cycle_degraded, "all-unreadable cycle should be treated as degraded");
    Expect(filtered.degraded_cycle_count == 1, "first degraded cycle should stay within the grace window");
    Expect(filtered.snapshot.at(9).readable && filtered.snapshot.at(10).readable,
           "first degraded cycle should keep the previous readable snapshot");
}

} // namespace

int main() {
    TestNoChange();
    TestMonoSourceChangeReapplies();
    TestStereoSourceChangeReapplies();
    TestTopologyChangeWarns();
    TestInvalidSourceWarns();
    TestTransientExplicitOffIsMasked();
    TestConfirmedExplicitOffWarns();
    TestUnreadableSourceWarns();
    TestMixedChangesPreferWarning();
    TestBootstrapDoesNotTrigger();
    TestUnreadablePollIsMaskedBeforeThreshold();
    TestUnreadablePollKeepsLastConfirmedStateAfterThreshold();
    TestMissingStereoModeKeepsLastConfirmedTopology();
    TestFullCycleGlitchUsesCycleGrace();

    std::cout << "source_monitor_tests: OK\n";
    return 0;
}
