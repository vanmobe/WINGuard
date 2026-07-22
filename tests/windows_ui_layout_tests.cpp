#include "internal/windows_ui_layout.h"

#include <cstdlib>
#include <iostream>

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    using namespace WingConnector::WindowsUi;

    // These scales bound the reported 150% clipping regression and the mixed-
    // DPI monitor transitions expected during native Windows verification.
    Expect(ScaleDip(100, 96) == 100, "96 DPI must preserve base dimensions");
    Expect(ScaleDip(100, 120) == 125, "125% DPI must scale dimensions proportionally");
    Expect(ScaleDip(100, 144) == 150, "150% DPI must scale dimensions proportionally");
    Expect(ScaleDip(100, 192) == 200, "200% DPI must scale dimensions proportionally");
    Expect(ScaleDip(100, 0) == 100, "unknown DPI must fall back to 96 DPI");

    Expect(PreferredWindowExtent(1920, 860, 820) == 860,
           "large work areas should use the macOS-equivalent preferred extent");
    Expect(PreferredWindowExtent(830, 860, 820) == 830,
           "the preferred extent should be clamped to the work area");
    Expect(PreferredWindowExtent(820, 860, 820) == 820,
           "the minimum should win when it fits in the work area");
    Expect(PreferredWindowExtent(720, 860, 820) == 720,
           "small work areas must never be exceeded");

    const MainVerticalLayout base = CalculateMainVerticalLayout(620, 96);
    Expect(base.page_y == 154, "compact header and tabs should reclaim vertical space");
    Expect(base.page_height > 0, "minimum-height windows must retain a usable page viewport");
    Expect(base.page_y + base.page_height <= base.footer_y,
           "page content must never overlap the footer");

    const MainVerticalLayout scaled = CalculateMainVerticalLayout(930, 144);
    Expect(scaled.page_y == ScaleDip(base.page_y, 144),
           "vertical layout must scale consistently at 150%");
    Expect(scaled.page_y + scaled.page_height <= scaled.footer_y,
           "scaled page content must never overlap the footer");

    const MainVerticalLayout doubled = CalculateMainVerticalLayout(1120, 192);
    Expect(doubled.page_y == ScaleDip(base.page_y, 192),
           "vertical layout must scale consistently at 200%");
    Expect(doubled.page_y + doubled.page_height <= doubled.footer_y,
           "200% page content must never overlap the footer");

    const MainVerticalLayout minimum_window = CalculateMainVerticalLayout(520, 96);
    Expect(minimum_window.page_height > 0,
           "the monitor-clamped minimum window must retain a scrollable page viewport");
    Expect(minimum_window.page_y + minimum_window.page_height <= minimum_window.footer_y,
           "minimum-window content must not overlap the footer");

    // Deliberately below the supported minimum: defensive geometry should
    // collapse the page viewport instead of allowing page/footer overlap.
    const MainVerticalLayout short_window = CalculateMainVerticalLayout(210, 96);
    Expect(short_window.page_height == 0,
           "extremely short windows must collapse the viewport instead of overlapping the footer");
    Expect(short_window.page_y + short_window.page_height <= short_window.footer_y,
           "short-window layout must preserve page/footer ordering");

    std::cout << "windows_ui_layout_tests passed" << std::endl;
    return 0;
}
