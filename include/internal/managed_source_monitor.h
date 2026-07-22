#ifndef MANAGED_SOURCE_MONITOR_H
#define MANAGED_SOURCE_MONITOR_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "wingconnector/wing_osc.h"

namespace WingConnector {
namespace ManagedSourceMonitor {

enum class Action {
    None,
    ReapplyRouting,
    WarnTopologyChange,
    WarnInvalidSource,
};

struct Decision {
    Action action = Action::None;
    std::vector<int> changed_channels;
};

struct FilterResult {
    std::map<int, ManagedChannelInputState> snapshot;
    std::map<int, int> unreadable_counts;
    int degraded_cycle_count = 0;
    bool cycle_degraded = false;
};

inline bool IsValidState(const ManagedChannelInputState& state) {
    return state.readable && !state.source_group.empty() && state.source_group != "OFF" && state.source_input > 0;
}

inline size_t CountReadableStates(const std::map<int, ManagedChannelInputState>& states) {
    size_t readable = 0;
    for (const auto& [channel_number, state] : states) {
        (void)channel_number;
        if (state.readable) {
            readable++;
        }
    }
    return readable;
}

inline FilterResult ApplyTransientReadabilityFilter(
    const std::map<int, ManagedChannelInputState>& previous,
    const std::map<int, ManagedChannelInputState>& current,
    const std::map<int, int>& previous_unreadable_counts,
    int previous_degraded_cycle_count,
    int per_channel_threshold,
    int degraded_cycle_threshold) {
    FilterResult result;
    result.snapshot = current;
    result.unreadable_counts = previous_unreadable_counts;

    result.cycle_degraded = !previous.empty() && CountReadableStates(current) == 0;
    if (result.cycle_degraded) {
        result.degraded_cycle_count = previous_degraded_cycle_count + 1;
    } else {
        result.degraded_cycle_count = 0;
    }

    for (const auto& [channel_number, current_state] : current) {
        if (IsValidState(current_state)) {
            result.unreadable_counts[channel_number] = 0;
            continue;
        }

        auto previous_it = previous.find(channel_number);
        if (previous_it == previous.end() || !IsValidState(previous_it->second)) {
            result.unreadable_counts[channel_number] = 0;
            continue;
        }

        const int consecutive_failures = result.unreadable_counts[channel_number] + 1;
        result.unreadable_counts[channel_number] = consecutive_failures;
        // A partial UDP reply is not evidence that routing changed. Explicit
        // invalid/OFF values can also appear briefly while the WING updates
        // related fields, so require consecutive confirmations before exposing
        // the state to the change classifier.
        if (!current_state.readable || consecutive_failures < per_channel_threshold) {
            result.snapshot[channel_number] = previous_it->second;
        }
    }

    for (auto& [channel_number, current_state] : result.snapshot) {
        auto previous_it = previous.find(channel_number);
        if (current_state.readable && !current_state.stereo_readable &&
            previous_it != previous.end() && IsValidState(previous_it->second) &&
            current_state.source_group == previous_it->second.source_group &&
            current_state.source_input == previous_it->second.source_input) {
            current_state.stereo_linked = previous_it->second.stereo_linked;
            current_state.stereo_readable = previous_it->second.stereo_readable;
        }
    }

    (void)degraded_cycle_threshold;

    return result;
}

inline Decision ClassifyChange(const std::map<int, ManagedChannelInputState>& previous,
                               const std::map<int, ManagedChannelInputState>& current) {
    Decision decision;
    std::set<int> changed;

    for (const auto& [channel_number, previous_state] : previous) {
        auto current_it = current.find(channel_number);
        if (current_it == current.end()) {
            changed.insert(channel_number);
            decision.action = Action::WarnInvalidSource;
            continue;
        }

        const ManagedChannelInputState& current_state = current_it->second;
        const bool previous_valid = IsValidState(previous_state);
        const bool current_valid = IsValidState(current_state);

        if (!current_valid) {
            if (previous_valid ||
                previous_state.readable != current_state.readable ||
                previous_state.source_group != current_state.source_group ||
                previous_state.source_input != current_state.source_input) {
                changed.insert(channel_number);
                decision.action = Action::WarnInvalidSource;
            }
            continue;
        }

        if (!previous_valid) {
            changed.insert(channel_number);
            if (decision.action != Action::WarnInvalidSource) {
                decision.action = Action::ReapplyRouting;
            }
            continue;
        }

        if (previous_state.stereo_linked != current_state.stereo_linked) {
            changed.insert(channel_number);
            decision.action = Action::WarnTopologyChange;
            continue;
        }

        if (previous_state.source_group != current_state.source_group ||
            previous_state.source_input != current_state.source_input) {
            changed.insert(channel_number);
            if (decision.action == Action::None) {
                decision.action = Action::ReapplyRouting;
            }
        }
    }

    decision.changed_channels.assign(changed.begin(), changed.end());
    return decision;
}

}  // namespace ManagedSourceMonitor
}  // namespace WingConnector

#endif  // MANAGED_SOURCE_MONITOR_H
