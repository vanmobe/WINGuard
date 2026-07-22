/*
 * macOS Native AUDIOLAB.wing.reaper.virtualsoundcheck Dialog Implementation
 * Provides consolidated dialog for all Wing operations
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include "internal/wing_connector_dialog_macos.h"
#include "wingconnector/reaper_extension.h"
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace WingConnector;

namespace {

constexpr bool kShowBridgeTabInMainUI = false;

std::vector<WingConnector::ChannelSelectionInfo> SelectedSourcesOnly(
    const std::vector<WingConnector::ChannelSelectionInfo>& all_sources) {
    std::vector<WingConnector::ChannelSelectionInfo> selected;
    for (const auto& source : all_sources) {
        if (source.selected) {
            selected.push_back(source);
        }
    }
    return selected;
}

}  // namespace

@interface AdoptionEditorCoordinator : NSObject
{
@public
    std::vector<AdoptionEditorRow> rows;
    NSSegmentedControl* modeControl;
    NSTextField* warningLabel;
    NSButton* applyButton;
    NSMutableArray* channelPopups;
    NSMutableArray* slotPopups;
}
- (void)rebuildSlotChoices;
- (void)selectionChanged:(id)sender;
@end

@implementation AdoptionEditorCoordinator

- (NSString*)slotLabelForStart:(int)slotStart stereo:(BOOL)stereo {
    if (stereo) {
        return [NSString stringWithFormat:@"%d-%d", slotStart, slotStart + 1];
    }
    return [NSString stringWithFormat:@"%d", slotStart];
}

- (void)rebuildSlotChoices {
    const bool cardMode = [modeControl selectedSegment] == 1;
    const int slotLimit = cardMode ? 32 : 48;

    for (NSInteger i = 0; i < (NSInteger)[slotPopups count]; ++i) {
        NSPopUpButton* popup = [slotPopups objectAtIndex:i];
        const auto& row = rows[(size_t)i];
        const NSInteger previousTag = [[popup selectedItem] tag];
        [popup removeAllItems];

        NSString* autoLabel = @"Auto";
        if (row.suggested_slot_start > 0) {
            autoLabel = [NSString stringWithFormat:@"Auto (%@)",
                         [self slotLabelForStart:row.suggested_slot_start stereo:row.stereo_like]];
        }
        [popup addItemWithTitle:autoLabel];
        [[popup lastItem] setTag:0];

        if (row.stereo_like) {
            for (int slot = 1; slot + 1 <= slotLimit; slot += 2) {
                [popup addItemWithTitle:[self slotLabelForStart:slot stereo:YES]];
                [[popup lastItem] setTag:slot];
            }
        } else {
            for (int slot = 1; slot <= slotLimit; ++slot) {
                [popup addItemWithTitle:[self slotLabelForStart:slot stereo:NO]];
                [[popup lastItem] setTag:slot];
            }
        }

        const NSInteger matchingIndex = [popup indexOfItemWithTag:previousTag];
        if (previousTag > 0 && matchingIndex >= 0) {
            [popup selectItemAtIndex:matchingIndex];
        } else {
            [popup selectItemAtIndex:0];
        }
    }

    [self selectionChanged:nil];
}

- (void)selectionChanged:(id)sender {
    (void)sender;
    std::set<int> chosenChannels;
    std::set<int> chosenSlots;
    std::string warning;

    for (NSInteger i = 0; i < (NSInteger)[channelPopups count]; ++i) {
        NSPopUpButton* channelPopup = [channelPopups objectAtIndex:i];
        NSPopUpButton* slotPopup = [slotPopups objectAtIndex:i];
        const auto& row = rows[(size_t)i];

        const int channel = (int)[[channelPopup selectedItem] tag];
        if (!chosenChannels.insert(channel).second) {
            warning = "Duplicate WING channel selected. Each channel can only be assigned once.";
            break;
        }

        const int slotStart = (int)[[slotPopup selectedItem] tag];
        if (slotStart <= 0) {
            continue;
        }

        if (row.stereo_like && (slotStart % 2) == 0) {
            warning = "Stereo rows must start on an odd playback slot.";
            break;
        }

        const int slotEnd = row.stereo_like ? (slotStart + 1) : slotStart;
        for (int slot = slotStart; slot <= slotEnd; ++slot) {
            if (!chosenSlots.insert(slot).second) {
                warning = "Duplicate playback slot selected. Resolve slot conflicts before applying.";
                break;
            }
        }
        if (!warning.empty()) {
            break;
        }
    }

    if (warning.empty()) {
        [warningLabel setStringValue:@"No conflicts detected."];
        [warningLabel setTextColor:[NSColor secondaryLabelColor]];
        [applyButton setEnabled:YES];
    } else {
        [warningLabel setStringValue:[NSString stringWithUTF8String:warning.c_str()]];
        [warningLabel setTextColor:[NSColor systemRedColor]];
        [applyButton setEnabled:NO];
    }
}

@end

// ===== CHANNEL SELECTION DIALOG =====

extern "C" {

bool ShowChannelSelectionDialog(std::vector<WingConnector::ChannelSelectionInfo>& channels,
                                const char* title,
                                const char* description,
                                bool& setup_soundcheck,
                                bool& overwrite_existing) {
    @autoreleasepool {
        NSAlert* alert = [[[NSAlert alloc] init] autorelease];
        [alert setMessageText:[NSString stringWithUTF8String:title]];
        [alert setInformativeText:[NSString stringWithUTF8String:description]];
        [alert setAlertStyle:NSAlertStyleInformational];

        // Calculate height needed for all channels
        int numChannels = (int)channels.size();
        int rowHeight = 24;
        int maxHeight = 400;
        int scrollHeight = std::min(numChannels * rowHeight + 20, maxHeight);

        // Create scrollable view for checkboxes
        NSScrollView* scrollView = [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 500, scrollHeight)] autorelease];
        [scrollView setHasVerticalScroller:YES];
        [scrollView setHasHorizontalScroller:NO];
        [scrollView setBorderType:NSBezelBorder];

        // Document view to hold all checkboxes
        NSView* documentView = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, numChannels * rowHeight)] autorelease];

        // Create checkbox array to track user selections
        NSMutableArray* checkboxes = [NSMutableArray arrayWithCapacity:numChannels];

        // Add checkbox for each channel
        int yPos = numChannels * rowHeight - rowHeight;
        for (int i = 0; i < numChannels; i++) {
            const auto& ch = channels[i];
            NSString* kindLabel = @"SRC";
            switch (ch.kind) {
                case SourceKind::Channel: kindLabel = @"CH"; break;
                case SourceKind::Bus: kindLabel = @"BUS"; break;
                case SourceKind::Main: kindLabel = @"MAIN"; break;
                case SourceKind::Matrix: kindLabel = @"MTX"; break;
            }
            const std::string display_name = ch.name.empty()
                ? (std::string([kindLabel UTF8String]) + " " + std::to_string(ch.source_number))
                : ch.name;

            // Create title showing channel info
            NSString* title = nil;
            if (ch.stereo_linked && !ch.partner_source_group.empty()) {
                title = [NSString stringWithFormat:@"%@%02d  %s  [%s%d / %s%d]%s",
                         kindLabel,
                         ch.source_number,
                         display_name.c_str(),
                         ch.source_group.c_str(),
                         ch.source_input,
                         ch.partner_source_group.c_str(),
                         ch.partner_source_input,
                         ch.soundcheck_capable ? "" : " [Record only]"];
            } else {
                title = [NSString stringWithFormat:@"%@%02d  %s  [%s%d]%s%s",
                         kindLabel,
                         ch.source_number,
                         display_name.c_str(),
                         ch.source_group.c_str(),
                         ch.source_input,
                         ch.stereo_linked ? " [Stereo]" : "",
                         ch.soundcheck_capable ? "" : " [Record only]"];
            }

            NSButton* checkbox = [[[NSButton alloc] initWithFrame:NSMakeRect(10, yPos, 460, 20)] autorelease];
            [checkbox setButtonType:NSButtonTypeSwitch];
            [checkbox setTitle:title];
            [checkbox setState:ch.selected ? NSControlStateValueOn : NSControlStateValueOff];

            [documentView addSubview:checkbox];
            [checkboxes addObject:checkbox];

            yPos -= rowHeight;
        }

        [scrollView setDocumentView:documentView];
        NSClipView* clipView = [scrollView contentView];
        NSRect clipBounds = [clipView bounds];
        NSRect documentBounds = [documentView bounds];
        CGFloat topOffset = std::max<CGFloat>(0.0, NSMaxY(documentBounds) - NSHeight(clipBounds));
        [clipView scrollToPoint:NSMakePoint(0, topOffset)];
        [scrollView reflectScrolledClipView:clipView];

        // Create container view for scroll view + options
        NSView* containerView = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 500, scrollHeight + 70)] autorelease];

        // Position scroll view at top of container
        [scrollView setFrameOrigin:NSMakePoint(0, 70)];
        [containerView addSubview:scrollView];

        // Add soundcheck mode checkbox at bottom
        NSButton* overwriteCheckbox = [[[NSButton alloc] initWithFrame:NSMakeRect(10, 36, 480, 20)] autorelease];
        [overwriteCheckbox setButtonType:NSButtonTypeSwitch];
        [overwriteCheckbox setTitle:@"Replace all existing REAPER tracks when applying this source selection"];
        [overwriteCheckbox setState:overwrite_existing ? NSControlStateValueOn : NSControlStateValueOff];
        [containerView addSubview:overwriteCheckbox];

        NSButton* soundcheckCheckbox = [[[NSButton alloc] initWithFrame:NSMakeRect(10, 10, 480, 20)] autorelease];
        [soundcheckCheckbox setButtonType:NSButtonTypeSwitch];
        [soundcheckCheckbox setTitle:@"Configure soundcheck mode for selected channels only (ALT + REAPER playback inputs)"];
        [soundcheckCheckbox setState:setup_soundcheck ? NSControlStateValueOn : NSControlStateValueOff];
        [containerView addSubview:soundcheckCheckbox];

        [alert setAccessoryView:containerView];

        // Add buttons
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        // Show dialog
        NSInteger result = [alert runModal];

        if (result == NSAlertFirstButtonReturn) {
            // OK clicked - update selection states
            for (int i = 0; i < numChannels; i++) {
                NSButton* checkbox = [checkboxes objectAtIndex:i];
                channels[i].selected = ([checkbox state] == NSControlStateValueOn);
            }
            // Update soundcheck mode option
            setup_soundcheck = ([soundcheckCheckbox state] == NSControlStateValueOn);
            overwrite_existing = ([overwriteCheckbox state] == NSControlStateValueOn);
            return true;
        }

        // Cancel clicked
        return false;
    }
}

bool ShowExistingProjectAdoptionEditor(const std::vector<AdoptionEditorRow>& rows,
                                       const std::vector<int>& available_channels,
                                       const char* initial_output_mode,
                                       std::string& output_mode_out,
                                       std::string& channel_overrides_spec_out,
                                       std::string& slot_overrides_spec_out,
                                       bool& apply_now_out) {
    @autoreleasepool {
        if (rows.empty()) {
            return false;
        }

        while (true) {
            // Drain temporary Cocoa ownership after every retry, not only when
            // the editor eventually returns from this function.
            NSAutoreleasePool* iterationPool = [[NSAutoreleasePool alloc] init];
            NSAlert* alert = [[[NSAlert alloc] init] autorelease];
            [alert setMessageText:@"Editable Existing Project Adoption"];
            [alert setInformativeText:@"Review or override the proposed channel mapping before applying. Slot overrides use Auto by default and only offer valid choices."];
            [alert setAlertStyle:NSAlertStyleInformational];

            const CGFloat rowHeight = 26.0;
            const CGFloat headerHeight = 26.0;
            const CGFloat footerHeight = 78.0;
            const CGFloat width = 780.0;
            const CGFloat scrollHeight = std::min<CGFloat>(360.0, headerHeight + rows.size() * rowHeight + 12.0);

            NSView* containerView = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, scrollHeight + footerHeight)] autorelease];
            NSSegmentedControl* modeControl = [[[NSSegmentedControl alloc] initWithFrame:NSMakeRect(12, 12, 180, 26)] autorelease];
            [modeControl setSegmentCount:2];
            [modeControl setLabel:@"USB" forSegment:0];
            [modeControl setLabel:@"CARD" forSegment:1];
            const std::string initial_mode = initial_output_mode ? initial_output_mode : "USB";
            [modeControl setSelectedSegment:(initial_mode == "CARD") ? 1 : 0];
            [containerView addSubview:modeControl];

            NSTextField* hintLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(210, 12, width - 222, 26)] autorelease];
            [hintLabel setEditable:NO];
            [hintLabel setBordered:NO];
            [hintLabel setDrawsBackground:NO];
            [hintLabel setStringValue:@"Channel changes are required only when you want a different WING channel than the suggestion."];
            [containerView addSubview:hintLabel];

            NSTextField* warningLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(12, 44, width - 24, 20)] autorelease];
            [warningLabel setEditable:NO];
            [warningLabel setBordered:NO];
            [warningLabel setDrawsBackground:NO];
            [warningLabel setStringValue:@"No conflicts detected."];
            [warningLabel setTextColor:[NSColor secondaryLabelColor]];
            [containerView addSubview:warningLabel];

            NSScrollView* scrollView = [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, footerHeight, width, scrollHeight)] autorelease];
            [scrollView setHasVerticalScroller:YES];
            [scrollView setBorderType:NSBezelBorder];

            const CGFloat contentHeight = headerHeight + rows.size() * rowHeight;
            NSView* documentView = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, width - 18, contentHeight)] autorelease];

            NSArray<NSString*>* headerTitles = @[@"Track", @"Stereo", @"Suggested", @"WING Channel", @"Suggested Slot", @"Slot Override"];
            NSArray<NSNumber*>* headerXs = @[@12.0, @250.0, @320.0, @420.0, @540.0, @650.0];
            for (NSInteger i = 0; i < (NSInteger)[headerTitles count]; ++i) {
                NSTextField* label = [[[NSTextField alloc] initWithFrame:NSMakeRect([[headerXs objectAtIndex:i] doubleValue],
                                                                                      contentHeight - headerHeight + 4.0,
                                                                                      (i == 0 ? 220.0 : 100.0),
                                                                                      20.0)] autorelease];
                [label setEditable:NO];
                [label setBordered:NO];
                [label setDrawsBackground:NO];
                [label setFont:[NSFont boldSystemFontOfSize:12.0]];
                [label setStringValue:[headerTitles objectAtIndex:i]];
                [documentView addSubview:label];
            }

            NSMutableArray* channelControls = [NSMutableArray arrayWithCapacity:rows.size()];
            NSMutableArray* slotPopups = [NSMutableArray arrayWithCapacity:rows.size()];

            CGFloat y = contentHeight - headerHeight - rowHeight;
            for (const auto& row : rows) {
                NSTextField* trackLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(12, y + 3.0, 260.0, 20.0)] autorelease];
                [trackLabel setEditable:NO];
                [trackLabel setBordered:NO];
                [trackLabel setDrawsBackground:NO];
                [trackLabel setStringValue:[NSString stringWithFormat:@"%d. %s", row.track_index, row.track_name.c_str()]];
                [documentView addSubview:trackLabel];

                NSTextField* stereoLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(250, y + 3.0, 60.0, 20.0)] autorelease];
                [stereoLabel setEditable:NO];
                [stereoLabel setBordered:NO];
                [stereoLabel setDrawsBackground:NO];
                [stereoLabel setStringValue:row.stereo_like ? @"Stereo" : @"Mono"];
                [documentView addSubview:stereoLabel];

                NSTextField* suggestedLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(320, y + 3.0, 80.0, 20.0)] autorelease];
                [suggestedLabel setEditable:NO];
                [suggestedLabel setBordered:NO];
                [suggestedLabel setDrawsBackground:NO];
                [suggestedLabel setStringValue:[NSString stringWithFormat:@"CH%d", row.suggested_channel]];
                [documentView addSubview:suggestedLabel];

                NSPopUpButton* channelPopup = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(420, y, 100.0, 26.0) pullsDown:NO] autorelease];
                for (int channel_number : available_channels) {
                    NSString* title = [NSString stringWithFormat:@"CH%d", channel_number];
                    [channelPopup addItemWithTitle:title];
                    [[channelPopup lastItem] setTag:channel_number];
                }
                [channelPopup selectItemWithTag:row.assigned_channel];
                [documentView addSubview:channelPopup];
                [channelControls addObject:channelPopup];

                NSTextField* suggestedSlotLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(540, y + 3.0, 90.0, 20.0)] autorelease];
                [suggestedSlotLabel setEditable:NO];
                [suggestedSlotLabel setBordered:NO];
                [suggestedSlotLabel setDrawsBackground:NO];
                if (row.suggested_slot_start > 0) {
                    if (row.suggested_slot_end > row.suggested_slot_start) {
                        [suggestedSlotLabel setStringValue:[NSString stringWithFormat:@"%d-%d", row.suggested_slot_start, row.suggested_slot_end]];
                    } else {
                        [suggestedSlotLabel setStringValue:[NSString stringWithFormat:@"%d", row.suggested_slot_start]];
                    }
                } else {
                    [suggestedSlotLabel setStringValue:@"-"];
                }
                [documentView addSubview:suggestedSlotLabel];

                NSPopUpButton* slotPopup = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(650, y, 110.0, 26.0) pullsDown:NO] autorelease];
                [documentView addSubview:slotPopup];
                [slotPopups addObject:slotPopup];

                y -= rowHeight;
            }

            [scrollView setDocumentView:documentView];
            [containerView addSubview:scrollView];
            [alert setAccessoryView:containerView];

            [alert addButtonWithTitle:@"Apply"];
            [alert addButtonWithTitle:@"Cancel"];
            NSButton* applyButton = [[alert buttons] objectAtIndex:0];

            AdoptionEditorCoordinator* coordinator = [[[AdoptionEditorCoordinator alloc] init] autorelease];
            coordinator->rows = rows;
            coordinator->modeControl = modeControl;
            coordinator->warningLabel = warningLabel;
            coordinator->applyButton = applyButton;
            coordinator->channelPopups = channelControls;
            coordinator->slotPopups = slotPopups;

            [modeControl setTarget:coordinator];
            [modeControl setAction:@selector(rebuildSlotChoices)];
            for (NSPopUpButton* popup in channelControls) {
                [popup setTarget:coordinator];
                [popup setAction:@selector(selectionChanged:)];
            }
            for (NSPopUpButton* popup in slotPopups) {
                [popup setTarget:coordinator];
                [popup setAction:@selector(selectionChanged:)];
            }
            [coordinator rebuildSlotChoices];

            const NSInteger result = [alert runModal];
            if (result != NSAlertFirstButtonReturn) {
                apply_now_out = false;
                [iterationPool drain];
                return false;
            }

            std::set<int> chosen_channels;
            std::set<int> overridden_slots;
            std::ostringstream channel_spec;
            std::ostringstream slot_spec;
            bool first_channel_override = true;
            bool first_slot_override = true;
            bool has_conflict = false;
            std::string conflict_message;

            for (NSInteger i = 0; i < (NSInteger)rows.size(); ++i) {
                NSPopUpButton* channelPopup = [channelControls objectAtIndex:i];
                NSPopUpButton* slotPopup = [slotPopups objectAtIndex:i];
                const auto& row = rows[(size_t)i];
                const int chosen_channel = (int)[[channelPopup selectedItem] tag];
                if (!chosen_channels.insert(chosen_channel).second) {
                    has_conflict = true;
                    conflict_message = "Each WING channel can only be assigned once. Resolve duplicate channel selections before applying.";
                    break;
                }
                if (chosen_channel != row.suggested_channel) {
                    if (!first_channel_override) {
                        channel_spec << ";";
                    }
                    channel_spec << row.track_index << "=CH" << chosen_channel;
                    first_channel_override = false;
                }

                const int chosen_slot = (int)[[slotPopup selectedItem] tag];
                if (chosen_slot <= 0) {
                    continue;
                }

                const std::string slot_value = row.stereo_like
                    ? (std::to_string(chosen_slot) + "-" + std::to_string(chosen_slot + 1))
                    : std::to_string(chosen_slot);
                if (!first_slot_override) {
                    slot_spec << ";";
                }
                slot_spec << row.track_index << "=" << slot_value;
                first_slot_override = false;

                const size_t dash = slot_value.find('-');
                try {
                    const int slot_start = std::stoi(slot_value.substr(0, dash));
                    const int slot_end = (dash == std::string::npos) ? slot_start : std::stoi(slot_value.substr(dash + 1));
                    for (int slot = slot_start; slot <= slot_end; ++slot) {
                        if (!overridden_slots.insert(slot).second) {
                            has_conflict = true;
                            conflict_message = "A playback slot override is used more than once. Resolve duplicate slot overrides before applying.";
                            break;
                        }
                    }
                    if (has_conflict) {
                        break;
                    }
                } catch (...) {
                    has_conflict = true;
                    conflict_message = "Playback slot overrides must look like 9 or 9-10.";
                    break;
                }
            }

            if (has_conflict) {
                NSAlert* conflictAlert = [[[NSAlert alloc] init] autorelease];
                [conflictAlert setMessageText:@"Fix Adoption Conflicts"];
                [conflictAlert setInformativeText:[NSString stringWithUTF8String:conflict_message.c_str()]];
                [conflictAlert addButtonWithTitle:@"OK"];
                [conflictAlert runModal];
                [iterationPool drain];
                continue;
            }

            output_mode_out = ([modeControl selectedSegment] == 1) ? "CARD" : "USB";
            channel_overrides_spec_out = channel_spec.str();
            slot_overrides_spec_out = slot_spec.str();
            apply_now_out = true;
            [iterationPool drain];
            return true;
        }
    }
}

} // extern "C"

// ===== MAIN WING CONNECTOR WINDOW =====

@interface WingConnectorFlippedView : NSView
@end

@implementation WingConnectorFlippedView
- (BOOL)isFlipped {
    return NO;
}
@end

@interface WingConnectorWindowController : NSWindowController <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
{
    // UI Elements
    NSScrollView* mainScrollView;
    WingConnectorFlippedView* formContentView;
    NSWindow* debugLogWindow;
    NSPopUpButton* wingDropdown;
    NSTextField* manualIPField;
    NSButton* scanButton;
    NSMutableArray* discoveredIPs;
    NSImageView* statusIconView;
    NSTextField* statusLabel;
    NSImageView* midiStatusIconView;
    NSTextField* midiStatusLabel;
    NSImageView* recorderStatusIconView;
    NSTextField* recorderStatusLabel;
    NSButton* setupSoundcheckButton;
    NSButton* applyPendingSetupButton;
    NSButton* discardPendingSetupButton;
    NSButton* toggleSoundcheckButton;
    NSButton* connectButton;
    NSImageView* validationIconView;
    NSTextField* validationStatusLabel;
    NSTabViewItem* consoleTabItem;
    NSTabViewItem* reaperTabItem;
    NSTabViewItem* wingTabItemRef;
    NSTabViewItem* controlIntegrationTabItem;
    NSTabViewItem* bridgeTabItemRef;
    NSTextField* consoleTabStatusLabel;
    NSTextField* reaperTabStatusLabel;
    NSTextField* wingTabStatusLabel;
    NSTextField* controlIntegrationTabStatusLabel;
    NSTextField* bridgeTabStatusLabel;
    NSTextField* pendingSetupSummaryLabel;
    NSTextField* setupReadinessDetailLabel;
    NSTextField* setupSoundcheckDescriptionLabel;
    NSTabView* settingsTabView;
    NSTextView* activityLogView;
    NSScrollView* logScrollView;
    NSButton* debugLogToggleButton;
    NSSegmentedControl* outputModeControl;
    NSSegmentedControl* midiActionsControl;
    NSButton* applyMidiActionsButton;
    NSButton* discardMidiActionsButton;
    NSTextField* midiActionsSummaryLabel;
    NSTextField* midiActionsDetailLabel;
    NSSegmentedControl* autoRecordEnableControl;
    NSSegmentedControl* autoRecordModeControl;
    NSTextField* thresholdField;
    NSTextField* holdField;
    NSButton* applyAutomationButton;
    NSTextField* automationDetailLabel;
    NSButton* applyRecorderButton;
    NSTextField* recorderDetailLabel;
    NSTextField* recorderFollowHintLabel;
    NSSegmentedControl* recorderEnableControl;
    NSPopUpButton* monitorTrackDropdown;
    NSSegmentedControl* recorderTargetControl;
    NSPopUpButton* sdSourceDropdown;
    NSSegmentedControl* recorderFollowControl;
    NSButton* oscOutEnableCheckbox;
    NSTextField* oscHostField;
    NSTextField* oscPortField;
    NSTextField* meterPreviewLabel;
    NSPopUpButton* ccLayerDropdown;
    NSButton* bridgeEnableCheckbox;
    NSPopUpButton* bridgeMidiOutputDropdown;
    NSSegmentedControl* bridgeMessageTypeControl;
    NSPopUpButton* bridgeMidiChannelDropdown;
    NSScrollView* bridgeMappingScrollView;
    NSTableView* bridgeMappingTableView;
    NSPopUpButton* bridgeSourceKindDropdown;
    NSPopUpButton* bridgeSourceNumberDropdown;
    NSTextField* bridgeMidiValueField;
    NSButton* bridgeMappingEnabledCheckbox;
    NSButton* bridgeAddOrUpdateMappingButton;
    NSButton* bridgeRemoveMappingButton;
    NSTextField* bridgeStatusLabel;
    NSButton* applyBridgeButton;
    NSTimer* meterPreviewTimer;

    BOOL isConnected;
    BOOL isWorking;  // Prevents re-entrant button clicks while an operation is in progress
    BOOL liveSetupValidated;  // True when Wing + REAPER routing validate as a complete live setup
    BOOL validationInProgress;  // Prevent overlapping auto-connect/validation runs
    BOOL hasPendingSetupDraft;
    BOOL pendingSetupSoundcheck;
    BOOL pendingReplaceExisting;
    BOOL pendingSetupUsesExistingSelection;
    std::string pendingOutputMode;
    std::vector<WingConnector::ChannelSelectionInfo> pendingSetupChannels;
    CGFloat collapsedContentHeight;
    CGFloat expandedContentHeight;
    BOOL automationSettingsDirty;
    BOOL recorderSettingsDirty;
    BOOL midiActionsDirty;
    BOOL pendingMidiActionsEnabled;
    ValidationState latestLiveSetupValidationState;
    std::string latestLiveSetupValidationDetails;
    ValidationState latestMidiValidationState;
    std::string latestMidiValidationDetails;
}

- (instancetype)init;
- (void)setupUI;
- (void)updateConnectionStatus;
- (void)updateToggleSoundcheckButtonLabel;
- (void)updateSetupSoundcheckButtonLabel;
- (void)updateApplyPendingSetupButtonLabel;
- (void)updateAutoTriggerControlsEnabled;
- (void)updateValidationStatusLabel;
- (void)updatePendingSetupUI;
- (void)updateMidiActionsUI;
- (void)updateSetupReadinessDetails;
- (void)updateAutomationDetails;
- (void)updateRecorderStatusLabel;
- (void)updateMidiStatusLabel;
- (void)updateTabStatusIndicators;
- (void)clearPendingSetupDraft:(BOOL)resetMode;
- (void)setHeaderStatusIcon:(NSImageView*)iconView symbolName:(NSString*)symbolName fallback:(NSString*)fallback color:(NSColor*)color;
- (void)setConnectionStatusText:(NSString*)text color:(NSColor*)color connected:(BOOL)connected;
- (void)setValidationStatusText:(NSString*)text color:(NSColor*)color;
- (void)refreshLiveSetupValidation;
- (void)finalizeFormLayout;
- (void)adjustWindowHeightToFitContent;
- (void)updateFormLayoutForCurrentWindowSize;
- (void)appendToLog:(NSString*)message;
- (void)setWorkingState:(BOOL)working;
- (void)onDebugLogToggled:(id)sender;
- (void)windowDidResize:(NSNotification*)notification;
- (void)createDebugLogWindow;
- (NSString*)bridgeFamilyLabelForKind:(SourceKind)kind;
- (NSInteger)bridgeSourceCountForKind:(SourceKind)kind;
- (void)refreshBridgeSourceNumberDropdown;
- (void)refreshBridgeMappingTable;
- (void)loadBridgeMappingSelectionIntoEditor;
- (void)scrollTabViewToTop:(NSScrollView*)scrollView;

- (void)startDiscoveryScan;
- (void)populateDropdownWithItems:(NSArray*)items ips:(NSArray*)ips;
- (void)onWingDropdownChanged:(id)sender;
- (void)onScanClicked:(id)sender;
- (void)onConnectClicked:(id)sender;
- (NSString*)selectedWingIP;
- (NSString*)selectedOrManualWingIP;
- (void)onManualIPChanged:(id)sender;

- (void)onSetupSoundcheckClicked:(id)sender;
- (void)onApplyPendingSetupClicked:(id)sender;
- (void)onDiscardPendingSetupClicked:(id)sender;
- (void)onToggleSoundcheckClicked:(id)sender;
- (void)onOutputModeChanged:(id)sender;
- (void)onMidiActionsToggled:(id)sender;
- (void)onApplyMidiActionsClicked:(id)sender;
- (void)onDiscardMidiActionsClicked:(id)sender;
- (void)onAutoRecordSettingsChanged:(id)sender;
- (void)onRecorderSettingsChanged:(id)sender;
- (void)onApplyAutomationSettingsClicked:(id)sender;
- (void)onApplyRecorderSettingsClicked:(id)sender;
- (void)refreshMonitorTrackDropdown;
- (void)onMonitorTrackChanged:(id)sender;
- (void)persistConfigAndLog:(NSString*)message;
- (void)onMeterPreviewTimer:(NSTimer*)timer;
- (void)syncPendingAutomationSettingsFromUI;
- (void)syncPendingRecorderSettingsFromUI;
- (void)refreshBridgeMidiOutputDropdown;
- (void)syncBridgeSettingsFromUI;
- (void)resetBridgeMappingEditor;
- (void)onBridgeSettingsChanged:(id)sender;
- (void)onBridgeSourceKindChanged:(id)sender;
- (void)onAddOrUpdateBridgeMappingClicked:(id)sender;
- (void)onRemoveBridgeMappingClicked:(id)sender;
- (void)onApplyBridgeSettingsClicked:(id)sender;
- (void)refreshBridgeStatus;

- (void)runSetupSoundcheckFlow;
- (void)runApplyPendingSetupFlow;
- (void)runToggleSoundcheckModeFlow;
- (void)selectTabWithIdentifier:(NSString*)identifier;

@end

@implementation WingConnectorWindowController

- (instancetype)init {
    // Create the window with modern styling
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 860, 780)
                                                     styleMask:(NSWindowStyleMaskTitled |
                                                               NSWindowStyleMaskClosable |
                                                               NSWindowStyleMaskMiniaturizable |
                                                               NSWindowStyleMaskResizable)
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    [window setTitle:@"WINGuard"];
    [window setMinSize:NSMakeSize(820, 560)];
    [window center];

    self = [super initWithWindow:window];
    [window release];  // NSWindowController retains the window, release our creation reference
    if (!self) {
        return nil;
    }

    [window setDelegate:self];
    discoveredIPs = [[NSMutableArray alloc] init];  // Explicitly retain
    isConnected = NO;
    isWorking = NO;
    liveSetupValidated = NO;
    validationInProgress = NO;
    hasPendingSetupDraft = NO;
    pendingSetupSoundcheck = YES;
    pendingReplaceExisting = YES;
    pendingSetupUsesExistingSelection = NO;
    pendingOutputMode = ReaperExtension::Instance().GetConfig().soundcheck_output_mode;
    automationSettingsDirty = NO;
    recorderSettingsDirty = ReaperExtension::Instance().GetConfig().recorder_coordination_enabled ? YES : NO;
    midiActionsDirty = NO;
    pendingMidiActionsEnabled = ReaperExtension::Instance().IsMidiActionsEnabled();
    latestLiveSetupValidationState = ValidationState::NotReady;
    latestLiveSetupValidationDetails = "Connect to a Wing to validate the current managed setup, then rebuild it only when routing or recording mode needs to change.";
    latestMidiValidationState = ReaperExtension::Instance().IsMidiActionsEnabled() ? ValidationState::Warning : ValidationState::NotReady;
    latestMidiValidationDetails = ReaperExtension::Instance().IsMidiActionsEnabled()
        ? "MIDI shortcuts are enabled, but their WING button mapping has not been checked yet."
        : "MIDI shortcuts are disabled.";
    meterPreviewTimer = nil;
    collapsedContentHeight = 780.0;
    expandedContentHeight = 780.0;

    // MUST call setupUI FIRST to initialize activityLogView!
    [self setupUI];
    [self updateConnectionStatus];

    // Set up log callback to capture C++ Log() calls
    auto log_lambda = [self](const std::string& msg) {
        NSString* nsMsg = [NSString stringWithUTF8String:msg.c_str()];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self appendToLog:nsMsg];
        });
    };
    ReaperExtension::Instance().SetLogCallback(log_lambda);

    [self appendToLog:@"\nScanning network for Wing consoles...\n"];

    // Auto-scan for Wings on the network
    [self startDiscoveryScan];
    meterPreviewTimer = [[NSTimer scheduledTimerWithTimeInterval:0.5
                                                          target:self
                                                        selector:@selector(onMeterPreviewTimer:)
                                                        userInfo:nil
                                                         repeats:YES] retain];

    return self;
}

- (void)dealloc {
    // The extension outlives this modeless controller, so it must not retain a
    // callback that can dispatch back to this object after teardown.
    ReaperExtension::Instance().SetLogCallback({});
    [discoveredIPs release];
    // Release UI elements that we retain in instance variables
    [wingDropdown release];
    [mainScrollView release];
    [formContentView release];
    [debugLogWindow release];
    [manualIPField release];
    [scanButton release];
    [statusIconView release];
    [statusLabel release];
    [midiStatusIconView release];
    [midiStatusLabel release];
    [recorderStatusIconView release];
    [recorderStatusLabel release];
    [setupSoundcheckButton release];
    [toggleSoundcheckButton release];
    [connectButton release];
    [validationIconView release];
    [validationStatusLabel release];
    [consoleTabItem release];
    [reaperTabItem release];
    [wingTabItemRef release];
    [controlIntegrationTabItem release];
    [bridgeTabItemRef release];
    [consoleTabStatusLabel release];
    [reaperTabStatusLabel release];
    [wingTabStatusLabel release];
    [controlIntegrationTabStatusLabel release];
    [bridgeTabStatusLabel release];
    [setupSoundcheckDescriptionLabel release];
    [setupReadinessDetailLabel release];
    [settingsTabView release];
    [activityLogView release];
    [logScrollView release];
    [debugLogToggleButton release];
    [outputModeControl release];
    [midiActionsControl release];
    [applyMidiActionsButton release];
    [discardMidiActionsButton release];
    [midiActionsSummaryLabel release];
    [midiActionsDetailLabel release];
    [autoRecordEnableControl release];
    [autoRecordModeControl release];
    [thresholdField release];
    [holdField release];
    [applyAutomationButton release];
    [automationDetailLabel release];
    [applyRecorderButton release];
    [recorderDetailLabel release];
    [recorderFollowHintLabel release];
    [recorderEnableControl release];
    [monitorTrackDropdown release];
    [recorderTargetControl release];
    [sdSourceDropdown release];
    [recorderFollowControl release];
    [oscOutEnableCheckbox release];
    [oscHostField release];
    [oscPortField release];
    [meterPreviewLabel release];
    [ccLayerDropdown release];
    [bridgeEnableCheckbox release];
    [bridgeMidiOutputDropdown release];
    [bridgeMessageTypeControl release];
    [bridgeMidiChannelDropdown release];
    [bridgeMappingScrollView release];
    [bridgeMappingTableView release];
    [bridgeSourceKindDropdown release];
    [bridgeSourceNumberDropdown release];
    [bridgeMidiValueField release];
    [bridgeMappingEnabledCheckbox release];
    [bridgeAddOrUpdateMappingButton release];
    [bridgeRemoveMappingButton release];
    [bridgeStatusLabel release];
    [applyBridgeButton release];
    [meterPreviewTimer invalidate];
    [meterPreviewTimer release];
    [super dealloc];
}

- (void)setupUI {
    NSView* windowContentView = [[self window] contentView];
    const CGFloat contentWidth = NSWidth([windowContentView bounds]);
    mainScrollView = [[NSScrollView alloc] initWithFrame:[windowContentView bounds]];
    [mainScrollView setHasVerticalScroller:YES];
    [mainScrollView setHasHorizontalScroller:NO];
    [mainScrollView setBorderType:NSNoBorder];
    [mainScrollView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [windowContentView addSubview:mainScrollView];

    formContentView = [[WingConnectorFlippedView alloc] initWithFrame:NSMakeRect(0, 0, NSWidth([windowContentView bounds]), expandedContentHeight)];
    [formContentView setAutoresizingMask:NSViewWidthSizable];
    [mainScrollView setDocumentView:formContentView];

    NSView* contentView = formContentView;
    int yPos = (int)expandedContentHeight - 80;

    // ===== HEADER WITH LOGO =====
    NSBox* headerBox = [[NSBox alloc] initWithFrame:NSMakeRect(0, yPos - 54, contentWidth, 152)];
    [headerBox setBoxType:NSBoxCustom];
    [headerBox setFillColor:[NSColor colorWithWhite:0.95 alpha:1.0]];
    [headerBox setBorderWidth:0];
    [headerBox setAutoresizingMask:(NSViewWidthSizable | NSViewMinYMargin)];
    [contentView addSubview:headerBox];

    // App Icon
    NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(20, yPos, 40, 40)];
    NSImage* appIcon = [NSImage imageNamed:NSImageNameApplicationIcon];
    [iconView setImage:appIcon];
    [iconView setAutoresizingMask:NSViewMinYMargin];
    [contentView addSubview:iconView];

    // Title
    NSTextField* titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(70, yPos + 20, 400, 24)];
    [titleLabel setStringValue:@"WINGuard"];
    [titleLabel setFont:[NSFont systemFontOfSize:18 weight:NSFontWeightMedium]];
    [titleLabel setBezeled:NO];
    [titleLabel setEditable:NO];
    [titleLabel setSelectable:NO];
    [titleLabel setBackgroundColor:[NSColor clearColor]];
    [titleLabel setTextColor:[NSColor labelColor]];
    [titleLabel setAutoresizingMask:NSViewMinYMargin];
    [contentView addSubview:titleLabel];

    // Subtitle
    NSTextField* subtitleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(70, yPos, 400, 18)];
    [subtitleLabel setStringValue:@"Guard every take. Faster setup, safer record(w)ing!"];
    [subtitleLabel setFont:[NSFont systemFontOfSize:12]];
    [subtitleLabel setBezeled:NO];
    [subtitleLabel setEditable:NO];
    [subtitleLabel setSelectable:NO];
    [subtitleLabel setBackgroundColor:[NSColor clearColor]];
    [subtitleLabel setTextColor:[NSColor secondaryLabelColor]];
    [subtitleLabel setAutoresizingMask:NSViewMinYMargin];
    [contentView addSubview:subtitleLabel];

    NSBox* statusPanel = [[NSBox alloc] initWithFrame:NSMakeRect(contentWidth - 370, yPos - 42, 340, 108)];
    [statusPanel setBoxType:NSBoxCustom];
    [statusPanel setCornerRadius:10.0];
    [statusPanel setBorderWidth:1.0];
    [statusPanel setBorderColor:[NSColor colorWithWhite:0.86 alpha:1.0]];
    [statusPanel setFillColor:[NSColor colorWithWhite:0.98 alpha:1.0]];
    [statusPanel setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:statusPanel];

    statusIconView = [[NSImageView alloc] initWithFrame:NSMakeRect(contentWidth - 352, yPos + 38, 18, 18)];
    [statusIconView setImageScaling:NSImageScaleProportionallyUpOrDown];
    [statusIconView setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:statusIconView];

    statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(contentWidth - 330, yPos + 37, 290, 20)];
    [statusLabel setStringValue:@"Console: Not Connected"];
    [statusLabel setFont:[NSFont systemFontOfSize:12]];
    [statusLabel setBezeled:NO];
    [statusLabel setEditable:NO];
    [statusLabel setSelectable:NO];
    [statusLabel setBackgroundColor:[NSColor clearColor]];
    [statusLabel setAlignment:NSTextAlignmentRight];
    [statusLabel setTextColor:[NSColor labelColor]];
    [statusLabel setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:statusLabel];

    validationIconView = [[NSImageView alloc] initWithFrame:NSMakeRect(contentWidth - 352, yPos + 14, 18, 18)];
    [validationIconView setImageScaling:NSImageScaleProportionallyUpOrDown];
    [validationIconView setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:validationIconView];

    validationStatusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(contentWidth - 330, yPos + 13, 290, 20)];
    [validationStatusLabel setStringValue:@"Reaper Recorder: Not Ready"];
    [validationStatusLabel setFont:[NSFont systemFontOfSize:12]];
    [validationStatusLabel setBezeled:NO];
    [validationStatusLabel setEditable:NO];
    [validationStatusLabel setSelectable:NO];
    [validationStatusLabel setBackgroundColor:[NSColor clearColor]];
    [validationStatusLabel setAlignment:NSTextAlignmentRight];
    [validationStatusLabel setTextColor:[NSColor labelColor]];
    [validationStatusLabel setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:validationStatusLabel];

    recorderStatusIconView = [[NSImageView alloc] initWithFrame:NSMakeRect(contentWidth - 352, yPos - 10, 18, 18)];
    [recorderStatusIconView setImageScaling:NSImageScaleProportionallyUpOrDown];
    [recorderStatusIconView setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:recorderStatusIconView];

    recorderStatusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(contentWidth - 330, yPos - 11, 290, 20)];
    [recorderStatusLabel setStringValue:@"Wing Recorder: Disabled"];
    [recorderStatusLabel setFont:[NSFont systemFontOfSize:12]];
    [recorderStatusLabel setBezeled:NO];
    [recorderStatusLabel setEditable:NO];
    [recorderStatusLabel setSelectable:NO];
    [recorderStatusLabel setBackgroundColor:[NSColor clearColor]];
    [recorderStatusLabel setAlignment:NSTextAlignmentRight];
    [recorderStatusLabel setTextColor:[NSColor labelColor]];
    [recorderStatusLabel setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:recorderStatusLabel];

    midiStatusIconView = [[NSImageView alloc] initWithFrame:NSMakeRect(contentWidth - 352, yPos - 34, 18, 18)];
    [midiStatusIconView setImageScaling:NSImageScaleProportionallyUpOrDown];
    [midiStatusIconView setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:midiStatusIconView];

    midiStatusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(contentWidth - 330, yPos - 35, 290, 20)];
    [midiStatusLabel setStringValue:@"Wing control integration: Disabled"];
    [midiStatusLabel setFont:[NSFont systemFontOfSize:12]];
    [midiStatusLabel setBezeled:NO];
    [midiStatusLabel setEditable:NO];
    [midiStatusLabel setSelectable:NO];
    [midiStatusLabel setBackgroundColor:[NSColor clearColor]];
    [midiStatusLabel setAlignment:NSTextAlignmentRight];
    [midiStatusLabel setTextColor:[NSColor labelColor]];
    [midiStatusLabel setAutoresizingMask:(NSViewMinXMargin | NSViewMinYMargin)];
    [contentView addSubview:midiStatusLabel];

    [self setConnectionStatusText:@"Console: Not Connected" color:[NSColor secondaryLabelColor] connected:NO];
    [self setValidationStatusText:@"Reaper Recorder: Not Ready" color:[NSColor secondaryLabelColor]];
    [self updateMidiStatusLabel];
    [self updateRecorderStatusLabel];

    yPos -= 150;
    auto& cfg = ReaperExtension::Instance().GetConfig();
    auto& ext = ReaperExtension::Instance();

    auto addInfoLabel = ^(NSView* parent, NSRect frame, NSString* text, CGFloat fontSize, NSColor* color) {
        NSTextField* label = [[NSTextField alloc] initWithFrame:frame];
        [label setStringValue:text];
        [label setFont:[NSFont systemFontOfSize:fontSize]];
        [label setBezeled:NO];
        [label setEditable:NO];
        [label setSelectable:NO];
        [label setBackgroundColor:[NSColor clearColor]];
        [label setTextColor:color];
        [label setLineBreakMode:NSLineBreakByWordWrapping];
        [label setUsesSingleLineMode:NO];
        [parent addSubview:label];
    };

    auto addIntroCallout = ^(NSView* parent, NSRect frame, NSString* text) {
        NSView* box = [[NSView alloc] initWithFrame:frame];
        [box setWantsLayer:YES];
        [[box layer] setCornerRadius:8.0];
        [[box layer] setBorderWidth:1.0];
        [[box layer] setBorderColor:[[NSColor colorWithWhite:0.86 alpha:1.0] CGColor]];
        [[box layer] setBackgroundColor:[[NSColor colorWithWhite:0.965 alpha:1.0] CGColor]];
        [parent addSubview:box];

        const CGFloat labelWidth = NSWidth(frame) - 28.0;
        const CGFloat maxLabelHeight = NSHeight(frame) - 12.0;
        NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(14.0, 0.0, labelWidth, maxLabelHeight)];
        [label setStringValue:text];
        [label setFont:[NSFont systemFontOfSize:12]];
        [label setBezeled:NO];
        [label setEditable:NO];
        [label setSelectable:NO];
        [label setBackgroundColor:[NSColor clearColor]];
        [label setTextColor:[NSColor secondaryLabelColor]];
        [label setLineBreakMode:NSLineBreakByWordWrapping];
        [label setUsesSingleLineMode:NO];
        NSSize labelSize = [[label cell] cellSizeForBounds:NSMakeRect(0.0, 0.0, labelWidth, maxLabelHeight)];
        CGFloat centeredY = floor((NSHeight(frame) - labelSize.height) * 0.5);
        centeredY = std::max<CGFloat>(6.0, centeredY);
        [label setFrame:NSMakeRect(14.0, centeredY, labelWidth, std::min(maxLabelHeight, labelSize.height))];
        [box addSubview:label];
    };

    settingsTabView = [[NSTabView alloc] initWithFrame:NSMakeRect(20, 20, contentWidth - 40, 620)];
    [settingsTabView setTabViewType:NSTopTabsBezelBorder];
    [settingsTabView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [settingsTabView setDelegate:self];
    [contentView addSubview:settingsTabView];

    auto makeScrollableTab = ^NSScrollView* (WingConnectorFlippedView** documentViewOut, CGFloat documentHeight) {
        const CGFloat tabWidth = NSWidth([settingsTabView bounds]);
        NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, tabWidth - 30, 580)];
        [scrollView setHasVerticalScroller:YES];
        [scrollView setHasHorizontalScroller:NO];
        [scrollView setBorderType:NSNoBorder];
        [scrollView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

        WingConnectorFlippedView* documentView = [[WingConnectorFlippedView alloc] initWithFrame:NSMakeRect(0, 0, tabWidth - 40, documentHeight)];
        [documentView setAutoresizingMask:NSViewWidthSizable];
        [scrollView setDocumentView:documentView];

        if (documentViewOut) {
            *documentViewOut = documentView;
        }
        return scrollView;
    };

    WingConnectorFlippedView* setupTabView = nil;
    WingConnectorFlippedView* automationTabView = nil;
    WingConnectorFlippedView* wingTabView = nil;
    WingConnectorFlippedView* advancedTabView = nil;
    WingConnectorFlippedView* bridgeTabView = nil;
    const CGFloat setupTabHeight = 760.0;
    const CGFloat automationTabHeight = 1120.0;
    const CGFloat wingTabHeight = 760.0;
    const CGFloat advancedTabHeight = 760.0;
    const CGFloat bridgeTabHeight = 980.0;
    const CGFloat tabTopInset = 26.0;
    const CGFloat introLabelHeight = 56.0;
    NSScrollView* setupTabScrollView = makeScrollableTab(&setupTabView, setupTabHeight);
    NSScrollView* automationTabScrollView = makeScrollableTab(&automationTabView, automationTabHeight);
    NSScrollView* wingTabScrollView = makeScrollableTab(&wingTabView, wingTabHeight);
    NSScrollView* advancedTabScrollView = makeScrollableTab(&advancedTabView, advancedTabHeight);
    NSScrollView* bridgeTabScrollView = makeScrollableTab(&bridgeTabView, bridgeTabHeight);

    consoleTabItem = [[NSTabViewItem alloc] initWithIdentifier:@"console"];
    [consoleTabItem setLabel:@"Console"];
    [consoleTabItem setView:setupTabScrollView];
    [settingsTabView addTabViewItem:consoleTabItem];

    reaperTabItem = [[NSTabViewItem alloc] initWithIdentifier:@"reaper"];
    [reaperTabItem setLabel:@"Reaper"];
    [reaperTabItem setView:automationTabScrollView];
    [settingsTabView addTabViewItem:reaperTabItem];

    wingTabItemRef = [[NSTabViewItem alloc] initWithIdentifier:@"wing"];
    [wingTabItemRef setLabel:@"Wing"];
    [wingTabItemRef setView:wingTabScrollView];
    [settingsTabView addTabViewItem:wingTabItemRef];

    controlIntegrationTabItem = [[NSTabViewItem alloc] initWithIdentifier:@"control-integration"];
    [controlIntegrationTabItem setLabel:@"Control Integration"];
    [controlIntegrationTabItem setView:advancedTabScrollView];
    [settingsTabView addTabViewItem:controlIntegrationTabItem];

    if (kShowBridgeTabInMainUI) {
        bridgeTabItemRef = [[NSTabViewItem alloc] initWithIdentifier:@"bridge"];
        [bridgeTabItemRef setLabel:@"Bridge"];
        [bridgeTabItemRef setView:bridgeTabScrollView];
        [settingsTabView addTabViewItem:bridgeTabItemRef];
    }

    const CGFloat labelX = 20;
    const CGFloat controlX = 220;
    const CGFloat labelW = 180;
    const CGFloat setupWidth = 760;
    const CGFloat statusChipX = 620;

    auto makeTabStatusLabel = ^NSTextField*(NSView* parent, CGFloat y) {
        NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(statusChipX, y, 140, 20)];
        [label setStringValue:@"Inactive"];
        [label setFont:[NSFont systemFontOfSize:12.5 weight:NSFontWeightBold]];
        [label setBezeled:NO];
        [label setEditable:NO];
        [label setSelectable:NO];
        [label setBackgroundColor:[NSColor clearColor]];
        [label setAlignment:NSTextAlignmentRight];
        [label setTextColor:[NSColor secondaryLabelColor]];
        [parent addSubview:label];
        return label;
    };

    CGFloat setupY = setupTabHeight - tabTopInset - introLabelHeight;
    addIntroCallout(setupTabView, NSMakeRect(20, setupY, 760, introLabelHeight),
                    @"Connect to a Wing, choose where recording channels go, and get live or soundcheck playback ready without cable gymnastics.");
    setupY -= (introLabelHeight + 10.0);

    NSTextField* setupConnectionHeader = [[NSTextField alloc] initWithFrame:NSMakeRect(20, setupY, 300, 20)];
    [setupConnectionHeader setStringValue:@"🌐 Connection"];
    [setupConnectionHeader setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [setupConnectionHeader setBezeled:NO];
    [setupConnectionHeader setEditable:NO];
    [setupConnectionHeader setSelectable:NO];
    [setupConnectionHeader setBackgroundColor:[NSColor clearColor]];
    [setupConnectionHeader setTextColor:[NSColor labelColor]];
    [setupTabView addSubview:setupConnectionHeader];
    consoleTabStatusLabel = makeTabStatusLabel(setupTabView, setupY);
    setupY -= 32;
    addInfoLabel(setupTabView, NSMakeRect(20, setupY, 760, 30),
                 @"Use Scan to find a console on the network, or enter its IP manually if discovery comes back empty-handed.",
                 11, [NSColor secondaryLabelColor]);
    setupY -= 44;

    NSTextField* consoleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, setupY + 4, 110, 20)];
    [consoleLabel setStringValue:@"Wing Console:"];
    [consoleLabel setFont:[NSFont systemFontOfSize:12]];
    [consoleLabel setBezeled:NO];
    [consoleLabel setEditable:NO];
    [consoleLabel setSelectable:NO];
    [consoleLabel setBackgroundColor:[NSColor clearColor]];
    [consoleLabel setTextColor:[NSColor secondaryLabelColor]];
    [setupTabView addSubview:consoleLabel];
    wingDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(130, setupY, 500, 28) pullsDown:NO];
    [wingDropdown addItemWithTitle:@"Scanning..."];
    [[wingDropdown itemAtIndex:0] setEnabled:NO];
    [wingDropdown setEnabled:NO];
    [wingDropdown setTarget:self];
    [wingDropdown setAction:@selector(onWingDropdownChanged:)];
    [setupTabView addSubview:wingDropdown];
    scanButton = [[NSButton alloc] initWithFrame:NSMakeRect(650, setupY + 2, 120, 28)];
    [scanButton setTitle:@"Scan"];
    [scanButton setBezelStyle:NSBezelStyleRounded];
    [scanButton setTarget:self];
    [scanButton setAction:@selector(onScanClicked:)];
    [setupTabView addSubview:scanButton];
    setupY -= 30;
    addInfoLabel(setupTabView, NSMakeRect(130, setupY, 560, 16),
                 @"Pick a discovered Wing to fill the connection details automatically.",
                 10, [NSColor tertiaryLabelColor]);
    setupY -= 28;

    NSTextField* manualIPLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, setupY + 4, 110, 20)];
    [manualIPLabel setStringValue:@"Manual IP:"];
    [manualIPLabel setFont:[NSFont systemFontOfSize:12]];
    [manualIPLabel setBezeled:NO];
    [manualIPLabel setEditable:NO];
    [manualIPLabel setSelectable:NO];
    [manualIPLabel setBackgroundColor:[NSColor clearColor]];
    [manualIPLabel setTextColor:[NSColor secondaryLabelColor]];
    [setupTabView addSubview:manualIPLabel];
    manualIPField = [[NSTextField alloc] initWithFrame:NSMakeRect(130, setupY, 260, 24)];
    if (!cfg.wing_ip.empty()) {
        [manualIPField setStringValue:[NSString stringWithUTF8String:cfg.wing_ip.c_str()]];
    }
    [manualIPField setPlaceholderString:@"Use this if scan does not find your Wing"];
    [manualIPField setFont:[NSFont systemFontOfSize:11]];
    [manualIPField setTarget:self];
    [manualIPField setAction:@selector(onManualIPChanged:)];
    [setupTabView addSubview:manualIPField];
    setupY -= 28;
    addInfoLabel(setupTabView, NSMakeRect(130, setupY, 560, 28),
                 @"If you already know the console IP, skip the scan and connect directly.",
                 10, [NSColor tertiaryLabelColor]);
    setupY -= 42;

    connectButton = [[NSButton alloc] initWithFrame:NSMakeRect(650, setupY - 2, 120, 28)];
    [connectButton setBezelStyle:NSBezelStyleRounded];
    [connectButton setTitle:@"Connect"];
    [connectButton setTarget:self];
    [connectButton setAction:@selector(onConnectClicked:)];
    [setupTabView addSubview:connectButton];
    addInfoLabel(setupTabView, NSMakeRect(20, setupY, 600, 26),
                 @"Console connection and recording-readiness status stay pinned in the header above, visible from every tab.",
                 11, [NSColor secondaryLabelColor]);
    setupY -= 46;

    NSBox* setupSeparator = [[NSBox alloc] initWithFrame:NSMakeRect(20, setupY, setupWidth, 1)];
    [setupSeparator setBoxType:NSBoxSeparator];
    [setupTabView addSubview:setupSeparator];

    CGFloat reaperY = automationTabHeight - tabTopInset - introLabelHeight;
    addIntroCallout(automationTabView, NSMakeRect(20, reaperY, 760, introLabelHeight),
                    @"Prepare REAPER for live recording and virtual soundcheck here: choose USB or CARD routing, stage and apply the source layout, switch prepared channels between live inputs and playback, and use Auto Trigger when you want signal-driven starts.");
    reaperY -= (introLabelHeight + 10.0);

    NSTextField* routingHeader = [[NSTextField alloc] initWithFrame:NSMakeRect(20, reaperY, 300, 20)];
    [routingHeader setStringValue:@"🎚 Recording and Soundcheck"];
    [routingHeader setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [routingHeader setBezeled:NO];
    [routingHeader setEditable:NO];
    [routingHeader setSelectable:NO];
    [routingHeader setBackgroundColor:[NSColor clearColor]];
    [routingHeader setTextColor:[NSColor labelColor]];
    [automationTabView addSubview:routingHeader];
    reaperTabStatusLabel = makeTabStatusLabel(automationTabView, reaperY);
    reaperY -= 32;
    addInfoLabel(automationTabView, NSMakeRect(20, reaperY, 760, 30),
                 @"Setup Live Recording can replace the current REAPER track list, rebuild the selected recording paths, and keep soundcheck switching limited to prepared channels.",
                 11, [NSColor secondaryLabelColor]);
    reaperY -= 44;

    NSTextField* outputModeLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, reaperY + 8, labelW, 20)];
    [outputModeLabel setStringValue:@"Recording I/O Mode:"];
    [outputModeLabel setFont:[NSFont systemFontOfSize:11]];
    [outputModeLabel setBezeled:NO];
    [outputModeLabel setEditable:NO];
    [outputModeLabel setSelectable:NO];
    [outputModeLabel setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:outputModeLabel];
    outputModeControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, reaperY + 4, 120, 24)];
    [outputModeControl setSegmentCount:2];
    [outputModeControl setLabel:@"USB" forSegment:0];
    [outputModeControl setLabel:@"CARD" forSegment:1];
    [outputModeControl setSelectedSegment:(cfg.soundcheck_output_mode == "CARD") ? 1 : 0];
    [outputModeControl setSegmentStyle:NSSegmentStyleRounded];
    [outputModeControl setTarget:self];
    [outputModeControl setAction:@selector(onOutputModeChanged:)];
    [automationTabView addSubview:outputModeControl];
    reaperY -= 28;
    addInfoLabel(automationTabView, NSMakeRect(controlX, reaperY, 520, 28),
                 @"Choose where the Wing sends the recording channels. USB is the usual direct-to-computer path; CARD uses the Wing audio card route.",
                 10, [NSColor tertiaryLabelColor]);
    reaperY -= 40;

    pendingSetupSummaryLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, reaperY, 540, 36)];
    [pendingSetupSummaryLabel setStringValue:@"No pending setup changes. Choose sources for a new setup, or change recording mode to stage a rebuild of the current managed setup."];
    [pendingSetupSummaryLabel setFont:[NSFont systemFontOfSize:11]];
    [pendingSetupSummaryLabel setBezeled:NO];
    [pendingSetupSummaryLabel setEditable:NO];
    [pendingSetupSummaryLabel setSelectable:NO];
    [pendingSetupSummaryLabel setBackgroundColor:[NSColor clearColor]];
    [pendingSetupSummaryLabel setTextColor:[NSColor secondaryLabelColor]];
    [pendingSetupSummaryLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [pendingSetupSummaryLabel setUsesSingleLineMode:NO];
    [automationTabView addSubview:pendingSetupSummaryLabel];
    reaperY -= 46;

    setupReadinessDetailLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, reaperY, 540, 56)];
    [setupReadinessDetailLabel setStringValue:@"Connect to a Wing to validate the current managed setup, then rebuild it only when routing or recording mode needs to change."];
    [setupReadinessDetailLabel setFont:[NSFont systemFontOfSize:10.5]];
    [setupReadinessDetailLabel setBezeled:NO];
    [setupReadinessDetailLabel setEditable:NO];
    [setupReadinessDetailLabel setSelectable:NO];
    [setupReadinessDetailLabel setBackgroundColor:[NSColor clearColor]];
    [setupReadinessDetailLabel setTextColor:[NSColor secondaryLabelColor]];
    [setupReadinessDetailLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [setupReadinessDetailLabel setUsesSingleLineMode:NO];
    [automationTabView addSubview:setupReadinessDetailLabel];
    reaperY -= 60;

    setupSoundcheckButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, reaperY, 160, 32)];
    [setupSoundcheckButton setBezelStyle:NSBezelStyleRounded];
    [setupSoundcheckButton setTitle:@"Choose Sources…"];
    [setupSoundcheckButton setTarget:self];
    [setupSoundcheckButton setAction:@selector(onSetupSoundcheckClicked:)];
    [automationTabView addSubview:setupSoundcheckButton];

    applyPendingSetupButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX + 180, reaperY, 180, 32)];
    [applyPendingSetupButton setBezelStyle:NSBezelStyleRounded];
    [applyPendingSetupButton setTitle:@"Apply Setup"];
    [applyPendingSetupButton setTarget:self];
    [applyPendingSetupButton setAction:@selector(onApplyPendingSetupClicked:)];
    [applyPendingSetupButton setEnabled:NO];
    [automationTabView addSubview:applyPendingSetupButton];

    discardPendingSetupButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX + 370, reaperY, 140, 32)];
    [discardPendingSetupButton setBezelStyle:NSBezelStyleRounded];
    [discardPendingSetupButton setTitle:@"Discard"];
    [discardPendingSetupButton setTarget:self];
    [discardPendingSetupButton setAction:@selector(onDiscardPendingSetupClicked:)];
    [discardPendingSetupButton setEnabled:NO];
    [automationTabView addSubview:discardPendingSetupButton];

    reaperY -= 42;

    toggleSoundcheckButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, reaperY, 220, 32)];
    [toggleSoundcheckButton setBezelStyle:NSBezelStyleRounded];
    [toggleSoundcheckButton setTitle:@"🎙️ Live Mode"];
    [toggleSoundcheckButton setTarget:self];
    [toggleSoundcheckButton setAction:@selector(onToggleSoundcheckClicked:)];
    [toggleSoundcheckButton setEnabled:NO];
    [automationTabView addSubview:toggleSoundcheckButton];
    NSTextField* toggleDesc = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, reaperY + 8, labelW, 20)];
    [toggleDesc setStringValue:@"Switch live/soundcheck:"];
    [toggleDesc setFont:[NSFont systemFontOfSize:11]];
    [toggleDesc setBezeled:NO];
    [toggleDesc setEditable:NO];
    [toggleDesc setSelectable:NO];
    [toggleDesc setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:toggleDesc];
    reaperY -= 42;
    addInfoLabel(automationTabView, NSMakeRect(controlX, reaperY, 540, 28),
                 @"After setup is validated, this flips prepared channels between live inputs and REAPER playback. One button, less panic.",
                 10, [NSColor tertiaryLabelColor]);
    reaperY -= 42;

    NSBox* reaperSeparator = [[NSBox alloc] initWithFrame:NSMakeRect(20, reaperY, setupWidth, 1)];
    [reaperSeparator setBoxType:NSBoxSeparator];
    [automationTabView addSubview:reaperSeparator];
    reaperY -= 28;

    CGFloat autoY = reaperY;

    NSTextField* triggerHeader = [[NSTextField alloc] initWithFrame:NSMakeRect(20, autoY, 240, 20)];
    [triggerHeader setStringValue:@"⚡ Auto Trigger"];
    [triggerHeader setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [triggerHeader setBezeled:NO];
    [triggerHeader setEditable:NO];
    [triggerHeader setSelectable:NO];
    [triggerHeader setBackgroundColor:[NSColor clearColor]];
    [triggerHeader setTextColor:[NSColor labelColor]];
    [automationTabView addSubview:triggerHeader];
    autoY -= 32;
    addInfoLabel(automationTabView, NSMakeRect(20, autoY, 590, 30),
                 @"Trigger controls wake up after live setup validates, because they depend on the prepared recording path.",
                 11, [NSColor secondaryLabelColor]);
    autoY -= 44;

    NSTextField* autoEnableLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, autoY + 8, labelW, 20)];
    [autoEnableLabel setStringValue:@"Enable Trigger:"];
    [autoEnableLabel setFont:[NSFont systemFontOfSize:11]];
    [autoEnableLabel setBezeled:NO];
    [autoEnableLabel setEditable:NO];
    [autoEnableLabel setSelectable:NO];
    [autoEnableLabel setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:autoEnableLabel];
    autoRecordEnableControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, autoY + 4, 120, 24)];
    [autoRecordEnableControl setSegmentCount:2];
    [autoRecordEnableControl setLabel:@"OFF" forSegment:0];
    [autoRecordEnableControl setLabel:@"ON" forSegment:1];
    [autoRecordEnableControl setSelectedSegment:cfg.auto_record_enabled ? 1 : 0];
    [autoRecordEnableControl setTarget:self];
    [autoRecordEnableControl setAction:@selector(onAutoRecordSettingsChanged:)];
    [automationTabView addSubview:autoRecordEnableControl];
    autoY -= 28;
    addInfoLabel(automationTabView, NSMakeRect(controlX, autoY, 350, 16),
                 @"Turns signal-based trigger monitoring on or off.",
                 10, [NSColor tertiaryLabelColor]);
    autoY -= 32;

    NSTextField* trackLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, autoY + 8, labelW, 20)];
    [trackLabel setStringValue:@"Monitor Track:"];
    [trackLabel setFont:[NSFont systemFontOfSize:11]];
    [trackLabel setBezeled:NO];
    [trackLabel setEditable:NO];
    [trackLabel setSelectable:NO];
    [trackLabel setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:trackLabel];
    monitorTrackDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX, autoY + 4, 220, 24) pullsDown:NO];
    [monitorTrackDropdown setTarget:self];
    [monitorTrackDropdown setAction:@selector(onMonitorTrackChanged:)];
    [automationTabView addSubview:monitorTrackDropdown];
    [self refreshMonitorTrackDropdown];
    autoY -= 28;
    addInfoLabel(automationTabView, NSMakeRect(controlX, autoY, 360, 16),
                 @"Choose which REAPER track is watched for trigger level. Auto watches all armed and monitored tracks.",
                 10, [NSColor tertiaryLabelColor]);
    autoY -= 32;

    automationDetailLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, autoY, 540, 38)];
    [automationDetailLabel setStringValue:@"Auto Trigger will become available once recording setup is ready."];
    [automationDetailLabel setFont:[NSFont systemFontOfSize:10.5]];
    [automationDetailLabel setBezeled:NO];
    [automationDetailLabel setEditable:NO];
    [automationDetailLabel setSelectable:NO];
    [automationDetailLabel setBackgroundColor:[NSColor clearColor]];
    [automationDetailLabel setTextColor:[NSColor secondaryLabelColor]];
    [automationDetailLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [automationDetailLabel setUsesSingleLineMode:NO];
    [automationTabView addSubview:automationDetailLabel];
    autoY -= 42;

    NSTextField* modeLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, autoY + 8, labelW, 20)];
    [modeLabel setStringValue:@"Trigger Mode:"];
    [modeLabel setFont:[NSFont systemFontOfSize:11]];
    [modeLabel setBezeled:NO];
    [modeLabel setEditable:NO];
    [modeLabel setSelectable:NO];
    [modeLabel setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:modeLabel];
    autoRecordModeControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, autoY + 4, 220, 24)];
    [autoRecordModeControl setSegmentCount:2];
    [autoRecordModeControl setLabel:@"WARNING" forSegment:0];
    [autoRecordModeControl setLabel:@"RECORD" forSegment:1];
    [autoRecordModeControl setSelectedSegment:cfg.auto_record_warning_only ? 0 : 1];
    [autoRecordModeControl setTarget:self];
    [autoRecordModeControl setAction:@selector(onAutoRecordSettingsChanged:)];
    [automationTabView addSubview:autoRecordModeControl];
    autoY -= 28;
    addInfoLabel(automationTabView, NSMakeRect(controlX, autoY, 360, 28),
                 @"Warning flashes Wing controls when the trigger fires. Record also starts and stops recording automatically when the moment arrives.",
                 10, [NSColor tertiaryLabelColor]);
    autoY -= 40;

    NSTextField* thresholdLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, autoY + 8, labelW, 20)];
    [thresholdLabel setStringValue:@"Trigger Threshold:"];
    [thresholdLabel setFont:[NSFont systemFontOfSize:11]];
    [thresholdLabel setBezeled:NO];
    [thresholdLabel setEditable:NO];
    [thresholdLabel setSelectable:NO];
    [thresholdLabel setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:thresholdLabel];
    thresholdField = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, autoY + 4, 80, 24)];
    [thresholdField setStringValue:[NSString stringWithFormat:@"%.1f", cfg.auto_record_threshold_db]];
    [thresholdField setTarget:self];
    [thresholdField setAction:@selector(onAutoRecordSettingsChanged:)];
    [automationTabView addSubview:thresholdField];
    addInfoLabel(automationTabView, NSMakeRect(controlX + 90, autoY + 8, 50, 16),
                 @"dBFS", 10, [NSColor tertiaryLabelColor]);
    autoY -= 28;
    addInfoLabel(automationTabView, NSMakeRect(controlX, autoY, 360, 28),
                 @"Recording starts when the monitored signal rises above this level. Raise it to ignore low background noise and stage rustling.",
                 10, [NSColor tertiaryLabelColor]);
    autoY -= 40;

    NSTextField* holdLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, autoY + 8, labelW, 20)];
    [holdLabel setStringValue:@"Hold Time:"];
    [holdLabel setFont:[NSFont systemFontOfSize:11]];
    [holdLabel setBezeled:NO];
    [holdLabel setEditable:NO];
    [holdLabel setSelectable:NO];
    [holdLabel setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:holdLabel];
    holdField = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, autoY + 4, 80, 24)];
    [holdField setStringValue:[NSString stringWithFormat:@"%.1f", cfg.auto_record_hold_ms / 1000.0]];
    [holdField setTarget:self];
    [holdField setAction:@selector(onAutoRecordSettingsChanged:)];
    [automationTabView addSubview:holdField];
    addInfoLabel(automationTabView, NSMakeRect(controlX + 90, autoY + 8, 50, 16),
                 @"s", 10, [NSColor tertiaryLabelColor]);
    autoY -= 28;
    addInfoLabel(automationTabView, NSMakeRect(controlX, autoY, 360, 28),
                 @"Controls how long the signal may stay quiet before the trigger stops or resets. Longer times avoid nervous stop/start behavior.",
                 10, [NSColor tertiaryLabelColor]);
    autoY -= 44;

    meterPreviewLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, autoY + 8, 320, 20)];
    [meterPreviewLabel setStringValue:@"Trigger level: -- dBFS"];
    [meterPreviewLabel setFont:[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]];
    [meterPreviewLabel setBezeled:NO];
    [meterPreviewLabel setEditable:NO];
    [meterPreviewLabel setSelectable:NO];
    [meterPreviewLabel setBackgroundColor:[NSColor clearColor]];
    [automationTabView addSubview:meterPreviewLabel];
    addInfoLabel(automationTabView, NSMakeRect(350, autoY + 8, 240, 20),
                 @"Live meter readout for the current trigger source.",
                 10, [NSColor tertiaryLabelColor]);
    autoY -= 34;

    applyAutomationButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, autoY, 220, 32)];
    [applyAutomationButton setBezelStyle:NSBezelStyleRounded];
    [applyAutomationButton setTitle:@"Apply Auto Trigger Settings"];
    [applyAutomationButton setTarget:self];
    [applyAutomationButton setAction:@selector(onApplyAutomationSettingsClicked:)];
    [applyAutomationButton setEnabled:NO];
    [automationTabView addSubview:applyAutomationButton];
    addInfoLabel(automationTabView, NSMakeRect(controlX + 230, autoY + 8, 230, 20),
                 @"Pending changes stay parked until you apply them.",
                 10, [NSColor tertiaryLabelColor]);
    autoY -= 28;
    addInfoLabel(automationTabView, NSMakeRect(controlX, autoY, 360, 28),
                 @"Changing trigger settings pauses automation immediately. Recording cannot auto-start again until these values are applied.",
                 10, [NSColor secondaryLabelColor]);
    autoY -= 40;

    CGFloat wingY = wingTabHeight - tabTopInset - introLabelHeight;
    addIntroCallout(wingTabView, NSMakeRect(20, wingY, 760, introLabelHeight),
                    @"Manage the Wing-side recorder behavior here: target selection, source feed, and whether the recorder follows REAPER-triggered automation.");
    wingY -= (introLabelHeight + 10.0);

    NSTextField* recorderHeader = [[NSTextField alloc] initWithFrame:NSMakeRect(20, wingY, 240, 20)];
    [recorderHeader setStringValue:@"📼 Recorder Coordination"];
    [recorderHeader setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [recorderHeader setBezeled:NO];
    [recorderHeader setEditable:NO];
    [recorderHeader setSelectable:NO];
    [recorderHeader setBackgroundColor:[NSColor clearColor]];
    [recorderHeader setTextColor:[NSColor labelColor]];
    [wingTabView addSubview:recorderHeader];
    wingTabStatusLabel = makeTabStatusLabel(wingTabView, wingY);
    wingY -= 32;
    addInfoLabel(wingTabView, NSMakeRect(20, wingY, 590, 30),
                 @"These options decide which Wing recorder is prepared and whether it follows the auto-trigger or REAPER transport.",
                 11, [NSColor secondaryLabelColor]);
    wingY -= 44;

    NSTextField* recorderEnableLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, wingY + 8, 180, 20)];
    [recorderEnableLabel setStringValue:@"Recorder Control:"];
    [recorderEnableLabel setFont:[NSFont systemFontOfSize:11]];
    [recorderEnableLabel setBezeled:NO];
    [recorderEnableLabel setEditable:NO];
    [recorderEnableLabel setSelectable:NO];
    [recorderEnableLabel setBackgroundColor:[NSColor clearColor]];
    [wingTabView addSubview:recorderEnableLabel];
    recorderEnableControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, wingY + 4, 120, 24)];
    [recorderEnableControl setSegmentCount:2];
    [recorderEnableControl setLabel:@"OFF" forSegment:0];
    [recorderEnableControl setLabel:@"ON" forSegment:1];
    [recorderEnableControl setSelectedSegment:cfg.recorder_coordination_enabled ? 1 : 0];
    [recorderEnableControl setTarget:self];
    [recorderEnableControl setAction:@selector(onRecorderSettingsChanged:)];
    [wingTabView addSubview:recorderEnableControl];
    wingY -= 28;
    addInfoLabel(wingTabView, NSMakeRect(controlX, wingY, 360, 28),
                 @"Turns WING recorder coordination on or off for this workflow.",
                 10, [NSColor tertiaryLabelColor]);
    wingY -= 36;

    NSTextField* recorderTargetLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, wingY + 8, 180, 20)];
    [recorderTargetLabel setStringValue:@"Recorder Target:"];
    [recorderTargetLabel setFont:[NSFont systemFontOfSize:11]];
    [recorderTargetLabel setBezeled:NO];
    [recorderTargetLabel setEditable:NO];
    [recorderTargetLabel setSelectable:NO];
    [recorderTargetLabel setBackgroundColor:[NSColor clearColor]];
    [wingTabView addSubview:recorderTargetLabel];
    recorderTargetControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, wingY + 4, 300, 24)];
    [recorderTargetControl setSegmentCount:2];
    [recorderTargetControl setLabel:@"SD (WING-LIVE)" forSegment:0];
    [recorderTargetControl setLabel:@"USB Recorder" forSegment:1];
    [recorderTargetControl setSelectedSegment:(cfg.recorder_target == "USBREC") ? 1 : 0];
    [recorderTargetControl setTarget:self];
    [recorderTargetControl setAction:@selector(onRecorderSettingsChanged:)];
    [wingTabView addSubview:recorderTargetControl];
    wingY -= 28;
    addInfoLabel(wingTabView, NSMakeRect(controlX, wingY, 360, 28),
                 @"Choose which recorder gets the red-light treatment when recorder automation is enabled.",
                 10, [NSColor tertiaryLabelColor]);
    wingY -= 36;

    NSTextField* sdSourceLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, wingY + 8, 180, 20)];
    [sdSourceLabel setStringValue:@"Recorder Source Pair:"];
    [sdSourceLabel setFont:[NSFont systemFontOfSize:11]];
    [sdSourceLabel setBezeled:NO];
    [sdSourceLabel setEditable:NO];
    [sdSourceLabel setSelectable:NO];
    [sdSourceLabel setBackgroundColor:[NSColor clearColor]];
    [wingTabView addSubview:sdSourceLabel];
    sdSourceDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX, wingY + 4, 220, 24) pullsDown:NO];
    for (int i = 1; i <= 7; i += 2) {
        NSString* title = [NSString stringWithFormat:@"MAIN %d/%d", i, i + 1];
        [sdSourceDropdown addItemWithTitle:title];
        [[sdSourceDropdown itemAtIndex:((i - 1) / 2)] setTag:i];
    }
    int selectedLeft = std::max(1, cfg.sd_lr_left_input);
    int selectedIndex = std::max(0, std::min(3, (selectedLeft - 1) / 2));
    [sdSourceDropdown selectItemAtIndex:selectedIndex];
    [sdSourceDropdown setTarget:self];
    [sdSourceDropdown setAction:@selector(onRecorderSettingsChanged:)];
    [wingTabView addSubview:sdSourceDropdown];
    wingY -= 28;
    addInfoLabel(wingTabView, NSMakeRect(controlX, wingY, 360, 28),
                 @"Select which MAIN stereo pair is sent to the chosen Wing recorder when settings are applied.",
                 10, [NSColor tertiaryLabelColor]);
    wingY -= 36;

    NSTextField* recorderFollowLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, wingY + 8, 180, 20)];
    [recorderFollowLabel setStringValue:@"Follow Auto-Trigger:"];
    [recorderFollowLabel setFont:[NSFont systemFontOfSize:11]];
    [recorderFollowLabel setBezeled:NO];
    [recorderFollowLabel setEditable:NO];
    [recorderFollowLabel setSelectable:NO];
    [recorderFollowLabel setBackgroundColor:[NSColor clearColor]];
    [wingTabView addSubview:recorderFollowLabel];
    recorderFollowControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, wingY + 4, 120, 24)];
    [recorderFollowControl setSegmentCount:2];
    [recorderFollowControl setLabel:@"OFF" forSegment:0];
    [recorderFollowControl setLabel:@"ON" forSegment:1];
    [recorderFollowControl setSelectedSegment:cfg.sd_auto_record_with_reaper ? 1 : 0];
    [recorderFollowControl setTarget:self];
    [recorderFollowControl setAction:@selector(onRecorderSettingsChanged:)];
    [wingTabView addSubview:recorderFollowControl];
    wingY -= 28;
    addInfoLabel(wingTabView, NSMakeRect(controlX, wingY, 540, 28),
                 @"When enabled, the chosen Wing recorder follows plugin-controlled auto-trigger recordings.",
                 10, [NSColor tertiaryLabelColor]);
    wingY -= 30;

    recorderFollowHintLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, wingY, 540, 34)];
    [recorderFollowHintLabel setStringValue:@""];
    [recorderFollowHintLabel setFont:[NSFont systemFontOfSize:10.5]];
    [recorderFollowHintLabel setBezeled:NO];
    [recorderFollowHintLabel setEditable:NO];
    [recorderFollowHintLabel setSelectable:NO];
    [recorderFollowHintLabel setBackgroundColor:[NSColor clearColor]];
    [recorderFollowHintLabel setTextColor:[NSColor secondaryLabelColor]];
    [recorderFollowHintLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [recorderFollowHintLabel setUsesSingleLineMode:NO];
    [wingTabView addSubview:recorderFollowHintLabel];
    wingY -= 40;

    recorderDetailLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, wingY, 540, 38)];
    [recorderDetailLabel setStringValue:@"Recorder coordination is using the currently applied settings."];
    [recorderDetailLabel setFont:[NSFont systemFontOfSize:10.5]];
    [recorderDetailLabel setBezeled:NO];
    [recorderDetailLabel setEditable:NO];
    [recorderDetailLabel setSelectable:NO];
    [recorderDetailLabel setBackgroundColor:[NSColor clearColor]];
    [recorderDetailLabel setTextColor:[NSColor secondaryLabelColor]];
    [recorderDetailLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [recorderDetailLabel setUsesSingleLineMode:NO];
    [wingTabView addSubview:recorderDetailLabel];
    wingY -= 42;

    applyRecorderButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, wingY, 220, 32)];
    [applyRecorderButton setBezelStyle:NSBezelStyleRounded];
    [applyRecorderButton setTitle:@"Apply Recorder Settings"];
    [applyRecorderButton setTarget:self];
    [applyRecorderButton setAction:@selector(onApplyRecorderSettingsClicked:)];
    [applyRecorderButton setEnabled:NO];
    [wingTabView addSubview:applyRecorderButton];
    addInfoLabel(wingTabView, NSMakeRect(controlX + 230, wingY + 8, 260, 20),
                 @"Recorder changes stay staged until you apply them.",
                 10, [NSColor tertiaryLabelColor]);

    CGFloat advY = advancedTabHeight - tabTopInset - introLabelHeight;
    addIntroCallout(advancedTabView, NSMakeRect(20, advY, 760, introLabelHeight),
                    @"Map Wing controls into REAPER here, and keep diagnostics close by when you need to verify what the plugin is doing.");
    advY -= (introLabelHeight + 10.0);

    CGFloat bridgeY = bridgeTabHeight - tabTopInset - introLabelHeight;
    addIntroCallout(bridgeTabView, NSMakeRect(20, bridgeY, 760, introLabelHeight),
                    @"Bridge the selected WING strip to an external MIDI target. This runs separately from live recording and soundcheck setup, but uses the same shared Wing connection.");
    bridgeY -= (introLabelHeight + 10.0);

    NSTextField* bridgeHeader = [[NSTextField alloc] initWithFrame:NSMakeRect(20, bridgeY, 300, 20)];
    [bridgeHeader setStringValue:@"🔀 Selected Channel Bridge"];
    [bridgeHeader setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [bridgeHeader setBezeled:NO];
    [bridgeHeader setEditable:NO];
    [bridgeHeader setSelectable:NO];
    [bridgeHeader setBackgroundColor:[NSColor clearColor]];
    [bridgeHeader setTextColor:[NSColor labelColor]];
    [bridgeTabView addSubview:bridgeHeader];
    bridgeTabStatusLabel = makeTabStatusLabel(bridgeTabView, bridgeY);
    bridgeY -= 32;

    bridgeEnableCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(20, bridgeY + 4, 420, 20)];
    [bridgeEnableCheckbox setButtonType:NSButtonTypeSwitch];
    [bridgeEnableCheckbox setTitle:@"Follow the selected WING strip and send MIDI for mapped entries"];
    [bridgeEnableCheckbox setState:cfg.bridge_enabled ? NSControlStateValueOn : NSControlStateValueOff];
    [bridgeEnableCheckbox setTarget:self];
    [bridgeEnableCheckbox setAction:@selector(onBridgeSettingsChanged:)];
    [bridgeTabView addSubview:bridgeEnableCheckbox];
    bridgeY -= 28;

    bridgeStatusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, bridgeY, 580, 34)];
    [bridgeStatusLabel setStringValue:@"Bridge idle"];
    [bridgeStatusLabel setFont:[NSFont systemFontOfSize:11]];
    [bridgeStatusLabel setBezeled:NO];
    [bridgeStatusLabel setEditable:NO];
    [bridgeStatusLabel setSelectable:NO];
    [bridgeStatusLabel setBackgroundColor:[NSColor clearColor]];
    [bridgeStatusLabel setTextColor:[NSColor secondaryLabelColor]];
    [bridgeStatusLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [bridgeStatusLabel setUsesSingleLineMode:NO];
    [bridgeTabView addSubview:bridgeStatusLabel];
    bridgeY -= 46;

    addInfoLabel(bridgeTabView, NSMakeRect(20, bridgeY, 590, 28),
                 @"Add explicit mappings below. When one of those WING strips is selected, the bridge sends the configured MIDI value to the selected output.",
                 10, [NSColor tertiaryLabelColor]);
    bridgeY -= 34;

    NSTextField* bridgeOutputLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, bridgeY + 8, labelW, 20)];
    [bridgeOutputLabel setStringValue:@"MIDI Output:"];
    [bridgeOutputLabel setFont:[NSFont systemFontOfSize:11]];
    [bridgeOutputLabel setBezeled:NO];
    [bridgeOutputLabel setEditable:NO];
    [bridgeOutputLabel setSelectable:NO];
    [bridgeOutputLabel setBackgroundColor:[NSColor clearColor]];
    [bridgeTabView addSubview:bridgeOutputLabel];
    bridgeMidiOutputDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX, bridgeY + 4, 260, 24) pullsDown:NO];
    [bridgeMidiOutputDropdown setTarget:self];
    [bridgeMidiOutputDropdown setAction:@selector(onBridgeSettingsChanged:)];
    [bridgeTabView addSubview:bridgeMidiOutputDropdown];
    bridgeY -= 34;

    NSTextField* bridgeTypeLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, bridgeY + 8, labelW, 20)];
    [bridgeTypeLabel setStringValue:@"MIDI Behavior:"];
    [bridgeTypeLabel setFont:[NSFont systemFontOfSize:11]];
    [bridgeTypeLabel setBezeled:NO];
    [bridgeTypeLabel setEditable:NO];
    [bridgeTypeLabel setSelectable:NO];
    [bridgeTypeLabel setBackgroundColor:[NSColor clearColor]];
    [bridgeTabView addSubview:bridgeTypeLabel];
    bridgeMessageTypeControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, bridgeY + 4, 300, 24)];
    [bridgeMessageTypeControl setSegmentCount:3];
    [bridgeMessageTypeControl setLabel:@"Note" forSegment:0];
    [bridgeMessageTypeControl setLabel:@"Note+Off" forSegment:1];
    [bridgeMessageTypeControl setLabel:@"Program" forSegment:2];
    int bridgeSegment = 1;
    if (cfg.bridge_midi_message_type == "NOTE_ON") bridgeSegment = 0;
    else if (cfg.bridge_midi_message_type == "PROGRAM") bridgeSegment = 2;
    [bridgeMessageTypeControl setSelectedSegment:bridgeSegment];
    [bridgeMessageTypeControl setTarget:self];
    [bridgeMessageTypeControl setAction:@selector(onBridgeSettingsChanged:)];
    [bridgeTabView addSubview:bridgeMessageTypeControl];
    bridgeY -= 34;

    NSTextField* bridgeChannelLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, bridgeY + 8, labelW, 20)];
    [bridgeChannelLabel setStringValue:@"MIDI Channel:"];
    [bridgeChannelLabel setFont:[NSFont systemFontOfSize:11]];
    [bridgeChannelLabel setBezeled:NO];
    [bridgeChannelLabel setEditable:NO];
    [bridgeChannelLabel setSelectable:NO];
    [bridgeChannelLabel setBackgroundColor:[NSColor clearColor]];
    [bridgeTabView addSubview:bridgeChannelLabel];
    bridgeMidiChannelDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX, bridgeY + 4, 120, 24) pullsDown:NO];
    for (int i = 1; i <= 16; ++i) {
        NSString* title = [NSString stringWithFormat:@"Ch %d", i];
        [bridgeMidiChannelDropdown addItemWithTitle:title];
        [[bridgeMidiChannelDropdown itemAtIndex:i - 1] setTag:i];
    }
    [bridgeMidiChannelDropdown selectItemAtIndex:std::max(0, std::min(15, cfg.bridge_midi_channel - 1))];
    [bridgeMidiChannelDropdown setTarget:self];
    [bridgeMidiChannelDropdown setAction:@selector(onBridgeSettingsChanged:)];
    [bridgeTabView addSubview:bridgeMidiChannelDropdown];
    bridgeY -= 44;

    NSTextField* bridgeMappingsLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, bridgeY + 8, 200, 20)];
    [bridgeMappingsLabel setStringValue:@"Mappings:"];
    [bridgeMappingsLabel setFont:[NSFont systemFontOfSize:11]];
    [bridgeMappingsLabel setBezeled:NO];
    [bridgeMappingsLabel setEditable:NO];
    [bridgeMappingsLabel setSelectable:NO];
    [bridgeMappingsLabel setBackgroundColor:[NSColor clearColor]];
    [bridgeTabView addSubview:bridgeMappingsLabel];

    bridgeMappingScrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(controlX, bridgeY - 122, 360, 130)];
    [bridgeMappingScrollView setBorderType:NSBezelBorder];
    [bridgeMappingScrollView setHasVerticalScroller:YES];
    [bridgeMappingScrollView setHasHorizontalScroller:NO];
    bridgeMappingTableView = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 360, 130)];
    NSTableColumn* sourceColumn = [[[NSTableColumn alloc] initWithIdentifier:@"source"] autorelease];
    [[sourceColumn headerCell] setStringValue:@"Source"];
    [sourceColumn setWidth:120];
    [bridgeMappingTableView addTableColumn:sourceColumn];
    NSTableColumn* midiColumn = [[[NSTableColumn alloc] initWithIdentifier:@"midi"] autorelease];
    [[midiColumn headerCell] setStringValue:@"MIDI"];
    [midiColumn setWidth:70];
    [bridgeMappingTableView addTableColumn:midiColumn];
    NSTableColumn* statusColumn = [[[NSTableColumn alloc] initWithIdentifier:@"enabled"] autorelease];
    [[statusColumn headerCell] setStringValue:@"State"];
    [statusColumn setWidth:80];
    [bridgeMappingTableView addTableColumn:statusColumn];
    NSTableColumn* nameColumn = [[[NSTableColumn alloc] initWithIdentifier:@"name"] autorelease];
    [[nameColumn headerCell] setStringValue:@"Label"];
    [nameColumn setWidth:150];
    [bridgeMappingTableView addTableColumn:nameColumn];
    [bridgeMappingTableView setDelegate:self];
    [bridgeMappingTableView setDataSource:self];
    [bridgeMappingTableView setHeaderView:[[[NSTableHeaderView alloc] initWithFrame:NSMakeRect(0, 0, 360, 18)] autorelease]];
    [bridgeMappingScrollView setDocumentView:bridgeMappingTableView];
    [bridgeTabView addSubview:bridgeMappingScrollView];

    bridgeSourceKindDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX, bridgeY - 156, 120, 24) pullsDown:NO];
    [bridgeSourceKindDropdown addItemWithTitle:@"Channel"];
    [[bridgeSourceKindDropdown itemAtIndex:0] setTag:(NSInteger)SourceKind::Channel];
    [bridgeSourceKindDropdown addItemWithTitle:@"Bus"];
    [[bridgeSourceKindDropdown itemAtIndex:1] setTag:(NSInteger)SourceKind::Bus];
    [bridgeSourceKindDropdown addItemWithTitle:@"Main"];
    [[bridgeSourceKindDropdown itemAtIndex:2] setTag:(NSInteger)SourceKind::Main];
    [bridgeSourceKindDropdown addItemWithTitle:@"Matrix"];
    [[bridgeSourceKindDropdown itemAtIndex:3] setTag:(NSInteger)SourceKind::Matrix];
    [bridgeSourceKindDropdown setTarget:self];
    [bridgeSourceKindDropdown setAction:@selector(onBridgeSourceKindChanged:)];
    [bridgeTabView addSubview:bridgeSourceKindDropdown];

    bridgeSourceNumberDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX + 130, bridgeY - 156, 90, 24) pullsDown:NO];
    [bridgeSourceNumberDropdown setTarget:self];
    [bridgeSourceNumberDropdown setAction:@selector(onBridgeSettingsChanged:)];
    [bridgeTabView addSubview:bridgeSourceNumberDropdown];

    bridgeMidiValueField = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX + 230, bridgeY - 156, 70, 24)];
    [bridgeMidiValueField setStringValue:@"0"];
    [bridgeMidiValueField setTarget:self];
    [bridgeMidiValueField setAction:@selector(onBridgeSettingsChanged:)];
    [bridgeTabView addSubview:bridgeMidiValueField];

    bridgeMappingEnabledCheckbox = [[NSButton alloc] initWithFrame:NSMakeRect(controlX + 310, bridgeY - 152, 90, 20)];
    [bridgeMappingEnabledCheckbox setButtonType:NSButtonTypeSwitch];
    [bridgeMappingEnabledCheckbox setTitle:@"Enabled"];
    [bridgeMappingEnabledCheckbox setState:NSControlStateValueOn];
    [bridgeMappingEnabledCheckbox setTarget:self];
    [bridgeMappingEnabledCheckbox setAction:@selector(onBridgeSettingsChanged:)];
    [bridgeTabView addSubview:bridgeMappingEnabledCheckbox];

    addInfoLabel(bridgeTabView, NSMakeRect(controlX, bridgeY - 180, 360, 18),
                 @"Family, strip, and MIDI value for the selected WING strip entry.",
                 10, [NSColor tertiaryLabelColor]);

    bridgeAddOrUpdateMappingButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, bridgeY - 218, 140, 30)];
    [bridgeAddOrUpdateMappingButton setBezelStyle:NSBezelStyleRounded];
    [bridgeAddOrUpdateMappingButton setTitle:@"Add / Update Mapping"];
    [bridgeAddOrUpdateMappingButton setTarget:self];
    [bridgeAddOrUpdateMappingButton setAction:@selector(onAddOrUpdateBridgeMappingClicked:)];
    [bridgeTabView addSubview:bridgeAddOrUpdateMappingButton];

    bridgeRemoveMappingButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX + 150, bridgeY - 218, 120, 30)];
    [bridgeRemoveMappingButton setBezelStyle:NSBezelStyleRounded];
    [bridgeRemoveMappingButton setTitle:@"Remove Mapping"];
    [bridgeRemoveMappingButton setTarget:self];
    [bridgeRemoveMappingButton setAction:@selector(onRemoveBridgeMappingClicked:)];
    [bridgeTabView addSubview:bridgeRemoveMappingButton];
    bridgeY -= 240;

    applyBridgeButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, bridgeY, 220, 32)];
    [applyBridgeButton setBezelStyle:NSBezelStyleRounded];
    [applyBridgeButton setTitle:@"Apply Bridge Settings"];
    [applyBridgeButton setTarget:self];
    [applyBridgeButton setAction:@selector(onApplyBridgeSettingsClicked:)];
    [bridgeTabView addSubview:applyBridgeButton];
    [self refreshBridgeSourceNumberDropdown];
    [self refreshBridgeMappingTable];
    [self resetBridgeMappingEditor];
    bridgeY -= 40;

    NSTextField* midiHeader = [[NSTextField alloc] initWithFrame:NSMakeRect(20, advY, 240, 20)];
    [midiHeader setStringValue:@"🎛 Wing Control Integration"];
    [midiHeader setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [midiHeader setBezeled:NO];
    [midiHeader setEditable:NO];
    [midiHeader setSelectable:NO];
    [midiHeader setBackgroundColor:[NSColor clearColor]];
    [midiHeader setTextColor:[NSColor labelColor]];
    [advancedTabView addSubview:midiHeader];
    controlIntegrationTabStatusLabel = makeTabStatusLabel(advancedTabView, advY);
    advY -= 32;
    addInfoLabel(advancedTabView, NSMakeRect(20, advY, 590, 30),
                 @"These options map Wing user controls to REAPER actions and warning feedback for operators who want hands on the console.",
                 11, [NSColor secondaryLabelColor]);
    advY -= 44;

    NSTextField* midiActionsLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, advY + 8, labelW, 20)];
    [midiActionsLabel setStringValue:@"Wing Control Enabled:"];
    [midiActionsLabel setFont:[NSFont systemFontOfSize:11]];
    [midiActionsLabel setBezeled:NO];
    [midiActionsLabel setEditable:NO];
    [midiActionsLabel setSelectable:NO];
    [midiActionsLabel setBackgroundColor:[NSColor clearColor]];
    [advancedTabView addSubview:midiActionsLabel];
    BOOL midiFullyEnabled = ext.IsMidiActionsEnabled();
    midiActionsControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(controlX, advY + 4, 120, 24)];
    [midiActionsControl setSegmentCount:2];
    [midiActionsControl setLabel:@"OFF" forSegment:0];
    [midiActionsControl setLabel:@"ON" forSegment:1];
    [midiActionsControl setSelectedSegment:midiFullyEnabled ? 1 : 0];
    [midiActionsControl setSegmentStyle:NSSegmentStyleRounded];
    [midiActionsControl setTarget:self];
    [midiActionsControl setAction:@selector(onMidiActionsToggled:)];
    [midiActionsControl setEnabled:NO];
    [advancedTabView addSubview:midiActionsControl];
    advY -= 28;
    addInfoLabel(advancedTabView, NSMakeRect(controlX, advY, 360, 28),
                 @"Allows configured Wing buttons or user controls to trigger REAPER transport and related actions after live setup is validated.",
                 10, [NSColor tertiaryLabelColor]);
    advY -= 36;

    midiActionsSummaryLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, advY, 420, 34)];
    [midiActionsSummaryLabel setStringValue:@"No pending MIDI shortcut changes."];
    [midiActionsSummaryLabel setFont:[NSFont systemFontOfSize:11]];
    [midiActionsSummaryLabel setBezeled:NO];
    [midiActionsSummaryLabel setEditable:NO];
    [midiActionsSummaryLabel setSelectable:NO];
    [midiActionsSummaryLabel setBackgroundColor:[NSColor clearColor]];
    [midiActionsSummaryLabel setTextColor:[NSColor secondaryLabelColor]];
    [midiActionsSummaryLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [midiActionsSummaryLabel setUsesSingleLineMode:NO];
    [advancedTabView addSubview:midiActionsSummaryLabel];
    advY -= 42;

    midiActionsDetailLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(controlX, advY, 420, 38)];
    [midiActionsDetailLabel setStringValue:@"MIDI shortcuts are disabled."];
    [midiActionsDetailLabel setFont:[NSFont systemFontOfSize:10.5]];
    [midiActionsDetailLabel setBezeled:NO];
    [midiActionsDetailLabel setEditable:NO];
    [midiActionsDetailLabel setSelectable:NO];
    [midiActionsDetailLabel setBackgroundColor:[NSColor clearColor]];
    [midiActionsDetailLabel setTextColor:[NSColor secondaryLabelColor]];
    [midiActionsDetailLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [midiActionsDetailLabel setUsesSingleLineMode:NO];
    [advancedTabView addSubview:midiActionsDetailLabel];
    advY -= 42;

    NSTextField* ccLayerLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(labelX, advY + 8, labelW, 20)];
    [ccLayerLabel setStringValue:@"Warning CC Layer:"];
    [ccLayerLabel setFont:[NSFont systemFontOfSize:11]];
    [ccLayerLabel setBezeled:NO];
    [ccLayerLabel setEditable:NO];
    [ccLayerLabel setSelectable:NO];
    [ccLayerLabel setBackgroundColor:[NSColor clearColor]];
    [advancedTabView addSubview:ccLayerLabel];
    ccLayerDropdown = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(controlX, advY + 4, 140, 24) pullsDown:NO];
    for (int i = 1; i <= 16; ++i) {
        NSString* title = [NSString stringWithFormat:@"Layer %d", i];
        [ccLayerDropdown addItemWithTitle:title];
        [[ccLayerDropdown itemAtIndex:i - 1] setTag:i];
    }
    int selectedLayer = std::min(16, std::max(1, cfg.warning_flash_cc_layer));
    [ccLayerDropdown selectItemAtIndex:selectedLayer - 1];
    [ccLayerDropdown setTarget:self];
    [ccLayerDropdown setAction:@selector(onAutoRecordSettingsChanged:)];
    [advancedTabView addSubview:ccLayerDropdown];
    advY -= 28;
    addInfoLabel(advancedTabView, NSMakeRect(controlX, advY, 360, 28),
                 @"Select which Wing user-control layer should flash or react when the auto-trigger enters warning mode.",
                 10, [NSColor tertiaryLabelColor]);
    advY -= 40;

    applyMidiActionsButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, advY, 180, 30)];
    [applyMidiActionsButton setBezelStyle:NSBezelStyleRounded];
    [applyMidiActionsButton setTitle:@"Apply MIDI Shortcuts"];
    [applyMidiActionsButton setTarget:self];
    [applyMidiActionsButton setAction:@selector(onApplyMidiActionsClicked:)];
    [applyMidiActionsButton setEnabled:NO];
    [advancedTabView addSubview:applyMidiActionsButton];

    discardMidiActionsButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX + 190, advY, 120, 30)];
    [discardMidiActionsButton setBezelStyle:NSBezelStyleRounded];
    [discardMidiActionsButton setTitle:@"Discard"];
    [discardMidiActionsButton setTarget:self];
    [discardMidiActionsButton setAction:@selector(onDiscardMidiActionsClicked:)];
    [discardMidiActionsButton setEnabled:NO];
    [advancedTabView addSubview:discardMidiActionsButton];
    advY -= 42;

    NSBox* advancedSeparator = [[NSBox alloc] initWithFrame:NSMakeRect(20, advY, setupWidth, 1)];
    [advancedSeparator setBoxType:NSBoxSeparator];
    [advancedTabView addSubview:advancedSeparator];
    advY -= 28;

    NSTextField* supportHeader = [[NSTextField alloc] initWithFrame:NSMakeRect(20, advY, 240, 20)];
    [supportHeader setStringValue:@"🛠 Support and Diagnostics"];
    [supportHeader setFont:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]];
    [supportHeader setBezeled:NO];
    [supportHeader setEditable:NO];
    [supportHeader setSelectable:NO];
    [supportHeader setBackgroundColor:[NSColor clearColor]];
    [supportHeader setTextColor:[NSColor labelColor]];
    [advancedTabView addSubview:supportHeader];
    advY -= 32;
    addInfoLabel(advancedTabView, NSMakeRect(20, advY, 590, 30),
                 @"Use the debug log when things get weird, or when you simply want receipts for discovery, routing, validation, and recorder activity.",
                 11, [NSColor secondaryLabelColor]);
    advY -= 44;

    debugLogToggleButton = [[NSButton alloc] initWithFrame:NSMakeRect(controlX, advY, 180, 28)];
    [debugLogToggleButton setButtonType:NSButtonTypeMomentaryPushIn];
    [debugLogToggleButton setBezelStyle:NSBezelStyleRounded];
    [debugLogToggleButton setTitle:@"Open Debug Log"];
    [debugLogToggleButton setTarget:self];
    [debugLogToggleButton setAction:@selector(onDebugLogToggled:)];
    [advancedTabView addSubview:debugLogToggleButton];
    addInfoLabel(advancedTabView, NSMakeRect(labelX, advY + 4, labelW, 20),
                 @"Inspect plugin activity:" , 11, [NSColor labelColor]);
    advY -= 32;
    addInfoLabel(advancedTabView, NSMakeRect(controlX, advY, 360, 28),
                 @"Opens a live activity window with connection, validation, OSC, and routing messages for troubleshooting.",
                 10, [NSColor tertiaryLabelColor]);

    const int logHeight = 320;
    logScrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 20, 660, logHeight)];
    [logScrollView setHasVerticalScroller:YES];
    [logScrollView setHasHorizontalScroller:NO];
    [logScrollView setBorderType:NSBezelBorder];
    [logScrollView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [logScrollView setBackgroundColor:[NSColor textBackgroundColor]];

    activityLogView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 640, logHeight)];
    [activityLogView setFont:[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]];
    [activityLogView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [activityLogView setTextColor:[NSColor labelColor]];
    [activityLogView setBackgroundColor:[NSColor textBackgroundColor]];

    [logScrollView setDocumentView:activityLogView];

    auto normalizeTabDocument = ^(WingConnectorFlippedView* documentView, CGFloat minBottomPadding) {
        if (!documentView) {
            return;
        }

        CGFloat minY = CGFLOAT_MAX;
        CGFloat maxY = 0.0;
        for (NSView* subview in [documentView subviews]) {
            if ([subview isHidden]) {
                continue;
            }
            const NSRect frame = [subview frame];
            minY = std::min(minY, NSMinY(frame));
            maxY = std::max(maxY, NSMaxY(frame));
        }

        if (minY == CGFLOAT_MAX) {
            return;
        }

        if (minY < minBottomPadding) {
            const CGFloat shift = minBottomPadding - minY;
            for (NSView* subview in [documentView subviews]) {
                NSRect frame = [subview frame];
                frame.origin.y += shift;
                [subview setFrame:frame];
            }
            maxY += shift;
        }

        NSRect documentFrame = [documentView frame];
        documentFrame.size.height = std::max(documentFrame.size.height, maxY + 24.0);
        [documentView setFrame:documentFrame];
    };
    normalizeTabDocument(setupTabView, 26.0);
    normalizeTabDocument(automationTabView, 26.0);
    normalizeTabDocument(wingTabView, 26.0);
    normalizeTabDocument(advancedTabView, 26.0);
    if (kShowBridgeTabInMainUI) {
        normalizeTabDocument(bridgeTabView, 26.0);
    }

    [self scrollTabViewToTop:setupTabScrollView];
    [self scrollTabViewToTop:automationTabScrollView];
    [self scrollTabViewToTop:wingTabScrollView];
    [self scrollTabViewToTop:advancedTabScrollView];
    if (kShowBridgeTabInMainUI) {
        [self scrollTabViewToTop:bridgeTabScrollView];
    }

    [self refreshBridgeMidiOutputDropdown];
    [self refreshBridgeStatus];
    [self createDebugLogWindow];
    [self finalizeFormLayout];
}

- (void)updateConnectionStatus {
    isConnected = ReaperExtension::Instance().IsConnected();
    [self refreshMonitorTrackDropdown];

    if (isConnected) {
        [self setConnectionStatusText:@"Console: Connected" color:[NSColor systemGreenColor] connected:YES];
        [connectButton setTitle:@"Disconnect"];
    } else {
        [self setConnectionStatusText:@"Console: Not Connected" color:[NSColor secondaryLabelColor] connected:NO];
        [connectButton setTitle:@"Connect"];
    }
    // Re-enable buttons only if no operation is currently running
    if (!isWorking) {
        [setupSoundcheckButton setEnabled:YES];
        [scanButton setEnabled:YES];
        [connectButton setEnabled:YES];
        // Toggle button handled in updateToggleSoundcheckButtonLabel
    }
    [self updateValidationStatusLabel];
    [self updateMidiStatusLabel];
    [self updateRecorderStatusLabel];
    [self updatePendingSetupUI];
    [self updateSetupReadinessDetails];
    [self updateMidiActionsUI];
    [self updateToggleSoundcheckButtonLabel];
    [self updateSetupSoundcheckButtonLabel];
    [self updateAutoTriggerControlsEnabled];
    [self updateAutomationDetails];
    [self refreshLiveSetupValidation];
}

- (void)scrollTabViewToTop:(NSScrollView*)scrollView {
    if (!scrollView) {
        return;
    }

    NSClipView* clipView = [scrollView contentView];
    NSView* documentView = [scrollView documentView];
    if (!clipView || !documentView) {
        return;
    }

    CGFloat maxY = 0.0;
    for (NSView* subview in [documentView subviews]) {
        if ([subview isHidden]) {
            continue;
        }
        maxY = std::max(maxY, NSMaxY([subview frame]));
    }

    const CGFloat desiredTopPadding = 26.0;
    const CGFloat clipHeight = NSHeight([clipView bounds]);
    const CGFloat documentHeight = NSHeight([documentView bounds]);
    const CGFloat contentTop = maxY + desiredTopPadding;
    const CGFloat maxOffset = std::max<CGFloat>(0.0, documentHeight - clipHeight);
    const CGFloat topOffset = std::max<CGFloat>(0.0, std::min(maxOffset, contentTop - clipHeight));
    [clipView scrollToPoint:NSMakePoint(0, topOffset)];
    [scrollView reflectScrolledClipView:clipView];
}

- (void)tabView:(NSTabView*)tabView didSelectTabViewItem:(NSTabViewItem*)tabViewItem {
    if (tabView != settingsTabView) {
        return;
    }

    NSView* view = [tabViewItem view];
    if ([view isKindOfClass:[NSScrollView class]]) {
        [self scrollTabViewToTop:(NSScrollView*)view];
    }
}

- (void)setHeaderStatusIcon:(NSImageView*)iconView symbolName:(NSString*)symbolName fallback:(NSString*)fallback color:(NSColor*)color {
    if (!iconView) {
        return;
    }
    NSColor* resolvedColor = color ? color : [NSColor secondaryLabelColor];
    NSImage* symbol = nil;
    if (@available(macOS 11.0, *)) {
        NSImageSymbolConfiguration* config = [NSImageSymbolConfiguration configurationWithPointSize:14 weight:NSFontWeightSemibold];
        symbol = [NSImage imageWithSystemSymbolName:symbolName accessibilityDescription:nil];
        symbol = [symbol imageWithSymbolConfiguration:config];
    }
    if (!symbol && fallback) {
        symbol = [NSImage imageNamed:fallback];
    }
    [iconView setContentTintColor:resolvedColor];
    [iconView setImage:symbol];
}

- (void)setConnectionStatusText:(NSString*)text color:(NSColor*)color connected:(BOOL)connected {
    if (!statusLabel) {
        return;
    }
    NSColor* resolvedColor = color ? color : [NSColor secondaryLabelColor];
    [statusLabel setStringValue:text ? text : @"Console: Not Connected"];
    [statusLabel setTextColor:[NSColor labelColor]];
    [self setHeaderStatusIcon:statusIconView
                   symbolName:(connected ? @"dot.radiowaves.left.and.right" : @"bolt.horizontal.circle")
                     fallback:(connected ? NSImageNameStatusAvailable : NSImageNameStatusNone)
                        color:resolvedColor];
    [self updateTabStatusIndicators];
}

- (void)setValidationStatusText:(NSString*)text color:(NSColor*)color {
    if (!validationStatusLabel) {
        return;
    }
    NSString* resolvedText = text ? text : @"Reaper Recorder: Not Ready";
    NSColor* resolvedColor = color ? color : [NSColor secondaryLabelColor];
    [validationStatusLabel setStringValue:resolvedText];
    [validationStatusLabel setTextColor:[NSColor labelColor]];
    NSString* symbolName = @"questionmark.circle";
    NSString* fallbackName = NSImageNameStatusNone;
    if ([resolvedText containsString:@"Enabled + Record"]) {
        symbolName = @"bolt.badge.checkmark";
        fallbackName = NSImageNameStatusAvailable;
    } else if ([resolvedText containsString:@"Enabled + Warning Trigger"]) {
        symbolName = @"exclamationmark.shield";
        fallbackName = NSImageNameCaution;
    } else if ([resolvedText containsString:@"Enabled"]) {
        symbolName = @"checkmark.circle.fill";
        fallbackName = NSImageNameStatusAvailable;
    } else if ([resolvedText containsString:@"Pending"] || [resolvedText containsString:@"Checking"]) {
        symbolName = @"arrow.triangle.2.circlepath.circle.fill";
        fallbackName = NSImageNameRefreshTemplate;
    } else if ([resolvedText containsString:@"Needs Attention"] ||
               [resolvedText containsString:@"Review / Rebuild"]) {
        symbolName = @"exclamationmark.triangle.fill";
        fallbackName = NSImageNameCaution;
    }
    [self setHeaderStatusIcon:validationIconView symbolName:symbolName fallback:fallbackName color:resolvedColor];
}

- (void)updateTabStatusIndicators {
    auto setTabStatus = ^(NSTextField* label, NSString* text, NSColor* color) {
        if (!label) {
            return;
        }
        [label setStringValue:text ? text : @"Inactive"];
        [label setTextColor:color ? color : [NSColor secondaryLabelColor]];
    };

    NSString* consoleText = isConnected ? @"Connected" : @"Inactive";
    NSColor* consoleColor = isConnected ? [NSColor systemGreenColor] : [NSColor secondaryLabelColor];

    auto& config = ReaperExtension::Instance().GetConfig();
    NSString* reaperText = @"Inactive";
    NSColor* reaperColor = [NSColor secondaryLabelColor];
    if (validationInProgress || hasPendingSetupDraft ||
        pendingOutputMode != config.soundcheck_output_mode || automationSettingsDirty) {
        reaperText = @"Pending";
        reaperColor = [NSColor systemOrangeColor];
    } else if (liveSetupValidated && config.auto_record_enabled && !config.auto_record_warning_only) {
        reaperText = @"Ready";
        reaperColor = [NSColor systemGreenColor];
    } else if (liveSetupValidated || isConnected) {
        reaperText = @"Attention";
        reaperColor = [NSColor systemOrangeColor];
    }

    NSString* wingText = @"Inactive";
    NSColor* wingColor = [NSColor secondaryLabelColor];
    if (recorderSettingsDirty) {
        wingText = @"Pending";
        wingColor = [NSColor systemOrangeColor];
    } else if (config.recorder_coordination_enabled && config.sd_auto_record_with_reaper && config.auto_record_enabled) {
        wingText = @"Ready";
        wingColor = [NSColor systemGreenColor];
    } else if (config.recorder_coordination_enabled) {
        wingText = @"Enabled";
        wingColor = [NSColor systemOrangeColor];
    }

    NSString* controlText = @"Inactive";
    NSColor* controlColor = [NSColor secondaryLabelColor];
    if (midiActionsDirty || latestMidiValidationState == ValidationState::Warning) {
        controlText = @"Pending";
        controlColor = [NSColor systemOrangeColor];
    } else if (ReaperExtension::Instance().IsMidiActionsEnabled()) {
        controlText = @"Ready";
        controlColor = [NSColor systemGreenColor];
    }

    [consoleTabItem setLabel:@"Console"];
    [reaperTabItem setLabel:@"Reaper"];
    [wingTabItemRef setLabel:@"Wing"];
    [controlIntegrationTabItem setLabel:@"Control Integration"];

    setTabStatus(consoleTabStatusLabel, consoleText, consoleColor);
    setTabStatus(reaperTabStatusLabel, reaperText, reaperColor);
    setTabStatus(wingTabStatusLabel, wingText, wingColor);
    setTabStatus(controlIntegrationTabStatusLabel, controlText, controlColor);

    if (kShowBridgeTabInMainUI) {
        [bridgeTabItemRef setLabel:@"Bridge"];
        const BOOL bridgeEnabled = ReaperExtension::Instance().GetConfig().bridge_enabled;
        setTabStatus(bridgeTabStatusLabel,
                     bridgeEnabled ? @"Enabled" : @"Inactive",
                     bridgeEnabled ? [NSColor systemGreenColor] : [NSColor secondaryLabelColor]);
    }
}

- (void)updateValidationStatusLabel {
    auto& config = ReaperExtension::Instance().GetConfig();
    if (validationInProgress) {
        [self setValidationStatusText:@"Reaper Recorder: Checking" color:[NSColor systemOrangeColor]];
        return;
    }
    if (automationSettingsDirty) {
        [self setValidationStatusText:@"Reaper Recorder: Pending Trigger Apply" color:[NSColor systemOrangeColor]];
        return;
    }
    if (hasPendingSetupDraft || pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) {
        [self setValidationStatusText:@"Reaper Recorder: Pending Apply" color:[NSColor systemOrangeColor]];
        return;
    }
    if (liveSetupValidated) {
        if (config.auto_record_enabled) {
            [self setValidationStatusText:(config.auto_record_warning_only
                                           ? @"Reaper Recorder: Enabled + Warning Trigger"
                                           : @"Reaper Recorder: Enabled + Record")
                                   color:(config.auto_record_warning_only ? [NSColor systemOrangeColor] : [NSColor systemGreenColor])];
        } else {
            [self setValidationStatusText:@"Reaper Recorder: Enabled" color:[NSColor systemOrangeColor]];
        }
        return;
    }
    if (isConnected) {
        [self setValidationStatusText:@"Reaper Recorder: Review / Rebuild" color:[NSColor systemOrangeColor]];
    } else {
        [self setValidationStatusText:@"Reaper Recorder: Not Ready" color:[NSColor secondaryLabelColor]];
    }
    [self updateTabStatusIndicators];
}

- (void)updatePendingSetupUI {
    if (pendingSetupSummaryLabel) {
        NSString* summary = @"No pending setup changes. Choose sources for a new setup, or change recording mode to stage a rebuild of the current managed setup.";
        NSColor* summaryColor = [NSColor secondaryLabelColor];
        if (hasPendingSetupDraft || pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) {
            const auto selected_sources = SelectedSourcesOnly(pendingSetupChannels);
            if (selected_sources.empty()) {
                summary = [NSString stringWithFormat:@"Current managed setup staged for rebuild in %s mode. Click Rebuild Current Setup to reuse the saved selection and rewrite routing.", pendingOutputMode.c_str()];
            } else {
                summary = [NSString stringWithFormat:@"Changes staged for %lu sources in %s mode. Review if needed, then click Apply Setup.", static_cast<unsigned long>(selected_sources.size()), pendingOutputMode.c_str()];
            }
            if (pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode && !hasPendingSetupDraft) {
                summary = [NSString stringWithFormat:@"Recording I/O mode change staged. Click Rebuild Current Setup to reuse the current managed selection in %s mode.", pendingOutputMode.c_str()];
            }
            if (pendingSetupUsesExistingSelection && !hasPendingSetupDraft) {
                summary = [NSString stringWithFormat:@"Current managed selection staged for %s mode. Click Rebuild Current Setup to rebuild routing without reopening source selection.", pendingOutputMode.c_str()];
            } else {
                summary = [summary stringByAppendingString:@" Use Edit Pending Sources… to adjust the draft, or Discard to abandon it."];
            }
            summaryColor = [NSColor systemOrangeColor];
        }
        [pendingSetupSummaryLabel setStringValue:summary];
        [pendingSetupSummaryLabel setTextColor:summaryColor];
        [pendingSetupSummaryLabel setHidden:(!hasPendingSetupDraft && pendingOutputMode == ReaperExtension::Instance().GetConfig().soundcheck_output_mode) ? YES : NO];
    }

    if (applyPendingSetupButton) {
        const BOOL canApply = !isWorking &&
            ((hasPendingSetupDraft && !pendingSetupChannels.empty()) ||
             (!hasPendingSetupDraft && pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode));
        [applyPendingSetupButton setEnabled:canApply];
    }
    [self updateApplyPendingSetupButtonLabel];
    if (discardPendingSetupButton) {
        [discardPendingSetupButton setEnabled:(hasPendingSetupDraft || pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) && !isWorking];
    }
}

- (void)updateSetupReadinessDetails {
    if (!setupReadinessDetailLabel) {
        return;
    }
    const BOOL rebuildingCurrentManagedSetup =
        (!hasPendingSetupDraft && pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) ||
        (pendingSetupUsesExistingSelection && !hasPendingSetupDraft);
    NSString* detail = @"Connect to a Wing to validate the current managed setup, then rebuild it only when routing or recording mode needs to change.";
    NSString* nextStep = @"Next step: connect to a WING, validate the managed setup, then use Choose Sources only when you need a different selection.";
    NSColor* detailColor = [NSColor secondaryLabelColor];

    if (isWorking) {
        detail = @"Working on the current request.";
        nextStep = @"Next step: wait for the current operation to finish before staging another setup change.";
    } else if (hasPendingSetupDraft || pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) {
        if (rebuildingCurrentManagedSetup) {
            detail = @"A rebuild of the current managed setup is staged. Rebuilding will reuse the managed source selection and update WING routing, REAPER tracks, and playback inputs for the selected mode.";
            nextStep = @"Next step: click Rebuild Current Setup if you want to keep the current managed selection, or use Choose Sources to stage a different setup.";
        } else {
            detail = @"Pending setup changes are staged. Applying them will update WING routing, REAPER tracks, and playback inputs for the selected sources.";
            nextStep = @"Next step: review the staged source draft, then click Apply Setup.";
        }
        detailColor = [NSColor systemOrangeColor];
    } else if (!latestLiveSetupValidationDetails.empty()) {
        detail = [NSString stringWithUTF8String:latestLiveSetupValidationDetails.c_str()];
        if (latestLiveSetupValidationState == ValidationState::Ready) {
            nextStep = @"Setup is ready. Use Live/Soundcheck to switch the validated setup now, or change recording mode to stage a rebuild of the current managed setup.";
            detailColor = [NSColor systemGreenColor];
        } else if (latestLiveSetupValidationState == ValidationState::Warning) {
            nextStep = @"Next step: review the validation warning. Rebuild the current managed setup if routing changed, or use Choose Sources only if you want a different selection.";
            detailColor = [NSColor systemOrangeColor];
        }
    }

    NSString* combined = [NSString stringWithFormat:@"%@\n%@", detail, nextStep];
    [setupReadinessDetailLabel setStringValue:combined];
    [setupReadinessDetailLabel setTextColor:detailColor];
}

- (void)updateApplyPendingSetupButtonLabel {
    if (!applyPendingSetupButton) {
        return;
    }
    const BOOL rebuildingCurrentManagedSetup =
        (!hasPendingSetupDraft && pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) ||
        (pendingSetupUsesExistingSelection && !hasPendingSetupDraft);
    [applyPendingSetupButton setTitle:(rebuildingCurrentManagedSetup ? @"Rebuild Current Setup" : @"Apply Setup")];
}

- (void)updateMidiActionsUI {
    auto& extension = ReaperExtension::Instance();
    const BOOL appliedEnabled = extension.IsMidiActionsEnabled() ? YES : NO;
    const BOOL pendingApply = hasPendingSetupDraft || pendingOutputMode != extension.GetConfig().soundcheck_output_mode;
    const BOOL liveSetupControlsEnabled = (liveSetupValidated && !pendingApply && !isWorking) ? YES : NO;
    const BOOL canApplyMidiDraft = (!isWorking && midiActionsDirty &&
                                    (!pendingMidiActionsEnabled || liveSetupControlsEnabled)) ? YES : NO;

    if (!midiActionsDirty) {
        pendingMidiActionsEnabled = appliedEnabled;
        if (midiActionsControl) {
            [midiActionsControl setSelectedSegment:appliedEnabled ? 1 : 0];
        }
    }
    const BOOL layerControlEnabled = (!isWorking && (appliedEnabled || pendingMidiActionsEnabled)) ? YES : NO;

    if (midiActionsSummaryLabel) {
        NSString* summary = @"No pending MIDI shortcut changes.";
        NSColor* summaryColor = [NSColor secondaryLabelColor];
        if (midiActionsDirty) {
            summary = [NSString stringWithFormat:@"MIDI shortcut change staged: %s. Click Apply MIDI Shortcuts to program the WING buttons, or Discard to keep the current state.",
                       pendingMidiActionsEnabled ? "ON" : "OFF"];
            if (pendingMidiActionsEnabled && !liveSetupValidated) {
                summary = @"MIDI shortcut change staged, but apply stays unavailable until live setup validates.";
            }
            summaryColor = [NSColor systemOrangeColor];
        } else if (appliedEnabled) {
            summary = @"MIDI shortcuts are active on the currently configured WING CC layer.";
            summaryColor = [NSColor systemGreenColor];
        }
        [midiActionsSummaryLabel setStringValue:summary];
        [midiActionsSummaryLabel setTextColor:summaryColor];
    }

    if (midiActionsDetailLabel) {
        NSString* detail = @"MIDI shortcuts are disabled.";
        NSColor* detailColor = [NSColor secondaryLabelColor];
        if (midiActionsDirty) {
            detail = [NSString stringWithFormat:@"Pending MIDI shortcut change: %s. Apply to reprogram the WING buttons on layer %ld.",
                      pendingMidiActionsEnabled ? "ON" : "OFF",
                      (long)[[ccLayerDropdown selectedItem] tag]];
            detailColor = [NSColor systemOrangeColor];
        } else if (!latestMidiValidationDetails.empty()) {
            detail = [NSString stringWithUTF8String:latestMidiValidationDetails.c_str()];
            if (latestMidiValidationState == ValidationState::Ready) {
                detailColor = [NSColor systemGreenColor];
            } else if (latestMidiValidationState == ValidationState::Warning) {
                detailColor = [NSColor systemOrangeColor];
            }
        }
        [midiActionsDetailLabel setStringValue:detail];
        [midiActionsDetailLabel setTextColor:detailColor];
    }

    if (applyMidiActionsButton) {
        [applyMidiActionsButton setEnabled:canApplyMidiDraft];
    }
    if (discardMidiActionsButton) {
        [discardMidiActionsButton setEnabled:(midiActionsDirty && !isWorking) ? YES : NO];
    }
    if (ccLayerDropdown) {
        [ccLayerDropdown setEnabled:layerControlEnabled];
    }
    [self updateMidiStatusLabel];
}

- (void)updateAutomationDetails {
    if (!automationDetailLabel) {
        return;
    }
    NSString* detail = @"Next step: connect to a WING, choose sources, and apply the setup before enabling Auto Trigger.";
    NSColor* detailColor = [NSColor secondaryLabelColor];

    if (automationSettingsDirty) {
        detail = @"Auto Trigger settings changed. Next step: click Apply Auto Trigger Settings to resume trigger monitoring with the staged threshold, hold time, recorder, and CC layer.";
        detailColor = [NSColor systemOrangeColor];
    } else if (hasPendingSetupDraft || pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) {
        detail = @"Auto Trigger is blocked by pending setup changes. Next step: apply the staged setup or rebuild the current managed setup in the Reaper tab.";
        detailColor = [NSColor systemOrangeColor];
    } else if (!liveSetupValidated) {
        detail = [NSString stringWithFormat:@"Auto Trigger is blocked until recording setup validates. Next step: %s",
                  latestLiveSetupValidationDetails.empty() ? "prepare sources and routing in the Reaper tab." : latestLiveSetupValidationDetails.c_str()];
        detailColor = isConnected ? [NSColor systemOrangeColor] : [NSColor secondaryLabelColor];
    } else if (ReaperExtension::Instance().IsSoundcheckModeEnabled()) {
        detail = @"Auto Trigger is paused because Soundcheck Mode is active on the managed channels. Next step: switch back to Live Mode on WING or in the plugin before relying on signal-triggered starts.";
        detailColor = [NSColor systemOrangeColor];
    } else if (!ReaperExtension::Instance().GetConfig().auto_record_enabled) {
        detail = @"Auto Trigger is currently off. The recorder setup is configured, but trigger monitoring will stay idle until you turn it on and apply the change.";
        detailColor = [NSColor systemOrangeColor];
    } else {
        detail = @"Auto Trigger is clear to run with the current recording and virtual soundcheck setup.";
        detailColor = [NSColor systemGreenColor];
    }

    [automationDetailLabel setStringValue:detail];
    [automationDetailLabel setTextColor:detailColor];

    if (recorderDetailLabel) {
        NSString* recorderDetail = @"Recorder coordination is using the currently applied settings.";
        NSColor* recorderColor = [NSColor secondaryLabelColor];
        if (recorderSettingsDirty) {
            recorderDetail = @"Recorder coordination changes are staged. Next step: click Apply Recorder Settings to update the target recorder, source pair, and follow behavior.";
            recorderColor = [NSColor systemOrangeColor];
        } else if (!ReaperExtension::Instance().GetConfig().recorder_coordination_enabled) {
            recorderDetail = @"Recorder coordination is off. Next step: turn Recorder Control on to prepare a WING recorder and optionally follow auto-trigger recordings.";
        } else if (!liveSetupValidated && !isConnected) {
            recorderDetail = @"Recorder coordination can be staged now. Next step: connect to the WING before applying recorder settings.";
        } else if (!liveSetupValidated) {
            recorderDetail = @"Recorder coordination is waiting for setup validation. Next step: finish the Reaper tab workflow first.";
        } else if (liveSetupValidated) {
            recorderDetail = @"Recorder coordination is aligned with the current setup and ready to be used.";
            recorderColor = [NSColor systemGreenColor];
        }
        [recorderDetailLabel setStringValue:recorderDetail];
        [recorderDetailLabel setTextColor:recorderColor];
    }

    if (recorderFollowHintLabel) {
        NSString* hint = @"";
        NSColor* hintColor = [NSColor secondaryLabelColor];
        if (!ReaperExtension::Instance().GetConfig().recorder_coordination_enabled) {
            hint = @"Enable Recorder Control first to let the Wing recorder follow auto-trigger recordings.";
        } else if (hasPendingSetupDraft || pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode) {
            hint = @"Apply the pending REAPER recorder setup first before enabling recorder follow.";
            hintColor = [NSColor systemOrangeColor];
        } else if (!liveSetupValidated) {
            hint = @"Follow Auto-Trigger becomes available after the REAPER recorder setup is configured and validated.";
        } else if (!ReaperExtension::Instance().GetConfig().auto_record_enabled) {
            hint = @"Enable and apply Auto Trigger first before letting the Wing recorder follow it.";
        } else if (automationSettingsDirty) {
            hint = @"Apply the pending Auto Trigger settings first before enabling recorder follow.";
            hintColor = [NSColor systemOrangeColor];
        }
        [recorderFollowHintLabel setStringValue:hint];
        [recorderFollowHintLabel setTextColor:hintColor];
        [recorderFollowHintLabel setHidden:([hint length] == 0)];
    }
}

- (void)updateRecorderStatusLabel {
    if (!recorderStatusLabel) {
        return;
    }
    auto& config = ReaperExtension::Instance().GetConfig();
    NSString* text = @"Wing Recorder: Disabled";
    NSString* symbol = @"record.circle";
    NSString* fallback = NSImageNameStatusNone;
    NSColor* iconColor = [NSColor secondaryLabelColor];
    if (recorderSettingsDirty) {
        if (config.recorder_coordination_enabled) {
            text = config.sd_auto_record_with_reaper
                ? @"Wing Recorder: Pending Apply + Autostart"
                : @"Wing Recorder: Pending Apply";
        } else {
            text = @"Wing Recorder: Pending Disable";
        }
        symbol = @"arrow.triangle.2.circlepath.circle.fill";
        fallback = NSImageNameRefreshTemplate;
        iconColor = [NSColor systemOrangeColor];
    } else if (config.recorder_coordination_enabled && config.sd_auto_record_with_reaper && automationSettingsDirty) {
        text = @"Wing Recorder: Enabled + Pending Trigger Apply";
        symbol = @"arrow.triangle.2.circlepath.circle.fill";
        fallback = NSImageNameRefreshTemplate;
        iconColor = [NSColor systemOrangeColor];
    } else if (config.recorder_coordination_enabled) {
        if (config.sd_auto_record_with_reaper && config.auto_record_enabled) {
            text = @"Wing Recorder: Enabled + Autostart";
            symbol = @"play.circle.fill";
            fallback = NSImageNameStatusAvailable;
            iconColor = [NSColor systemGreenColor];
        } else if (config.sd_auto_record_with_reaper) {
            text = @"Wing Recorder: Enabled";
            symbol = @"checkmark.circle.fill";
            fallback = NSImageNameCaution;
            iconColor = [NSColor systemOrangeColor];
        } else {
            text = @"Wing Recorder: Enabled";
            symbol = @"record.circle.fill";
            fallback = NSImageNameStatusAvailable;
            iconColor = [NSColor systemGreenColor];
        }
    }
    [recorderStatusLabel setStringValue:text];
    [recorderStatusLabel setTextColor:[NSColor labelColor]];
    [self setHeaderStatusIcon:recorderStatusIconView symbolName:symbol fallback:fallback color:iconColor];
    [self updateTabStatusIndicators];
}

- (void)updateMidiStatusLabel {
    if (!midiStatusLabel) {
        return;
    }
    NSString* text = @"Wing control integration: Disabled";
    NSString* symbol = @"switch.2";
    NSString* fallback = NSImageNameStatusNone;
    NSColor* iconColor = [NSColor secondaryLabelColor];
    if (midiActionsDirty) {
        text = pendingMidiActionsEnabled ? @"Wing control integration: Pending Enable" : @"Wing control integration: Pending Disable";
        symbol = @"arrow.triangle.2.circlepath.circle.fill";
        fallback = NSImageNameRefreshTemplate;
        iconColor = [NSColor systemOrangeColor];
    } else if (ReaperExtension::Instance().IsMidiActionsEnabled()) {
        text = (latestMidiValidationState == ValidationState::Warning)
            ? @"Wing control integration: Enabled With Warning"
            : @"Wing control integration: Enabled";
        symbol = (latestMidiValidationState == ValidationState::Warning)
            ? @"exclamationmark.shield"
            : @"switch.2";
        fallback = (latestMidiValidationState == ValidationState::Warning)
            ? NSImageNameCaution
            : NSImageNameStatusAvailable;
        iconColor = (latestMidiValidationState == ValidationState::Warning)
            ? [NSColor systemOrangeColor]
            : [NSColor systemGreenColor];
    }
    [midiStatusLabel setStringValue:text];
    [midiStatusLabel setTextColor:[NSColor labelColor]];
    [self setHeaderStatusIcon:midiStatusIconView symbolName:symbol fallback:fallback color:iconColor];
    [self updateTabStatusIndicators];
}

- (void)clearPendingSetupDraft:(BOOL)resetMode {
    hasPendingSetupDraft = NO;
    pendingSetupUsesExistingSelection = NO;
    pendingSetupChannels.clear();
    pendingSetupSoundcheck = YES;
    pendingReplaceExisting = YES;
    pendingOutputMode = ReaperExtension::Instance().GetConfig().soundcheck_output_mode;
    if (resetMode && outputModeControl) {
        [outputModeControl setSelectedSegment:(pendingOutputMode == "CARD") ? 1 : 0];
    }
    [self updatePendingSetupUI];
    [self updateSetupReadinessDetails];
    [self updateValidationStatusLabel];
    [self updateAutomationDetails];
}

- (void)refreshMonitorTrackDropdown {
    auto& config = ReaperExtension::Instance().GetConfig();
    [monitorTrackDropdown removeAllItems];

    [monitorTrackDropdown addItemWithTitle:@"Auto (Armed+Monitored)"];
    [[monitorTrackDropdown itemAtIndex:0] setTag:0];

    const int track_count = ReaperExtension::Instance().GetProjectTrackCount();
    for (int i = 1; i <= track_count; ++i) {
        NSString* title = [NSString stringWithFormat:@"Track %d", i];
        [monitorTrackDropdown addItemWithTitle:title];
        [[monitorTrackDropdown itemAtIndex:i] setTag:i];
    }

    int wanted = std::max(0, config.auto_record_monitor_track);
    if (wanted > track_count) {
        wanted = 0;
        config.auto_record_monitor_track = 0;
    }
    [monitorTrackDropdown selectItemAtIndex:wanted];
}

- (void)onMonitorTrackChanged:(id)sender {
    (void)sender;
    auto& config = ReaperExtension::Instance().GetConfig();
    NSMenuItem* item = [monitorTrackDropdown selectedItem];
    config.auto_record_monitor_track = item ? (int)[item tag] : 0;
    [self onAutoRecordSettingsChanged:sender];
}

- (void)updateToggleSoundcheckButtonLabel {
    auto& extension = ReaperExtension::Instance();
    bool enabled = extension.IsSoundcheckModeEnabled();
    const bool pending_apply = hasPendingSetupDraft || pendingOutputMode != extension.GetConfig().soundcheck_output_mode;
    if (enabled) {
        [toggleSoundcheckButton setTitle:@"🎧 Soundcheck Mode"];
    } else {
        [toggleSoundcheckButton setTitle:@"🎙️ Live Mode"];
    }

    // Only enable when live setup validates against Wing + REAPER state.
    if (liveSetupValidated && !pending_apply && !isWorking) {
        [toggleSoundcheckButton setEnabled:YES];
    } else {
        [toggleSoundcheckButton setEnabled:NO];
    }
}

- (void)updateSetupSoundcheckButtonLabel {
    auto& extension = ReaperExtension::Instance();
    const bool pending_apply = hasPendingSetupDraft || pendingOutputMode != extension.GetConfig().soundcheck_output_mode;
    if (pending_apply) {
        [setupSoundcheckButton setTitle:@"Edit Pending Sources…"];
    } else {
        [setupSoundcheckButton setTitle:@"Choose Sources…"];
    }
    [self updatePendingSetupUI];
}

- (void)updateAutoTriggerControlsEnabled {
    auto& extension = ReaperExtension::Instance();
    const bool pending_apply = hasPendingSetupDraft || pendingOutputMode != extension.GetConfig().soundcheck_output_mode;
    const BOOL liveSetupControlsEnabled = (liveSetupValidated && !pending_apply && !isWorking) ? YES : NO;
    const BOOL sdControlsEnabled = isWorking ? NO : YES;
    const BOOL midiToggleEnabled = (!isWorking && (liveSetupValidated || extension.IsMidiActionsEnabled() || midiActionsDirty)) ? YES : NO;
    const BOOL autoTriggerEnabled = (liveSetupControlsEnabled && extension.GetConfig().auto_record_enabled) ? YES : NO;
    const BOOL recorderControlsEnabled = (sdControlsEnabled && extension.GetConfig().recorder_coordination_enabled) ? YES : NO;
    const BOOL recorderFollowEnabled = (recorderControlsEnabled &&
                                        autoTriggerEnabled &&
                                        !automationSettingsDirty) ? YES : NO;
    [midiActionsControl setEnabled:midiToggleEnabled];
    [monitorTrackDropdown setEnabled:autoTriggerEnabled];
    [autoRecordEnableControl setEnabled:liveSetupControlsEnabled];
    [autoRecordModeControl setEnabled:autoTriggerEnabled];
    [thresholdField setEnabled:autoTriggerEnabled];
    [holdField setEnabled:autoTriggerEnabled];
    [applyAutomationButton setEnabled:(!isWorking && liveSetupControlsEnabled && automationSettingsDirty) ? YES : NO];
    [recorderTargetControl setEnabled:recorderControlsEnabled];
    [recorderEnableControl setEnabled:sdControlsEnabled];
    [recorderFollowControl setEnabled:recorderFollowEnabled];
    [sdSourceDropdown setEnabled:recorderControlsEnabled];
    [applyRecorderButton setEnabled:(!isWorking && recorderSettingsDirty) ? YES : NO];
    [self updateMidiActionsUI];
}

- (void)refreshLiveSetupValidation {
    // Never block the UI thread with network/OSC queries.
    auto& extension = ReaperExtension::Instance();
    if (validationInProgress || isWorking || hasPendingSetupDraft ||
        pendingOutputMode != extension.GetConfig().soundcheck_output_mode) {
        return;
    }
    auto& config = extension.GetConfig();

    std::string candidate_ip;
    NSString* selectedIP = [self selectedOrManualWingIP];
    if (selectedIP && [selectedIP length] > 0) {
        candidate_ip = std::string([selectedIP UTF8String]);
    } else if (!config.wing_ip.empty()) {
        candidate_ip = config.wing_ip;
    }

    if (!extension.IsConnected() && candidate_ip.empty()) {
        liveSetupValidated = NO;
        latestLiveSetupValidationState = ValidationState::NotReady;
        latestLiveSetupValidationDetails = "No Wing IP available. Connect or select a console before validating recording setup.";
        [self updateValidationStatusLabel];
        [self updateSetupReadinessDetails];
        [self updateToggleSoundcheckButtonLabel];
        [self updateSetupSoundcheckButtonLabel];
        [self updateAutoTriggerControlsEnabled];
        [self updateAutomationDetails];
        [self appendToLog:@"Live setup validation: NOT READY — no Wing IP available to validate against.\n"];
        return;
    }

    validationInProgress = YES;
    [self updateValidationStatusLabel];
    WingConnectorWindowController* blockSelf = self;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        bool connected_now = extension.IsConnected();
        std::string details;

        // Only validate against Wing when already connected. Explicit connect/disconnect
        // is controlled by the UI button to keep behavior predictable.
        if (!connected_now) {
            config.wing_ip = candidate_ip;
            details = "Not connected to Wing. Connect first to validate setup.";
            dispatch_async(dispatch_get_main_queue(), ^{
                blockSelf->validationInProgress = NO;
                blockSelf->isConnected = NO;
                [blockSelf setConnectionStatusText:@"Console: Not Connected" color:[NSColor secondaryLabelColor] connected:NO];
                blockSelf->liveSetupValidated = NO;
                blockSelf->latestLiveSetupValidationState = ValidationState::NotReady;
                blockSelf->latestLiveSetupValidationDetails = details;
                [blockSelf updateValidationStatusLabel];
                [blockSelf updateSetupReadinessDetails];
                [blockSelf updateToggleSoundcheckButtonLabel];
                [blockSelf updateSetupSoundcheckButtonLabel];
                [blockSelf updateAutoTriggerControlsEnabled];
                [blockSelf updateAutomationDetails];
                [blockSelf appendToLog:[NSString stringWithFormat:@"Live setup validation: NOT READY — %s\n",
                                        details.c_str()]];
                });
            return;
        }

        ValidationState state = extension.ValidateLiveRecordingSetup(details);
        dispatch_async(dispatch_get_main_queue(), ^{
            blockSelf->validationInProgress = NO;
            // Avoid changing state while another operation is in progress.
            if (blockSelf->isWorking) {
                return;
            }
            blockSelf->isConnected = extension.IsConnected() ? YES : NO;
            [blockSelf setConnectionStatusText:(blockSelf->isConnected ? @"Console: Connected" : @"Console: Not Connected")
                                         color:(blockSelf->isConnected ? [NSColor systemGreenColor] : [NSColor secondaryLabelColor])
                                      connected:blockSelf->isConnected];
            blockSelf->liveSetupValidated = (state == ValidationState::Ready) ? YES : NO;
            blockSelf->latestLiveSetupValidationState = state;
            blockSelf->latestLiveSetupValidationDetails = details;
            [blockSelf updateValidationStatusLabel];
            [blockSelf updateSetupReadinessDetails];
            [blockSelf updateToggleSoundcheckButtonLabel];
            [blockSelf updateSetupSoundcheckButtonLabel];
            [blockSelf updateAutoTriggerControlsEnabled];
            [blockSelf updateAutomationDetails];
            [blockSelf appendToLog:[NSString stringWithFormat:@"Live setup validation: %s — %s\n",
                                    (state == ValidationState::Ready) ? "READY" :
                                    (state == ValidationState::Warning) ? "WARNING" : "NOT READY",
                                    details.c_str()]];
        });
    });
}

- (void)setWorkingState:(BOOL)working {
    isWorking = working;
    [setupSoundcheckButton setEnabled:!working];
    [scanButton setEnabled:!working];
    if (!working && scanButton) {
        [scanButton setTitle:@"Scan"];
    }
    [connectButton setEnabled:!working];
    [self updatePendingSetupUI];
    // Toggle button state depends on both working state and setup completion
    [self updateToggleSoundcheckButtonLabel];
    [self updateSetupSoundcheckButtonLabel];
    [self updateAutoTriggerControlsEnabled];
}

- (void)appendToLog:(NSString*)message {
    if (!message) {
        return;
    }
    NSString* cleaned = [message stringByReplacingOccurrencesOfString:@"AUDIOLAB.wing.reaper.virtualsoundcheck: "
                                                           withString:@""];
    cleaned = [cleaned stringByReplacingOccurrencesOfString:@"AUDIOLAB.wing.reaper.virtualsoundcheck:"
                                                 withString:@""];
    cleaned = [cleaned stringByReplacingOccurrencesOfString:@"WINGuard: "
                                                 withString:@""];
    cleaned = [cleaned stringByReplacingOccurrencesOfString:@"WINGuard:"
                                                 withString:@""];
    NSTextStorage* textStorage = [activityLogView textStorage];
    NSFont* font = [activityLogView font];
    if (!font) {
        font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    }
    NSColor* color = [activityLogView textColor];
    if (!color) {
        color = [NSColor labelColor];
    }
    NSDictionary* attributes = @{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: color
    };
    NSAttributedString* addition = [[[NSAttributedString alloc] initWithString:cleaned
                                                                    attributes:attributes] autorelease];
    [textStorage appendAttributedString:addition];

    // Bound long-running sessions while keeping append cost independent of
    // the amount of log history already displayed.
    constexpr NSUInteger kMaxLogCharacters = 32000;
    if ([textStorage length] > kMaxLogCharacters) {
        const NSUInteger overflow = [textStorage length] - kMaxLogCharacters;
        const NSRange trimRange = [[textStorage string]
            rangeOfComposedCharacterSequencesForRange:NSMakeRange(0, overflow)];
        [textStorage deleteCharactersInRange:trimRange];
    }

    // Scroll to bottom
    NSRange range = NSMakeRange([textStorage length], 0);
    [activityLogView scrollRangeToVisible:range];
}

// ===== WING DISCOVERY =====

- (void)startDiscoveryScan {
    [wingDropdown removeAllItems];
    [wingDropdown addItemWithTitle:@"Scanning..."];
    [[wingDropdown itemAtIndex:0] setEnabled:NO];
    [wingDropdown setEnabled:NO];
    [scanButton setTitle:@"Scanning..."];
    [scanButton setEnabled:NO];

    WingConnectorWindowController* blockSelf = self;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        auto wings = ReaperExtension::Instance().DiscoverWings(1500);
        NSMutableArray* items = [NSMutableArray array];
        NSMutableArray* ips   = [NSMutableArray array];
        for (const auto& w : wings) {
            NSString* label;
            if (!w.name.empty() && !w.model.empty()) {
                label = [NSString stringWithFormat:@"%s \u2013 %s  (%s)",
                         w.name.c_str(), w.model.c_str(), w.console_ip.c_str()];
            } else if (!w.name.empty()) {
                label = [NSString stringWithFormat:@"%s  (%s)",
                         w.name.c_str(), w.console_ip.c_str()];
            } else if (!w.model.empty()) {
                label = [NSString stringWithFormat:@"%s  (%s)",
                         w.model.c_str(), w.console_ip.c_str()];
            } else {
                label = [NSString stringWithUTF8String:w.console_ip.c_str()];
            }
            [items addObject:label];
            [ips   addObject:[NSString stringWithUTF8String:w.console_ip.c_str()]];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [blockSelf populateDropdownWithItems:items ips:ips];
            // Only re-enable scan button if no other operation is running
            if (!blockSelf->isWorking) {
                [blockSelf->scanButton setTitle:@"Scan"];
                [blockSelf->scanButton setEnabled:YES];
            }
        });
    });
}

- (void)populateDropdownWithItems:(NSArray*)items ips:(NSArray*)ips {
    [discoveredIPs release];
    discoveredIPs = [[NSMutableArray arrayWithArray:ips] retain];
    [wingDropdown removeAllItems];
    if ([items count] == 0) {
        [wingDropdown addItemWithTitle:@"No Wings found — press Scan"];
        [[wingDropdown itemAtIndex:0] setEnabled:NO];
        [wingDropdown setEnabled:NO];
        [self appendToLog:@"\u2717 No Wing consoles found on the network. Is the Wing powered on and reachable?\n"];
    } else {
        [wingDropdown setEnabled:YES];
        for (NSString* title in items) {
            [wingDropdown addItemWithTitle:title];
        }
        [wingDropdown selectItemAtIndex:0];
        // Immediately apply the first found Wing IP to config
        auto& config = ReaperExtension::Instance().GetConfig();
        config.wing_ip = std::string([[ips objectAtIndex:0] UTF8String]);
        [manualIPField setStringValue:[ips objectAtIndex:0]];
        [self appendToLog:[NSString stringWithFormat:@"Found %d Wing console(s):\n", (int)[items count]]];
        for (NSString* title in items) {
            [self appendToLog:[NSString stringWithFormat:@"  \u2022 %@\n", title]];
        }
        const BOOL shouldAutoConnect = ([items count] == 1 &&
                                        !ReaperExtension::Instance().IsConnected() &&
                                        !isWorking &&
                                        !validationInProgress);
        if (shouldAutoConnect) {
            [self appendToLog:@"Single Wing detected. Connecting automatically...\n"];
            [self onConnectClicked:nil];
        } else {
            [self refreshLiveSetupValidation];
        }
    }
}

- (void)onWingDropdownChanged:(id)sender {
    NSInteger idx = [wingDropdown indexOfSelectedItem];
    if (discoveredIPs && idx >= 0 && idx < (NSInteger)[discoveredIPs count]) {
        auto& config = ReaperExtension::Instance().GetConfig();
        config.wing_ip = std::string([[discoveredIPs objectAtIndex:idx] UTF8String]);
        [manualIPField setStringValue:[discoveredIPs objectAtIndex:idx]];
        [self appendToLog:[NSString stringWithFormat:@"Selected Wing: %@\n",
                          [wingDropdown titleOfSelectedItem]]];
    }
}

- (void)onScanClicked:(id)sender {
    [self appendToLog:@"\n=== Re-scanning for Wing consoles ===\n"];
    [self startDiscoveryScan];
}

- (void)onConnectClicked:(id)sender {
    if (isWorking || validationInProgress) return;

    WingConnectorWindowController* blockSelf = self;
    [self setWorkingState:YES];

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        auto& extension = ReaperExtension::Instance();
        auto& config = extension.GetConfig();

        if (extension.IsConnected()) {
            extension.DisconnectFromWing();
            dispatch_async(dispatch_get_main_queue(), ^{
                blockSelf->liveSetupValidated = NO;
                [blockSelf appendToLog:@"Disconnected from Wing.\n"];
                [blockSelf setWorkingState:NO];
                [blockSelf updateConnectionStatus];
            });
            return;
        }

        NSString* wingIP = [blockSelf selectedOrManualWingIP];
        if (wingIP && [wingIP length] > 0) {
            config.wing_ip = std::string([wingIP UTF8String]);
        }

        if (config.wing_ip.empty()) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"✗ No Wing selected. Press Scan and choose a console first.\n"];
                [blockSelf setWorkingState:NO];
                [blockSelf updateConnectionStatus];
            });
            return;
        }

        if (!extension.ConnectToWing()) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:[NSString stringWithFormat:@"✗ Connection failed to %s.\n",
                                        config.wing_ip.c_str()]];
                [blockSelf setWorkingState:NO];
                [blockSelf updateConnectionStatus];
            });
            return;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [blockSelf appendToLog:[NSString stringWithFormat:@"✓ Connected to %s\n",
                                    config.wing_ip.c_str()]];
            [blockSelf setWorkingState:NO];
            [blockSelf updateConnectionStatus];
        });
    });
}

- (NSString*)selectedWingIP {
    if (!wingDropdown || !discoveredIPs) {
        return nil;
    }
    NSInteger idx = [wingDropdown indexOfSelectedItem];
    if (idx >= 0 && idx < (NSInteger)[discoveredIPs count]) {
        return [discoveredIPs objectAtIndex:idx];
    }
    return nil;
}

- (NSString*)selectedOrManualWingIP {
    NSString* selected = [self selectedWingIP];
    if (selected && [selected length] > 0) {
        return selected;
    }
    if (manualIPField) {
        NSString* typed = [[manualIPField stringValue]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if ([typed length] > 0) {
            return typed;
        }
    }
    return nil;
}

- (void)onManualIPChanged:(id)sender {
    (void)sender;
    if (!manualIPField) {
        return;
    }
    NSString* typed = [[manualIPField stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    auto& config = ReaperExtension::Instance().GetConfig();
    config.wing_ip = ([typed length] > 0) ? std::string([typed UTF8String]) : std::string();
    if ([typed length] > 0) {
        [self appendToLog:[NSString stringWithFormat:@"Manual Wing IP set to %@\n", typed]];
    }
    [self persistConfigAndLog:nil];
    [self refreshLiveSetupValidation];
}

- (void)onSetupSoundcheckClicked:(id)sender {
    if (isWorking) return;  // Prevent re-entrant clicks
    pendingOutputMode = ([outputModeControl selectedSegment] == 0) ? "USB" : "CARD";
    [self appendToLog:[NSString stringWithFormat:@"\n=== Choosing sources for staged live setup (%s mode) ===\n",
                      pendingOutputMode.c_str()]];
    [self setWorkingState:YES];
    [self runSetupSoundcheckFlow];
}

- (void)onApplyPendingSetupClicked:(id)sender {
    if (isWorking) return;
    if (!(hasPendingSetupDraft || pendingOutputMode != ReaperExtension::Instance().GetConfig().soundcheck_output_mode)) {
        [self appendToLog:@"No pending live setup changes to apply.\n"];
        return;
    }

    [self appendToLog:[NSString stringWithFormat:@"\n=== Applying staged live setup (%s mode) ===\n",
                      pendingOutputMode.c_str()]];
    [self setWorkingState:YES];
    [self runApplyPendingSetupFlow];
}

- (void)onDiscardPendingSetupClicked:(id)sender {
    if (isWorking) return;
    [self clearPendingSetupDraft:YES];
    [self appendToLog:@"Discarded pending live setup changes.\n"];
    [self refreshLiveSetupValidation];
}

- (void)onToggleSoundcheckClicked:(id)sender {
    if (isWorking) return;  // Prevent re-entrant clicks
    [self appendToLog:@"\n=== Toggling Soundcheck Mode ===\n"];
    [self setWorkingState:YES];
    [self runToggleSoundcheckModeFlow];
}

- (void)onOutputModeChanged:(id)sender {
    auto& extension = ReaperExtension::Instance();
    pendingOutputMode = ([outputModeControl selectedSegment] == 0) ? "USB" : "CARD";

    std::string details;
    bool fullyAvailable = extension.CheckOutputModeAvailability(pendingOutputMode, details);
    [self appendToLog:[NSString stringWithFormat:@"Output mode staged: %s\n", pendingOutputMode.c_str()]];
    [self appendToLog:[NSString stringWithFormat:@"%s\n", details.c_str()]];

    if (!fullyAvailable) {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Selected mode may not be fully available"];
        [alert setInformativeText:[NSString stringWithUTF8String:details.c_str()]];
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
    }

    liveSetupValidated = NO;
    hasPendingSetupDraft = YES;
    pendingSetupUsesExistingSelection = pendingSetupChannels.empty() ? YES : NO;
    latestLiveSetupValidationState = ValidationState::Warning;
    latestLiveSetupValidationDetails = "Pending setup changes are staged and must be applied before recording readiness can be confirmed for the current managed setup.";
    [self updateValidationStatusLabel];
    [self updatePendingSetupUI];
    [self updateSetupReadinessDetails];
    [self updateSetupSoundcheckButtonLabel];
    [self updateToggleSoundcheckButtonLabel];
    [self updateAutoTriggerControlsEnabled];
    [self updateAutomationDetails];
    [self appendToLog:@"Recording I/O mode change staged. Review the pending setup and rebuild the current managed setup when ready.\n"];

    if (pendingSetupChannels.empty() && extension.IsConnected()) {
        WingConnectorWindowController* blockSelf = self;
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            auto loaded_sources = extension.GetAvailableSources();
            int selectedCount = 0;
            for (const auto& source : loaded_sources) {
                if (source.selected) {
                    selectedCount++;
                }
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                if (blockSelf->pendingOutputMode != (([blockSelf->outputModeControl selectedSegment] == 0) ? "USB" : "CARD")) {
                    return;
                }
                if (!loaded_sources.empty() && selectedCount > 0) {
                    blockSelf->pendingSetupChannels = loaded_sources;
                    blockSelf->pendingSetupUsesExistingSelection = NO;
                    [blockSelf appendToLog:[NSString stringWithFormat:@"Loaded %d currently selected sources into the pending draft for %s mode.\n",
                                           selectedCount,
                                           blockSelf->pendingOutputMode.c_str()]];
                    [blockSelf updatePendingSetupUI];
                    [blockSelf updateSetupReadinessDetails];
                }
            });
        });
    }
}

- (void)onMidiActionsToggled:(id)sender {
    const BOOL wantsEnabled = ([midiActionsControl selectedSegment] == 1);
    if (wantsEnabled && !liveSetupValidated) {
        [midiActionsControl setSelectedSegment:ReaperExtension::Instance().IsMidiActionsEnabled() ? 1 : 0];
        [self appendToLog:@"Configure live setup first before enabling MIDI shortcuts.\n"];
        return;
    }

    pendingMidiActionsEnabled = wantsEnabled;
    midiActionsDirty = (pendingMidiActionsEnabled != ReaperExtension::Instance().IsMidiActionsEnabled());
    latestMidiValidationState = pendingMidiActionsEnabled ? ValidationState::Warning : ValidationState::NotReady;
    latestMidiValidationDetails = pendingMidiActionsEnabled
        ? "Pending MIDI shortcut enablement has not been applied to the WING yet."
        : "Pending MIDI shortcut disablement has not been applied yet.";
    [self appendToLog:[NSString stringWithFormat:@"MIDI shortcut change staged: %s. Click Apply MIDI Shortcuts to commit it.\n",
                       pendingMidiActionsEnabled ? "ON" : "OFF"]];
    [self updateMidiActionsUI];
}

- (void)onApplyMidiActionsClicked:(id)sender {
    (void)sender;
    if (isWorking || !midiActionsDirty) {
        return;
    }
    if (pendingMidiActionsEnabled && !liveSetupValidated) {
        [self appendToLog:@"Validate live setup before applying MIDI shortcut changes.\n"];
        [self updateMidiActionsUI];
        return;
    }

    auto& extension = ReaperExtension::Instance();
    extension.EnableMidiActions(pendingMidiActionsEnabled);
    midiActionsDirty = NO;
    pendingMidiActionsEnabled = extension.IsMidiActionsEnabled();

    if (pendingMidiActionsEnabled) {
        [self appendToLog:@"✓ MIDI actions enabled - Wing buttons now control REAPER\n"];
        std::string details;
        ValidationState state = extension.ValidateMidiActionSetup(details);
        latestMidiValidationState = state;
        latestMidiValidationDetails = details;
        [self appendToLog:[NSString stringWithFormat:@"MIDI shortcut validation: %s — %s\n",
                           (state == ValidationState::Ready) ? "READY" :
                           (state == ValidationState::Warning) ? "WARNING" : "NOT READY",
                           details.c_str()]];
    } else {
        latestMidiValidationState = ValidationState::NotReady;
        latestMidiValidationDetails = "MIDI shortcuts are disabled.";
        [self appendToLog:@"MIDI actions disabled\n"];
    }
    [self updateMidiActionsUI];
    [self persistConfigAndLog:@"Saved MIDI action setting.\n"];
}

- (void)onDiscardMidiActionsClicked:(id)sender {
    (void)sender;
    midiActionsDirty = NO;
    pendingMidiActionsEnabled = ReaperExtension::Instance().IsMidiActionsEnabled();
    [midiActionsControl setSelectedSegment:pendingMidiActionsEnabled ? 1 : 0];
    if (!pendingMidiActionsEnabled) {
        latestMidiValidationState = ValidationState::NotReady;
        latestMidiValidationDetails = "MIDI shortcuts are disabled.";
    }
    [self appendToLog:@"Discarded pending MIDI shortcut changes.\n"];
    [self updateMidiActionsUI];
}

- (void)persistConfigAndLog:(NSString*)message {
    auto& config = ReaperExtension::Instance().GetConfig();
    const std::string path = WingConfig::GetConfigPath();
    if (config.SaveToFile(path)) {
        if (message) {
            [self appendToLog:message];
        }
    } else {
        [self appendToLog:@"⚠ Failed to save config.json\n"];
    }
}

- (void)refreshBridgeMidiOutputDropdown {
    if (!bridgeMidiOutputDropdown) {
        return;
    }
    [bridgeMidiOutputDropdown removeAllItems];
    [bridgeMidiOutputDropdown addItemWithTitle:@"Select MIDI output..."];
    [[bridgeMidiOutputDropdown itemAtIndex:0] setTag:-1];

    auto outputs = ReaperExtension::Instance().GetMidiOutputDevices();
    for (size_t i = 0; i < outputs.size(); ++i) {
        [bridgeMidiOutputDropdown addItemWithTitle:[NSString stringWithUTF8String:outputs[i].c_str()]];
        [[bridgeMidiOutputDropdown itemAtIndex:(NSInteger)i + 1] setTag:(NSInteger)i];
    }

    const int configured = ReaperExtension::Instance().GetConfig().bridge_midi_output_device;
    NSInteger matchIndex = 0;
    for (NSInteger i = 0; i < (NSInteger)[bridgeMidiOutputDropdown numberOfItems]; ++i) {
        if ([[bridgeMidiOutputDropdown itemAtIndex:i] tag] == configured) {
            matchIndex = i;
            break;
        }
    }
    [bridgeMidiOutputDropdown selectItemAtIndex:matchIndex];
}

- (NSString*)bridgeFamilyLabelForKind:(SourceKind)kind {
    switch (kind) {
        case SourceKind::Channel: return @"Channel";
        case SourceKind::Bus: return @"Bus";
        case SourceKind::Main: return @"Main";
        case SourceKind::Matrix: return @"Matrix";
    }
    return @"Channel";
}

- (NSInteger)bridgeSourceCountForKind:(SourceKind)kind {
    switch (kind) {
        case SourceKind::Channel: return 48;
        case SourceKind::Bus: return 16;
        case SourceKind::Main: return 4;
        case SourceKind::Matrix: return 8;
    }
    return 48;
}

- (void)refreshBridgeSourceNumberDropdown {
    if (!bridgeSourceNumberDropdown || !bridgeSourceKindDropdown) {
        return;
    }
    NSInteger previousTag = [[bridgeSourceNumberDropdown selectedItem] tag];
    if (previousTag <= 0) {
        previousTag = 1;
    }
    SourceKind kind = (SourceKind)[[bridgeSourceKindDropdown selectedItem] tag];
    const NSInteger count = [self bridgeSourceCountForKind:kind];
    [bridgeSourceNumberDropdown removeAllItems];
    for (NSInteger i = 1; i <= count; ++i) {
        [bridgeSourceNumberDropdown addItemWithTitle:[NSString stringWithFormat:@"%ld", (long)i]];
        [[bridgeSourceNumberDropdown itemAtIndex:i - 1] setTag:i];
    }
    NSInteger selectedIndex = std::max<NSInteger>(0, std::min(count - 1, previousTag - 1));
    [bridgeSourceNumberDropdown selectItemAtIndex:selectedIndex];
}

- (void)refreshBridgeMappingTable {
    if (bridgeMappingTableView) {
        [bridgeMappingTableView reloadData];
    }
}

- (void)resetBridgeMappingEditor {
    if (!bridgeSourceKindDropdown) {
        return;
    }
    [bridgeSourceKindDropdown selectItemAtIndex:0];
    [self refreshBridgeSourceNumberDropdown];
    [bridgeSourceNumberDropdown selectItemAtIndex:0];
    [bridgeMidiValueField setStringValue:@"0"];
    [bridgeMappingEnabledCheckbox setState:NSControlStateValueOn];
    [bridgeAddOrUpdateMappingButton setTitle:@"Add Mapping"];
    if (bridgeMappingTableView) {
        [bridgeMappingTableView deselectAll:nil];
    }
}

- (void)loadBridgeMappingSelectionIntoEditor {
    NSInteger row = bridgeMappingTableView ? [bridgeMappingTableView selectedRow] : -1;
    const auto& mappings = ReaperExtension::Instance().GetConfig().bridge_mappings;
    if (row < 0 || row >= (NSInteger)mappings.size()) {
        [self resetBridgeMappingEditor];
        return;
    }

    const auto& mapping = mappings[(size_t)row];
    for (NSInteger i = 0; i < (NSInteger)[bridgeSourceKindDropdown numberOfItems]; ++i) {
        if ([[bridgeSourceKindDropdown itemAtIndex:i] tag] == (NSInteger)mapping.kind) {
            [bridgeSourceKindDropdown selectItemAtIndex:i];
            break;
        }
    }
    [self refreshBridgeSourceNumberDropdown];
    NSInteger sourceIndex = std::max(0, mapping.source_number - 1);
    if (sourceIndex < (NSInteger)[bridgeSourceNumberDropdown numberOfItems]) {
        [bridgeSourceNumberDropdown selectItemAtIndex:sourceIndex];
    }
    [bridgeMidiValueField setStringValue:[NSString stringWithFormat:@"%d", mapping.midi_value]];
    [bridgeMappingEnabledCheckbox setState:mapping.enabled ? NSControlStateValueOn : NSControlStateValueOff];
    [bridgeAddOrUpdateMappingButton setTitle:@"Update Mapping"];
}

- (void)syncBridgeSettingsFromUI {
    auto& extension = ReaperExtension::Instance();
    auto& config = extension.GetConfig();
    config.bridge_enabled = ([bridgeEnableCheckbox state] == NSControlStateValueOn);
    NSMenuItem* outputItem = [bridgeMidiOutputDropdown selectedItem];
    config.bridge_midi_output_device = outputItem ? (int)[outputItem tag] : -1;
    switch ([bridgeMessageTypeControl selectedSegment]) {
        case 0: config.bridge_midi_message_type = "NOTE_ON"; break;
        case 1: config.bridge_midi_message_type = "NOTE_ON_OFF"; break;
        case 2: config.bridge_midi_message_type = "PROGRAM"; break;
        default: config.bridge_midi_message_type = "NOTE_ON_OFF"; break;
    }
    NSMenuItem* midiChannelItem = [bridgeMidiChannelDropdown selectedItem];
    config.bridge_midi_channel = midiChannelItem ? (int)[midiChannelItem tag] : 1;
}

- (void)refreshBridgeStatus {
    if (!bridgeStatusLabel) {
        return;
    }
    std::string status = ReaperExtension::Instance().GetBridgeStatusSummary();
    [bridgeStatusLabel setStringValue:[NSString stringWithUTF8String:status.c_str()]];
}

- (void)onBridgeSettingsChanged:(id)sender {
    (void)sender;
    [self syncBridgeSettingsFromUI];
    [self refreshBridgeStatus];
}

- (void)onBridgeSourceKindChanged:(id)sender {
    (void)sender;
    [self refreshBridgeSourceNumberDropdown];
    [self onBridgeSettingsChanged:nil];
}

- (void)onAddOrUpdateBridgeMappingClicked:(id)sender {
    (void)sender;
    auto& config = ReaperExtension::Instance().GetConfig();
    const SourceKind kind = (SourceKind)[[bridgeSourceKindDropdown selectedItem] tag];
    NSMenuItem* sourceItem = [bridgeSourceNumberDropdown selectedItem];
    const int sourceNumber = sourceItem ? (int)[sourceItem tag] : 1;
    const int midiValue = std::clamp((int)[[bridgeMidiValueField stringValue] intValue], 0, 127);
    const bool enabled = ([bridgeMappingEnabledCheckbox state] == NSControlStateValueOn);

    bool updated = false;
    for (auto& mapping : config.bridge_mappings) {
        if (mapping.kind == kind && mapping.source_number == sourceNumber) {
            mapping.midi_value = midiValue;
            mapping.enabled = enabled;
            updated = true;
            break;
        }
    }
    if (!updated) {
        BridgeMapping mapping;
        mapping.kind = kind;
        mapping.source_number = sourceNumber;
        mapping.midi_value = midiValue;
        mapping.enabled = enabled;
        config.bridge_mappings.push_back(mapping);
    }

    std::sort(config.bridge_mappings.begin(), config.bridge_mappings.end(), [](const BridgeMapping& lhs, const BridgeMapping& rhs) {
        if (lhs.kind != rhs.kind) {
            return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
        }
        return lhs.source_number < rhs.source_number;
    });
    [self refreshBridgeMappingTable];
    [self onBridgeSettingsChanged:nil];
}

- (void)onRemoveBridgeMappingClicked:(id)sender {
    (void)sender;
    NSInteger row = bridgeMappingTableView ? [bridgeMappingTableView selectedRow] : -1;
    auto& config = ReaperExtension::Instance().GetConfig();
    if (row < 0 || row >= (NSInteger)config.bridge_mappings.size()) {
        return;
    }
    config.bridge_mappings.erase(config.bridge_mappings.begin() + row);
    [self refreshBridgeMappingTable];
    [self resetBridgeMappingEditor];
    [self onBridgeSettingsChanged:nil];
}

- (void)onApplyBridgeSettingsClicked:(id)sender {
    (void)sender;
    auto& extension = ReaperExtension::Instance();
    [self syncBridgeSettingsFromUI];
    extension.ApplyBridgeSettings();
    [self persistConfigAndLog:@"Saved bridge settings.\n"];
    [self appendToLog:[NSString stringWithFormat:@"Bridge settings applied: enabled=%s, output=%ld, type=%s, mappings=%lu\n",
                       extension.GetConfig().bridge_enabled ? "ON" : "OFF",
                       (long)extension.GetConfig().bridge_midi_output_device,
                       extension.GetConfig().bridge_midi_message_type.c_str(),
                       (unsigned long)extension.GetConfig().bridge_mappings.size()]];
    [self refreshBridgeStatus];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    if (tableView != bridgeMappingTableView) {
        return 0;
    }
    return (NSInteger)ReaperExtension::Instance().GetConfig().bridge_mappings.size();
}

- (id)tableView:(NSTableView*)tableView objectValueForTableColumn:(NSTableColumn*)tableColumn row:(NSInteger)row {
    if (tableView != bridgeMappingTableView) {
        return @"";
    }
    const auto& mappings = ReaperExtension::Instance().GetConfig().bridge_mappings;
    if (row < 0 || row >= (NSInteger)mappings.size()) {
        return @"";
    }
    const auto& mapping = mappings[(size_t)row];
    NSString* identifier = [tableColumn identifier];
    if ([identifier isEqualToString:@"source"]) {
        return [NSString stringWithFormat:@"%@ %d", [self bridgeFamilyLabelForKind:mapping.kind], mapping.source_number];
    }
    if ([identifier isEqualToString:@"midi"]) {
        return [NSString stringWithFormat:@"%d", mapping.midi_value];
    }
    if ([identifier isEqualToString:@"enabled"]) {
        return mapping.enabled ? @"Enabled" : @"Disabled";
    }
    return [NSString stringWithFormat:@"%@ %d", [self bridgeFamilyLabelForKind:mapping.kind], mapping.source_number];
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    if ([notification object] == bridgeMappingTableView) {
        [self loadBridgeMappingSelectionIntoEditor];
    }
}

- (void)selectTabWithIdentifier:(NSString*)identifier {
    if (!settingsTabView || !identifier) {
        return;
    }
    if (!kShowBridgeTabInMainUI && [identifier isEqualToString:@"bridge"]) {
        identifier = @"control-integration";
    }
    for (NSTabViewItem* item in [settingsTabView tabViewItems]) {
        if ([[item identifier] isKindOfClass:[NSString class]] &&
            [(NSString*)[item identifier] isEqualToString:identifier]) {
            [settingsTabView selectTabViewItem:item];
            return;
        }
    }
}

- (void)onDebugLogToggled:(id)sender {
    (void)sender;
    if (!debugLogWindow) {
        [self createDebugLogWindow];
    }
    [debugLogWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    [self updateFormLayoutForCurrentWindowSize];
}

- (void)finalizeFormLayout {
    if (!formContentView) {
        return;
    }

    CGFloat minY = CGFLOAT_MAX;
    CGFloat maxY = 0.0;
    for (NSView* subview in [formContentView subviews]) {
        if ([subview isHidden]) {
            continue;
        }
        const NSRect frame = [subview frame];
        minY = std::min(minY, NSMinY(frame));
        maxY = std::max(maxY, NSMaxY(frame));
    }

    if (minY == CGFLOAT_MAX) {
        expandedContentHeight = collapsedContentHeight;
        [self updateFormLayoutForCurrentWindowSize];
        return;
    }

    const CGFloat desiredBottomPadding = 24.0;
    if (minY < desiredBottomPadding) {
        const CGFloat shift = desiredBottomPadding - minY;
        for (NSView* subview in [formContentView subviews]) {
            NSRect frame = [subview frame];
            frame.origin.y += shift;
            [subview setFrame:frame];
        }
        maxY += shift;
    }

    const CGFloat desiredTopPadding = 20.0;
    expandedContentHeight = std::max(collapsedContentHeight, maxY + desiredTopPadding);
    [self adjustWindowHeightToFitContent];
    [self updateFormLayoutForCurrentWindowSize];
}

- (void)adjustWindowHeightToFitContent {
    NSWindow* window = [self window];
    if (!window) {
        return;
    }

    NSRect currentFrame = [window frame];
    NSRect currentContentRect = [window contentRectForFrameRect:currentFrame];
    const CGFloat desiredContentHeight = expandedContentHeight;
    if (currentContentRect.size.height >= desiredContentHeight) {
        return;
    }

    NSScreen* screen = [window screen];
    if (!screen) {
        screen = [NSScreen mainScreen];
    }
    if (!screen) {
        return;
    }

    const NSRect visibleFrame = [screen visibleFrame];
    NSRect maxContentRect = [window contentRectForFrameRect:visibleFrame];
    const CGFloat maxContentHeight = maxContentRect.size.height;
    const CGFloat targetContentHeight = std::min(desiredContentHeight, maxContentHeight);
    if (targetContentHeight <= currentContentRect.size.height) {
        return;
    }

    const CGFloat delta = targetContentHeight - currentContentRect.size.height;
    currentFrame.origin.y -= delta;
    currentFrame.size.height += delta;
    [window setFrame:currentFrame display:NO];
}

- (void)updateFormLayoutForCurrentWindowSize {
    if (!mainScrollView || !formContentView) {
        return;
    }
    NSSize clipSize = [[mainScrollView contentView] bounds].size;
    const CGFloat targetHeight = std::max(expandedContentHeight, clipSize.height);
    [formContentView setFrame:NSMakeRect(0, 0, std::max(clipSize.width, (CGFloat)820.0), targetHeight)];
    NSClipView* clipView = [mainScrollView contentView];
    CGFloat topOriginY = std::max(0.0, targetHeight - NSHeight([clipView bounds]));
    [clipView scrollToPoint:NSMakePoint(0, topOriginY)];
    [mainScrollView reflectScrolledClipView:clipView];
}

- (void)createDebugLogWindow {
    if (debugLogWindow) {
        return;
    }
    debugLogWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 700, 360)
                                                 styleMask:(NSWindowStyleMaskTitled |
                                                           NSWindowStyleMaskClosable |
                                                           NSWindowStyleMaskMiniaturizable |
                                                           NSWindowStyleMaskResizable)
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
    [debugLogWindow setTitle:@"WINGuard Debug Log"];
    [debugLogWindow setMinSize:NSMakeSize(520, 220)];

    NSView* logContentView = [debugLogWindow contentView];
    [logScrollView setFrame:[logContentView bounds]];
    [logScrollView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [logContentView addSubview:logScrollView];
}

- (void)onAutoRecordSettingsChanged:(id)sender {
    auto& extension = ReaperExtension::Instance();
    auto& config = extension.GetConfig();
    (void)sender;
    [self syncPendingAutomationSettingsFromUI];
    extension.PauseAutoRecordForSetup();
    automationSettingsDirty = YES;
    [self updateValidationStatusLabel];
    [self updateAutoTriggerControlsEnabled];
    [self updateAutomationDetails];
    [self updateRecorderStatusLabel];
    [self appendToLog:[NSString stringWithFormat:@"Auto Trigger settings pending: %s, mode=%s, source=REAPER, threshold=%.1f dBFS, hold=%.1fs, track=%d, ccLayer=%d\n",
                       config.auto_record_enabled ? "ON" : "OFF",
                       config.auto_record_warning_only ? "WARNING" : "RECORD",
                       config.auto_record_threshold_db,
                       config.auto_record_hold_ms / 1000.0,
                       config.auto_record_monitor_track,
                       config.warning_flash_cc_layer]];
    [self appendToLog:@"Auto-trigger paused. Apply the Auto Trigger settings to make them active.\n"];
}

- (void)onRecorderSettingsChanged:(id)sender {
    auto& config = ReaperExtension::Instance().GetConfig();
    (void)sender;
    [self syncPendingRecorderSettingsFromUI];
    recorderSettingsDirty = YES;
    [self updateAutoTriggerControlsEnabled];
    [self updateAutomationDetails];
    NSString* recorderLabel = ([recorderTargetControl selectedSegment] == 1)
        ? @"USB recorder"
        : @"SD card (WING-LIVE)";
    [self appendToLog:[NSString stringWithFormat:@"Recorder settings pending: enabled=%s, target=%@, followAutoRecord=%s, sourcePair=%d/%d\n",
                       config.recorder_coordination_enabled ? "ON" : "OFF",
                       recorderLabel,
                       config.sd_auto_record_with_reaper ? "ON" : "OFF",
                       config.sd_lr_left_input,
                       config.sd_lr_right_input]];
    [self updateRecorderStatusLabel];
}

- (void)onApplyAutomationSettingsClicked:(id)sender {
    (void)sender;
    auto& extension = ReaperExtension::Instance();
    auto& config = extension.GetConfig();
    [self syncPendingAutomationSettingsFromUI];
    extension.ApplyAutoRecordSettings();
    extension.SyncMidiActionsToWing();

    automationSettingsDirty = NO;
    [self updateValidationStatusLabel];
    [self updateAutoTriggerControlsEnabled];
    [self updateAutomationDetails];
    [self updateRecorderStatusLabel];
    [self appendToLog:[NSString stringWithFormat:@"Applied Auto Trigger settings: %s, mode=%s, source=REAPER, threshold=%.1f dBFS, hold=%.1fs, track=%d, ccLayer=%d\n",
                       config.auto_record_enabled ? "ON" : "OFF",
                       config.auto_record_warning_only ? "WARNING" : "RECORD",
                       config.auto_record_threshold_db,
                       config.auto_record_hold_ms / 1000.0,
                       config.auto_record_monitor_track,
                       config.warning_flash_cc_layer]];
    [self persistConfigAndLog:@"Saved and applied Auto Trigger settings.\n"];
}

- (void)onApplyRecorderSettingsClicked:(id)sender {
    (void)sender;
    if (isWorking || !recorderSettingsDirty) {
        return;
    }
    auto& extension = ReaperExtension::Instance();
    auto& config = extension.GetConfig();
    [self syncPendingRecorderSettingsFromUI];
    NSString* recorderLabel = ([recorderTargetControl selectedSegment] == 1)
        ? @"USB recorder"
        : @"SD card (WING-LIVE)";
    extension.ApplyAutoRecordSettings();
    if (extension.IsConnected()) {
        if (config.recorder_coordination_enabled) {
            extension.ApplyRecorderRoutingNoDialog();
            [self appendToLog:[NSString stringWithFormat:@"Requested Main LR routing to %@ 1/2 (verify on WING).\n",
                               recorderLabel]];
        } else {
            [self appendToLog:[NSString stringWithFormat:@"%@ recorder coordination is disabled. Existing WING routing was not changed.\n",
                               recorderLabel]];
        }
    }
    recorderSettingsDirty = NO;
    [self updateAutoTriggerControlsEnabled];
    [self updateAutomationDetails];
    [self updateRecorderStatusLabel];
    [self appendToLog:[NSString stringWithFormat:@"Applied recorder settings: enabled=%s, target=%@, followAutoRecord=%s, sourcePair=%d/%d\n",
                       config.recorder_coordination_enabled ? "ON" : "OFF",
                       recorderLabel,
                       config.sd_auto_record_with_reaper ? "ON" : "OFF",
                       config.sd_lr_left_input,
                       config.sd_lr_right_input]];
    [self persistConfigAndLog:@"Saved and applied recorder settings.\n"];
}

- (void)syncPendingAutomationSettingsFromUI {
    auto& extension = ReaperExtension::Instance();
    auto& config = extension.GetConfig();
    config.auto_record_enabled = ([autoRecordEnableControl selectedSegment] == 1);
    config.auto_record_warning_only = ([autoRecordModeControl selectedSegment] == 0);
    config.auto_record_threshold_db = [[thresholdField stringValue] doubleValue];
    const double hold_seconds = std::max(0.0, [[holdField stringValue] doubleValue]);
    config.auto_record_hold_ms = (int)std::lround(hold_seconds * 1000.0);
    NSMenuItem* selectedTrackItem = [monitorTrackDropdown selectedItem];
    config.auto_record_monitor_track = selectedTrackItem ? (int)[selectedTrackItem tag] : 0;
    NSMenuItem* selectedLayerItem = [ccLayerDropdown selectedItem];
    config.warning_flash_cc_layer = selectedLayerItem ? (int)[selectedLayerItem tag] : 1;
}

- (void)syncPendingRecorderSettingsFromUI {
    auto& config = ReaperExtension::Instance().GetConfig();
    config.recorder_coordination_enabled = ([recorderEnableControl selectedSegment] == 1);
    config.sd_lr_route_enabled = config.recorder_coordination_enabled;
    config.sd_auto_record_with_reaper = ([recorderFollowControl selectedSegment] == 1);
    config.recorder_target = ([recorderTargetControl selectedSegment] == 1) ? "USBREC" : "WLIVE";
    NSMenuItem* sdItem = [sdSourceDropdown selectedItem];
    int sdLeft = sdItem ? (int)[sdItem tag] : 1;
    config.sd_lr_group = "MAIN";
    config.sd_lr_left_input = sdLeft;
    config.sd_lr_right_input = sdLeft + 1;
}

- (void)onMeterPreviewTimer:(NSTimer*)timer {
    (void)timer;
    auto& extension = ReaperExtension::Instance();
    [self refreshBridgeStatus];
    double lin = extension.ReadCurrentTriggerLevel();
    if (lin <= 0.0000001) {
        [meterPreviewLabel setStringValue:@"Trigger level: -inf dBFS"];
        return;
    }
    double db = 20.0 * std::log10(lin);
    [meterPreviewLabel setStringValue:[NSString stringWithFormat:@"Trigger level: %.1f dBFS", db]];
}

- (void)runSetupSoundcheckFlow {
    NSString* wingIP = [self selectedOrManualWingIP];
    WingConnectorWindowController* blockSelf = self;

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        auto& extension = ReaperExtension::Instance();

        if (!extension.IsConnected()) {
            dispatch_async(dispatch_get_main_queue(), ^{ [blockSelf appendToLog:@"Not connected — attempting to connect automatically so sources can be loaded...\n"]; });
            if (!wingIP) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ No Wing selected. Press Scan or enter a manual IP.\n"];
                    [blockSelf setWorkingState:NO];
                    [blockSelf updateConnectionStatus];
                });
                return;
            }
            auto& config = extension.GetConfig();
            config.wing_ip = std::string([wingIP UTF8String]);
            if (!extension.ConnectToWing()) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ Auto-connect failed. Check that the Wing is reachable.\n"];
                    [blockSelf setWorkingState:NO];
                    [blockSelf updateConnectionStatus];
                });
                return;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"✓ Auto-connected to Wing for source staging\n"];
                [blockSelf updateConnectionStatus];
            });
        }

        dispatch_async(dispatch_get_main_queue(), ^{ [blockSelf appendToLog:@"Getting Wing sources for live recording setup...\n"]; });
        auto channels = extension.GetAvailableSources();

        if (channels.empty()) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"✗ No selectable sources found.\n"];
                [blockSelf setWorkingState:NO];
            });
            return;
        }

        if (hasPendingSetupDraft && !pendingSetupChannels.empty()) {
            std::set<std::string> pendingIds;
            auto pendingSourceId = [](const WingConnector::ChannelSelectionInfo& source) {
                const char* kind = "SRC";
                switch (source.kind) {
                    case SourceKind::Channel: kind = "CH"; break;
                    case SourceKind::Bus: kind = "BUS"; break;
                    case SourceKind::Main: kind = "MAIN"; break;
                    case SourceKind::Matrix: kind = "MTX"; break;
                }
                return std::string(kind) + ":" + std::to_string(source.source_number);
            };
            for (const auto& source : pendingSetupChannels) {
                if (source.selected) {
                    pendingIds.insert(pendingSourceId(source));
                }
            }
            for (auto& channel : channels) {
                channel.selected = pendingIds.count(pendingSourceId(channel)) > 0;
            }
        }

        __block auto blockChannels = channels;
        __block bool confirmed = false;
        __block bool setup_soundcheck = pendingSetupSoundcheck ? true : false;
        __block bool overwrite_existing = pendingReplaceExisting ? true : false;
        dispatch_sync(dispatch_get_main_queue(), ^{
            [blockSelf appendToLog:[NSString stringWithFormat:@"Found %d selectable sources\n", (int)blockChannels.size()]];
            confirmed = ShowChannelSelectionDialog(
                blockChannels,
                "Review Sources for Live Setup",
                "Choose which channels, buses, or matrices should be included in the next apply. No routing will change until you confirm the pending setup.",
                setup_soundcheck,
                overwrite_existing
            );
        });

        if (!confirmed) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"Cancelled by user\n"];
                [blockSelf setWorkingState:NO];
            });
            return;
        }

        int selectedCount = 0;
        for (const auto& ch : blockChannels) {
            if (ch.selected) selectedCount++;
        }

        if (selectedCount == 0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"✗ No sources selected\n"];
                [blockSelf setWorkingState:NO];
            });
            return;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            blockSelf->pendingSetupChannels = blockChannels;
            blockSelf->hasPendingSetupDraft = YES;
            blockSelf->pendingSetupSoundcheck = setup_soundcheck ? YES : NO;
            blockSelf->pendingReplaceExisting = overwrite_existing ? YES : NO;
            blockSelf->pendingSetupUsesExistingSelection = NO;
            blockSelf->liveSetupValidated = NO;
            [blockSelf appendToLog:[NSString stringWithFormat:@"Staged live setup for %d selected sources. Review the summary and click Apply Setup when ready.\n",
                                    selectedCount]];
            [blockSelf setWorkingState:NO];
            [blockSelf updatePendingSetupUI];
            [blockSelf updateValidationStatusLabel];
            [blockSelf updateSetupSoundcheckButtonLabel];
            [blockSelf updateToggleSoundcheckButtonLabel];
            [blockSelf updateAutoTriggerControlsEnabled];
        });
    });
}

- (void)runApplyPendingSetupFlow {
    NSString* wingIP = [self selectedOrManualWingIP];
    WingConnectorWindowController* blockSelf = self;

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        auto& extension = ReaperExtension::Instance();

        extension.PauseAutoRecordForSetup();
        dispatch_async(dispatch_get_main_queue(), ^{
            [blockSelf appendToLog:@"Auto-trigger paused during setup configuration.\n"];
        });

        if (!extension.IsConnected()) {
            dispatch_async(dispatch_get_main_queue(), ^{ [blockSelf appendToLog:@"Not connected — attempting to connect automatically...\n"]; });
            if (!wingIP) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ No Wing selected. Press Scan or enter a manual IP.\n"];
                    [blockSelf setWorkingState:NO];
                    [blockSelf updateConnectionStatus];
                });
                return;
            }
            auto& config = extension.GetConfig();
            config.wing_ip = std::string([wingIP UTF8String]);
            if (!extension.ConnectToWing()) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ Auto-connect failed. Check that the Wing is reachable.\n"];
                    [blockSelf setWorkingState:NO];
                    [blockSelf updateConnectionStatus];
                });
                return;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"✓ Auto-connected to Wing\n"];
                [blockSelf updateConnectionStatus];
            });
        }

        std::vector<WingConnector::ChannelSelectionInfo> channels_to_apply;
        bool setup_soundcheck = blockSelf->pendingSetupSoundcheck ? true : false;
        bool overwrite_existing = blockSelf->pendingReplaceExisting ? true : false;

        if (blockSelf->pendingSetupUsesExistingSelection && blockSelf->pendingSetupChannels.empty()) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"No staged source draft found. Reusing the current managed selection for this rebuild.\n"];
            });
            channels_to_apply = extension.GetAvailableSources();
            if (channels_to_apply.empty()) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ Could not reload the current managed selection for rebuild.\n"];
                    [blockSelf setWorkingState:NO];
                });
                return;
            }
        } else if (blockSelf->hasPendingSetupDraft) {
            channels_to_apply = blockSelf->pendingSetupChannels;
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"No staged source draft found. Reusing the current managed selection for this rebuild.\n"];
            });
            channels_to_apply = extension.GetAvailableSources();
            if (channels_to_apply.empty()) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ Could not reload the current managed selection for rebuild.\n"];
                    [blockSelf setWorkingState:NO];
                });
                return;
            }
        }

        int selectedCount = 0;
        for (const auto& ch : channels_to_apply) {
            if (ch.selected) {
                selectedCount++;
            }
        }
        if (selectedCount == 0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"✗ No sources are staged for apply.\n"];
                [blockSelf setWorkingState:NO];
            });
            return;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [blockSelf appendToLog:[NSString stringWithFormat:@"Applying live recording setup for %d sources (%s existing REAPER tracks, %s mode)...\n",
                                    selectedCount,
                                    overwrite_existing ? "replacing" : "appending to",
                                    blockSelf->pendingOutputMode.c_str()]];
            const std::string applied_output_mode = extension.GetConfig().soundcheck_output_mode;
            extension.GetConfig().soundcheck_output_mode = blockSelf->pendingOutputMode;
            if (extension.SetupSoundcheckFromSelection(channels_to_apply, setup_soundcheck, overwrite_existing)) {
                [blockSelf appendToLog:@"✓ Live recording setup complete\n"];
                [blockSelf clearPendingSetupDraft:NO];
                [blockSelf refreshMonitorTrackDropdown];
                [blockSelf persistConfigAndLog:@"Saved live setup changes.\n"];
            } else {
                // Keep both the staged draft and its output mode available for a retry.
                extension.GetConfig().soundcheck_output_mode = applied_output_mode;
                [blockSelf appendToLog:@"✗ Live recording setup did not complete. The staged setup was preserved for retry.\n"];
            }
            [blockSelf setWorkingState:NO];
            [blockSelf refreshLiveSetupValidation];
        });
    });
}

- (void)runToggleSoundcheckModeFlow {
    // Capture UI values on the main thread before dispatching
    NSString* wingIP = [self selectedOrManualWingIP];
    WingConnectorWindowController* blockSelf = self;

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        auto& extension = ReaperExtension::Instance();

        if (!extension.IsConnected()) {
            dispatch_async(dispatch_get_main_queue(), ^{ [blockSelf appendToLog:@"Not connected — attempting to connect automatically...\n"]; });
            if (!wingIP) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ No Wing selected. Press Scan or enter a manual IP.\n"];
                    [blockSelf setWorkingState:NO];
                    [blockSelf updateConnectionStatus];
                });
                return;
            }
            auto& config = extension.GetConfig();
            config.wing_ip = std::string([wingIP UTF8String]);
            if (!extension.ConnectToWing()) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [blockSelf appendToLog:@"✗ Auto-connect failed. Check that the Wing is reachable.\n"];
                    [blockSelf setWorkingState:NO];
                    [blockSelf updateConnectionStatus];
                });
                return;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [blockSelf appendToLog:@"✓ Auto-connected to Wing\n"];
                [blockSelf updateConnectionStatus];
            });
        }

        // CRITICAL: ToggleSoundcheckMode() shows message boxes, which MUST run on main thread
        dispatch_async(dispatch_get_main_queue(), ^{
            [blockSelf appendToLog:@"Toggling soundcheck mode...\n"];

            extension.ToggleSoundcheckMode();
            bool enabled = extension.IsSoundcheckModeEnabled();

            if (enabled) {
                [blockSelf appendToLog:@"✓ Soundcheck mode ENABLED (using playback inputs)\n"];
            } else {
                [blockSelf appendToLog:@"✓ Soundcheck mode DISABLED (using live inputs)\n"];
            }
            [blockSelf updateToggleSoundcheckButtonLabel];
            [blockSelf setWorkingState:NO];
            [blockSelf refreshLiveSetupValidation];
        });
    });
}

@end

extern "C" {

void ShowWingConnectorDialogAtTab(const char* tab_identifier) {
    // Static to keep controller alive in MRC - window doesn't retain it by default
    static WingConnectorWindowController* controller = nil;
    NSString* desiredTab = tab_identifier ? [NSString stringWithUTF8String:tab_identifier] : @"console";

    // Must run UI operations on main thread
    dispatch_async(dispatch_get_main_queue(), ^{
        // NO @autoreleasepool here - it would drain objects that need to live longer
        // If window already exists and is visible, just bring it to front
        if (controller && [[controller window] isVisible]) {
            [controller selectTabWithIdentifier:desiredTab];
            [[controller window] makeKeyAndOrderFront:nil];
            return;
        }

        // Create new controller (retained by static variable)
        if (controller) {
            [controller release];
        }
        controller = [[WingConnectorWindowController alloc] init];
        [controller selectTabWithIdentifier:desiredTab];

        [[controller window] makeKeyAndOrderFront:nil];
    });
}

void ShowWingConnectorDialog() {
    ShowWingConnectorDialogAtTab("console");
}

} // extern "C"

#endif // __APPLE__
