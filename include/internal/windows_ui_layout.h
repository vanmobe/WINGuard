/*
 * Pure, Win32-free layout helpers for the native Windows dialogs.
 *
 * Keeping the arithmetic here makes the clipping-sensitive shell geometry
 * testable on every build host, including machines that cannot compile or
 * render the Win32 implementation.
 */

#ifndef WINGCONNECTOR_WINDOWS_UI_LAYOUT_H
#define WINGCONNECTOR_WINDOWS_UI_LAYOUT_H

#include <algorithm>

namespace WingConnector::WindowsUi {

constexpr unsigned int kBaseDpi = 96;
constexpr int kMainPreferredWindowWidthDip = 860;
constexpr int kMainPreferredWindowHeightDip = 780;
constexpr int kMainMinWindowWidthDip = 820;
constexpr int kMainMinWindowHeightDip = 560;

// A DIP is a 96-DPI logical pixel. Round to the nearest physical pixel so the
// same logical geometry remains stable while a window moves between monitors.
inline int ScaleDip(int value, unsigned int dpi) {
    const unsigned int effective_dpi = dpi == 0 ? kBaseDpi : dpi;
    return static_cast<int>((static_cast<long long>(value) * effective_dpi + (kBaseDpi / 2)) / kBaseDpi);
}

inline int PreferredWindowExtent(int work_extent,
                                 int preferred_extent,
                                 int minimum_extent) {
    // All three arguments are already in the same coordinate space. A small
    // monitor wins over the nominal minimum so the window never opens outside
    // the usable work area.
    if (work_extent <= 0) {
        return minimum_extent;
    }
    return std::min(work_extent, std::max(minimum_extent, preferred_extent));
}

struct MainVerticalLayout {
    int header_y = 0;
    int header_height = 0;
    int tab_y = 0;
    int tab_height = 0;
    int page_y = 0;
    int page_height = 0;
    int footer_y = 0;
    int footer_height = 0;
};

inline MainVerticalLayout CalculateMainVerticalLayout(int client_height, unsigned int dpi) {
    // The main dialog converts its client rectangle to DIPs and deliberately
    // calls this with kBaseDpi. Passing a real monitor DPI is supported for
    // invariant tests, but callers must not mix physical and logical units.
    MainVerticalLayout layout;
    layout.header_y = 0;
    layout.header_height = ScaleDip(104, dpi);
    layout.tab_y = layout.header_y + layout.header_height + ScaleDip(8, dpi);
    layout.tab_height = ScaleDip(38, dpi);
    layout.page_y = layout.tab_y + layout.tab_height + ScaleDip(4, dpi);
    layout.footer_height = ScaleDip(28, dpi);
    layout.footer_y = std::max(layout.page_y,
                               client_height - layout.footer_height - ScaleDip(10, dpi));
    const int available_page_height = std::max(0, layout.footer_y - layout.page_y - ScaleDip(8, dpi));
    layout.page_height = available_page_height < ScaleDip(24, dpi) ? 0 : available_page_height;
    return layout;
}

}  // namespace WingConnector::WindowsUi

#endif
