/*
 * Windows native WINGuard dialog implementation.
 *
 * REAPER owns process DPI awareness. These dialogs therefore keep DPI, fonts,
 * and geometry per top-level window and never change process-wide DPI state.
 * Layout is expressed in 96-DPI logical units; only Win32 calls and painting
 * rectangles cross into physical pixels.
 */

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>

#include "internal/wing_connector_dialog_windows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <mutex>
#include <utility>
#include <vector>

#include "internal/logger.h"
#include "internal/windows_ui_layout.h"
#include "reaper_plugin_functions.h"
#include "wingconnector/reaper_extension.h"

using namespace WingConnector;

extern REAPER_PLUGIN_HINSTANCE g_hInst;
extern HWND g_hwndParent;

namespace {

constexpr wchar_t kDialogClassName[] = L"WINGuardWindowsDialog";
constexpr wchar_t kSourceDialogClassName[] = L"WINGuardSourcePickerDialog";
constexpr wchar_t kDebugLogClassName[] = L"WINGuardDebugLogDialog";
constexpr wchar_t kAdoptionDialogClassName[] = L"WINGuardAdoptionDialog";
constexpr wchar_t kPageClassName[] = L"WINGuardWindowsPage";
constexpr wchar_t kLogoClassName[] = L"WINGuardWindowsLogo";
constexpr UINT_PTR kRefreshTimerId = 101;
constexpr UINT kRefreshTimerMs = 500;
constexpr UINT kMsgAsyncScanComplete = WM_APP + 1;
constexpr UINT kMsgAsyncConnectComplete = WM_APP + 2;
constexpr UINT kMsgAsyncSourcesComplete = WM_APP + 3;
constexpr UINT kMsgAsyncApplyPlanComplete = WM_APP + 4;
constexpr UINT kMsgAsyncToggleComplete = WM_APP + 5;
constexpr UINT kMsgAsyncValidationComplete = WM_APP + 6;
constexpr int kScrollLineStep = 36;
constexpr unsigned long kValidationRefreshIntervalMs = 1500;

struct AsyncScanResult {
    std::vector<WingInfo> wings;
    bool show_feedback = true;
};

struct AsyncConnectResult {
    bool success = false;
    std::string ip;
    std::string failure_detail;
    std::wstring success_footer;
};

struct AsyncSourcesResult {
    bool success = false;
    bool used_pending_draft = false;
    std::vector<SourceSelectionInfo> channels;
    std::string failure_detail;
};

struct AsyncApplyPlanResult {
    bool success = false;
    bool setup_soundcheck = true;
    bool replace_existing = true;
    std::string output_mode;
    std::vector<SourceSelectionInfo> prepared_channels;
    std::vector<PlaybackAllocation> prepared_allocations;
    std::string failure_detail;
};

struct AsyncToggleResult {
    bool success = false;
    bool enabled = false;
    std::string failure_detail;
};

struct AsyncValidationResult {
    unsigned long long generation = 0;
    ValidationState state = ValidationState::NotReady;
    std::string details;
};

enum ControlId {
    kIdTab = 100,
    kIdBannerGroup,
    kIdLogo,
    kIdTitle,
    kIdSubtitle,
    kIdStatusGroup,
    kIdHeaderConsoleIcon,
    kIdHeaderConsoleStatus,
    kIdHeaderValidationIcon,
    kIdHeaderValidationStatus,
    kIdHeaderRecorderIcon,
    kIdHeaderRecorderStatus,
    kIdHeaderMidiIcon,
    kIdHeaderMidiStatus,
    kIdConsoleStatusChip,
    kIdReaperStatusChip,
    kIdWingStatusChip,
    kIdControlStatusChip,
    kIdWingCombo,
    kIdScanButton,
    kIdManualIpEdit,
    kIdConnectButton,
    kIdConsoleHelp,
    kIdReaperOutputUsb,
    kIdReaperOutputCard,
    kIdPendingSummary,
    kIdReadinessDetail,
    kIdChooseSourcesButton,
    kIdApplySetupButton,
    kIdDiscardSetupButton,
    kIdToggleSoundcheckButton,
    kIdReaperHelp,
    kIdWingPlaceholder,
    kIdControlPlaceholder,
    kIdFooterStatus,
    kIdPageFrame,
    kIdTabConsoleButton,
    kIdTabReaperButton,
    kIdTabWingButton,
    kIdTabControlButton,
    kIdAutoTriggerHeader,
    kIdAutoTriggerDetail,
    kIdAutoTriggerHint,
    kIdAutoTriggerEnableOff,
    kIdAutoTriggerEnableOn,
    kIdAutoTriggerModeWarning,
    kIdAutoTriggerModeRecord,
    kIdAutoTriggerThresholdEdit,
    kIdAutoTriggerHoldEdit,
    kIdAutoTriggerMonitorTrackCombo,
    kIdAutoTriggerMeterLabel,
    kIdApplyAutoTriggerButton,
    kIdDiscardAutoTriggerButton,
    kIdConsoleSectionIcon,
    kIdReaperSectionIcon,
    kIdAutoTriggerSectionIcon,
    kIdWingSectionIcon,
    kIdControlSectionIcon,
    kIdRecorderEnableOff,
    kIdRecorderEnableOn,
    kIdRecorderTargetWLive,
    kIdRecorderTargetUsb,
    kIdRecorderPair1,
    kIdRecorderPair3,
    kIdRecorderPair5,
    kIdRecorderPair7,
    kIdRecorderFollowOff,
    kIdRecorderFollowOn,
    kIdRecorderDetail,
    kIdApplyRecorderButton,
    kIdDiscardRecorderButton,
    kIdMidiActionsOff,
    kIdMidiActionsOn,
    kIdMidiSummary,
    kIdMidiDetail,
    kIdWarningLayerCombo,
    kIdApplyMidiButton,
    kIdDiscardMidiButton,
    kIdOpenDebugLogButton,
    kIdClearDebugLogButton,
    kIdDebugLogView,
    kIdSourceList,
    kIdSourceSelectAll,
    kIdSourceSelectChannels,
    kIdSourceClear,
    kIdSourceModeSoundcheck,
    kIdSourceModeRecord,
    kIdSourceReplace,
    kIdSourceOk,
    kIdSourceCancel,
    kIdSourceCount
};

std::wstring ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], length);
    wide.resize(static_cast<size_t>(length - 1));
    return wide;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string(text.begin(), text.end());
    }
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &utf8[0], length, nullptr, nullptr);
    utf8.resize(static_cast<size_t>(length - 1));
    return utf8;
}

std::wstring ReadWindowText(HWND hwnd) {
    if (!hwnd) {
        return std::wstring();
    }
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(std::max(length, 0) + 1), L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, &text[0], length + 1);
    }
    text.resize(static_cast<size_t>(std::max(length, 0)));
    return text;
}

bool SetWindowTextIfChanged(HWND hwnd, const std::wstring& text) {
    if (!hwnd) {
        return false;
    }
    if (ReadWindowText(hwnd) == text) {
        return false;
    }
    SetWindowTextW(hwnd, text.c_str());
    return true;
}

void SetWindowFontRecursive(HWND hwnd, HFONT font) {
    if (!hwnd || !font) {
        return;
    }
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SetWindowFontRecursive(child, font);
    }
}

ULONG_PTR EnsureGdiplusToken() {
    static ULONG_PTR token = 0;
    static bool started = false;
    if (started) {
        return token;
    }
    Gdiplus::GdiplusStartupInput startup_input;
    if (Gdiplus::GdiplusStartup(&token, &startup_input, nullptr) == Gdiplus::Ok) {
        started = true;
    }
    return token;
}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(g_hInst), path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L".";
    }
    std::wstring full_path(path, path + length);
    const size_t slash = full_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return full_path.substr(0, slash);
}

std::wstring ResolveLogoPath() {
    std::array<std::wstring, 4> candidates = {
        ModuleDirectory() + L"\\wingguard-logo.png",
        ModuleDirectory() + L"\\assets\\wingguard-logo.png",
        L"assets\\wingguard-logo.png",
        L"wingguard-logo.png"
    };
    for (const auto& path : candidates) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return path;
        }
    }
    return std::wstring();
}

UINT WindowDpi(HWND hwnd) {
    // REAPER owns the process DPI-awareness context. Query each window's
    // effective DPI without changing process-wide or persistent thread-wide
    // awareness. GetDpiForWindow is resolved dynamically for older Windows.
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (get_dpi_for_window && hwnd) {
        const UINT dpi = get_dpi_for_window(hwnd);
        if (dpi != 0) {
            return dpi;
        }
    }

    HDC hdc = GetDC(hwnd);
    const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 0;
    if (hdc) {
        ReleaseDC(hwnd, hdc);
    }
    return dpi > 0 ? static_cast<UINT>(dpi) : WindowsUi::kBaseDpi;
}

int UnscalePixels(int value, UINT dpi) {
    // WM_SIZE reports physical pixels. Convert once at the layout boundary so
    // all downstream spacing and scroll state remain in DIPs.
    const UINT effective_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
    return static_cast<int>((static_cast<long long>(value) * WindowsUi::kBaseDpi + effective_dpi - 1) /
                            effective_dpi);
}

RECT GetPreferredWindowRect(UINT dpi) {
    // Preferred and minimum sizes mirror the macOS information hierarchy, but
    // the active monitor work area always has final authority.
    RECT work_area{0, 0, 1920, 1080};
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(g_hwndParent, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        work_area = monitor_info.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    }
    const int work_width = work_area.right - work_area.left;
    const int work_height = work_area.bottom - work_area.top;
    const int minimum_width = WindowsUi::ScaleDip(WindowsUi::kMainMinWindowWidthDip, dpi);
    const int minimum_height = WindowsUi::ScaleDip(WindowsUi::kMainMinWindowHeightDip, dpi);
    const int preferred_width = WindowsUi::ScaleDip(WindowsUi::kMainPreferredWindowWidthDip, dpi);
    const int preferred_height = WindowsUi::ScaleDip(WindowsUi::kMainPreferredWindowHeightDip, dpi);
    const int width = WindowsUi::PreferredWindowExtent(work_width, preferred_width, minimum_width);
    const int height = WindowsUi::PreferredWindowExtent(work_height, preferred_height, minimum_height);
    const int x = work_area.left + std::max(0, (work_width - width) / 2);
    const int y = work_area.top + std::max(0, (work_height - height) / 2);
    return RECT{x, y, x + width, y + height};
}

RECT GetFittedDialogRect(HWND owner, int width_dip, int height_dip, UINT dpi) {
    RECT work_area{0, 0, 1920, 1080};
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        work_area = monitor_info.rcWork;
    }
    const int work_width = work_area.right - work_area.left;
    const int work_height = work_area.bottom - work_area.top;
    const int width = std::min(work_width, WindowsUi::ScaleDip(width_dip, dpi));
    const int height = std::min(work_height, WindowsUi::ScaleDip(height_dip, dpi));
    const int x = work_area.left + std::max(0, (work_width - width) / 2);
    const int y = work_area.top + std::max(0, (work_height - height) / 2);
    return RECT{x, y, x + width, y + height};
}

void SetMonitorClampedMinimumTrackSize(HWND hwnd,
                                       MINMAXINFO* info,
                                       int minimum_width_dip,
                                       int minimum_height_dip,
                                       UINT dpi) {
    // A scaled logical minimum can exceed a small monitor's work area. Clamp it
    // so title chrome and resize handles remain reachable.
    if (!info) {
        return;
    }
    int minimum_width = WindowsUi::ScaleDip(minimum_width_dip, dpi);
    int minimum_height = WindowsUi::ScaleDip(minimum_height_dip, dpi);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        const int work_width = static_cast<int>(monitor_info.rcWork.right - monitor_info.rcWork.left);
        const int work_height = static_cast<int>(monitor_info.rcWork.bottom - monitor_info.rcWork.top);
        minimum_width = std::min(minimum_width, work_width);
        minimum_height = std::min(minimum_height, work_height);
    }
    info->ptMinTrackSize.x = minimum_width;
    info->ptMinTrackSize.y = minimum_height;
}

LOGFONTW SystemMessageFont(UINT dpi) {
    // Build role fonts from the user's Windows message font instead of pinning
    // Segoe UI or a fixed point size. This follows display DPI and system font
    // changes while preserving native Win32 text rendering. NONCLIENTMETRICS
    // does not provide the separate Accessibility Text Size scale factor.
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);

    using SystemParametersInfoForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static const auto system_parameters_info_for_dpi = reinterpret_cast<SystemParametersInfoForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));
    const bool loaded = system_parameters_info_for_dpi &&
                        system_parameters_info_for_dpi(SPI_GETNONCLIENTMETRICS,
                                                       sizeof(metrics),
                                                       &metrics,
                                                       0,
                                                       dpi);
    if (!loaded) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    }

    LOGFONTW font = metrics.lfMessageFont;
    if (font.lfHeight == 0) {
        font.lfHeight = -WindowsUi::ScaleDip(12, dpi);
        std::wcsncpy(font.lfFaceName, L"Segoe UI", LF_FACESIZE - 1);
        font.lfFaceName[LF_FACESIZE - 1] = L'\0';
    }
    font.lfQuality = CLEARTYPE_QUALITY;
    return font;
}

HFONT CreateUiFontForDpi(UINT dpi,
                         int size_percent = 100,
                         int weight = FW_DONTCARE,
                         bool monospace = false) {
    // The caller owns the returned HFONT. Each top-level dialog must bind a
    // replacement before deleting its previous DPI-specific font handles.
    LOGFONTW font = SystemMessageFont(dpi);
    const int base_height = std::max(1, std::abs(static_cast<int>(font.lfHeight)));
    font.lfHeight = -std::max(1, (base_height * size_percent + 50) / 100);
    if (weight != FW_DONTCARE) {
        font.lfWeight = weight;
    }
    if (monospace) {
        font.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        std::wcsncpy(font.lfFaceName, L"Consolas", LF_FACESIZE - 1);
        font.lfFaceName[LF_FACESIZE - 1] = L'\0';
    }
    return CreateFontIndirectW(&font);
}

HFONT CreateUiFont(HWND hwnd,
                   int size_percent = 100,
                   int weight = FW_DONTCARE,
                   bool monospace = false) {
    return CreateUiFontForDpi(WindowDpi(hwnd), size_percent, weight, monospace);
}

HFONT CreateSymbolFontForDpi(UINT dpi, int height_dip = 16, int weight = FW_SEMIBOLD) {
    LOGFONTW font = SystemMessageFont(dpi);
    font.lfHeight = -WindowsUi::ScaleDip(height_dip, dpi);
    font.lfWeight = weight;
    font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    std::wcsncpy(font.lfFaceName, L"Segoe UI Symbol", LF_FACESIZE - 1);
    font.lfFaceName[LF_FACESIZE - 1] = L'\0';
    return CreateFontIndirectW(&font);
}

struct HeaderStatusVisual {
    COLORREF color = RGB(110, 110, 110);
    std::wstring text;
};

void SaveConfigIfPossible(ReaperExtension& extension) {
    const std::string path = WingConfig::GetConfigPath();
    if (!extension.GetConfig().SaveToFile(path)) {
        Logger::Error("Failed to save WINGuard config to %s", path.c_str());
    }
}

int MeasureTextWidth(HWND control, const std::wstring& text) {
    if (!control) {
        return 0;
    }
    HDC hdc = GetDC(control);
    if (!hdc) {
        return 0;
    }
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HFONT old_font = font ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    RECT rect{0, 0, 0, 0};
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CALCRECT | DT_SINGLELINE);
    if (old_font) {
        SelectObject(hdc, old_font);
    }
    ReleaseDC(control, hdc);
    return rect.right - rect.left;
}

int ButtonWidthForLabelDip(HWND control, int min_width, int padding, UINT dpi) {
    return std::max(min_width,
                    UnscalePixels(MeasureTextWidth(control, ReadWindowText(control)), dpi) + padding);
}

int MultiLineTextHeight(HWND control, int width, const std::wstring& text);

int StaticHeightForTextDip(HWND control, int width, int min_height, int padding, UINT dpi) {
    if (!control) {
        return min_height;
    }
    const int measured_pixels = MultiLineTextHeight(control, WindowsUi::ScaleDip(width, dpi), ReadWindowText(control));
    return std::max(min_height, UnscalePixels(measured_pixels, dpi) + padding);
}

int MultiLineTextHeight(HWND control, int width, const std::wstring& text) {
    if (!control) {
        return 0;
    }
    HDC hdc = GetDC(control);
    if (!hdc) {
        return 0;
    }
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HFONT old_font = font ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    RECT rect{0, 0, width, 0};
    DrawTextW(hdc,
              text.c_str(),
              static_cast<int>(text.size()),
              &rect,
              DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL);
    if (old_font) {
        SelectObject(hdc, old_font);
    }
    ReleaseDC(control, hdc);
    return rect.bottom - rect.top;
}

std::wstring CleanLogMessage(const std::string& message) {
    std::wstring cleaned = ToWide(message);
    const std::array<std::wstring, 4> prefixes = {
        L"AUDIOLAB.wing.reaper.virtualsoundcheck: ",
        L"AUDIOLAB.wing.reaper.virtualsoundcheck:",
        L"WINGuard: ",
        L"WINGuard:"
    };
    for (const auto& prefix : prefixes) {
        size_t pos = std::wstring::npos;
        while ((pos = cleaned.find(prefix)) != std::wstring::npos) {
            cleaned.erase(pos, prefix.size());
        }
    }
    return cleaned;
}

struct SourcePickerResult {
    bool confirmed = false;
    bool setup_soundcheck = true;
    bool replace_existing = true;
    std::vector<SourceSelectionInfo> channels;
};

class SourcePickerDialog {
public:
    SourcePickerDialog(HWND owner,
                       std::vector<SourceSelectionInfo> channels,
                       bool setup_soundcheck,
                       bool replace_existing)
        : owner_(owner) {
        result_.channels = std::move(channels);
        result_.setup_soundcheck = setup_soundcheck;
        result_.replace_existing = replace_existing;
    }

    SourcePickerResult Run() {
        RegisterClass();
        dpi_ = WindowDpi(owner_);
        const RECT fitted = GetFittedDialogRect(owner_, 960, 680, dpi_);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            kSourceDialogClassName,
            L"Review Sources For Live Setup",
            WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_THICKFRAME,
            fitted.left, fitted.top, fitted.right - fitted.left, fitted.bottom - fitted.top,
            owner_,
            nullptr,
            g_hInst,
            this);
        if (!hwnd_) {
            return result_;
        }

        // This is an owned modal window rather than a resource-template dialog.
        // IsDialogMessage preserves keyboard traversal while the owner is
        // disabled, and ownership is restored on every exit path below.
        EnableWindow(owner_, FALSE);
        MSG msg{};
        while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (owner_) {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        if (title_font_) {
            DeleteObject(title_font_);
            title_font_ = nullptr;
        }
        return result_;
    }

private:
    static void RegisterClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = &SourcePickerDialog::WndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = kSourceDialogClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        SourcePickerDialog* self = reinterpret_cast<SourcePickerDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<SourcePickerDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) {
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        switch (msg) {
            case WM_CREATE: return self->OnCreate();
            case WM_COMMAND: return self->OnCommand(LOWORD(wparam), HIWORD(wparam));
            case WM_CTLCOLORSTATIC: return self->OnCtlColor(reinterpret_cast<HDC>(wparam));
            case WM_CTLCOLOREDIT: return self->OnCtlColor(reinterpret_cast<HDC>(wparam));
            case WM_CTLCOLORLISTBOX: return self->OnCtlColor(reinterpret_cast<HDC>(wparam));
            case WM_SIZE: return self->OnSize(LOWORD(lparam), HIWORD(lparam));
            case WM_GETMINMAXINFO: return self->OnGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(lparam));
            case WM_DPICHANGED:
                return self->OnDpiChanged(HIWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            case WM_SETTINGCHANGE:
                return self->OnSystemSettingsChanged();
            case WM_CLOSE:
                self->done_ = true;
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    LRESULT OnCreate() {
        RecreateFonts(WindowDpi(hwnd_));

        intro_ = CreateWindowW(L"STATIC",
                      L"Choose which channels, buses, or matrices should be included in the next apply. No routing changes happen until you confirm.",
                      WS_CHILD | WS_VISIBLE,
                      24, 20, 1048, 70,
                      hwnd_,
                      nullptr,
                      g_hInst,
                      nullptr);

        listbox_ = CreateWindowExW(WS_EX_CLIENTEDGE,
                                   L"LISTBOX",
                                   nullptr,
                                   WS_CHILD | WS_VISIBLE | LBS_EXTENDEDSEL | WS_VSCROLL | LBS_NOTIFY,
                                   24, 104, 1048, 530,
                                   hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceList)),
                                   g_hInst,
                                   nullptr);

        select_all_ = CreateWindowW(L"BUTTON", L"Select All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      24, 658, 164, 42, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceSelectAll)), g_hInst, nullptr);
        channels_only_ = CreateWindowW(L"BUTTON", L"Channels Only", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      204, 658, 194, 42, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceSelectChannels)), g_hInst, nullptr);
        clear_button_ = CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      414, 658, 118, 42, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceClear)), g_hInst, nullptr);

        count_label_ = CreateWindowW(L"STATIC", L"0 sources selected", WS_CHILD | WS_VISIBLE,
                                     572, 664, 340, 34, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceCount)), g_hInst, nullptr);

        mode_soundcheck_ = CreateWindowW(L"BUTTON", L"Soundcheck", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                         24, 724, 174, 34, hwnd_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceModeSoundcheck)), g_hInst, nullptr);
        mode_record_ = CreateWindowW(L"BUTTON", L"Record", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                    214, 724, 140, 34, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceModeRecord)), g_hInst, nullptr);
        replace_checkbox_ = CreateWindowW(L"BUTTON", L"Replace managed REAPER tracks", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                          24, 770, 520, 36, hwnd_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceReplace)), g_hInst, nullptr);

        apply_button_ = CreateWindowW(L"BUTTON", L"Apply Draft", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      812, 764, 170, 46, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceOk)), g_hInst, nullptr);
        cancel_button_ = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      998, 764, 110, 46, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSourceCancel)), g_hInst, nullptr);

        ApplyFonts();
        CheckRadioButton(hwnd_,
                         kIdSourceModeSoundcheck,
                         kIdSourceModeRecord,
                         result_.setup_soundcheck ? kIdSourceModeSoundcheck : kIdSourceModeRecord);
        SendMessageW(replace_checkbox_, BM_SETCHECK,
                     result_.replace_existing ? BST_CHECKED : BST_UNCHECKED, 0);
        PopulateList();
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    void ApplyFonts() {
        SetWindowFontRecursive(hwnd_, font_);
        SendMessageW(count_label_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        SendMessageW(listbox_, LB_SETITEMHEIGHT, 0, WindowsUi::ScaleDip(24, dpi_));
    }

    bool RecreateFonts(UINT dpi) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        HFONT replacement_font = CreateUiFontForDpi(new_dpi, 105);
        HFONT replacement_title = CreateUiFontForDpi(new_dpi, 115, FW_SEMIBOLD);
        if (!replacement_font || !replacement_title) {
            if (replacement_font) {
                DeleteObject(replacement_font);
            }
            if (replacement_title) {
                DeleteObject(replacement_title);
            }
            return false;
        }

        HFONT old_font = font_;
        HFONT old_title = title_font_;
        dpi_ = new_dpi;
        font_ = replacement_font;
        title_font_ = replacement_title;
        ApplyFonts();
        if (old_font) {
            DeleteObject(old_font);
        }
        if (old_title) {
            DeleteObject(old_title);
        }
        return true;
    }

    void LayoutControls(int client_width_pixels, int client_height_pixels) {
        if (!hwnd_) {
            return;
        }
        const int client_width = UnscalePixels(client_width_pixels, dpi_);
        const int client_height = UnscalePixels(client_height_pixels, dpi_);
        const auto move = [this](HWND control, int x, int y, int width, int height) {
            MoveWindow(control,
                       WindowsUi::ScaleDip(x, dpi_),
                       WindowsUi::ScaleDip(y, dpi_),
                       WindowsUi::ScaleDip(width, dpi_),
                       WindowsUi::ScaleDip(height, dpi_),
                       TRUE);
        };
        const auto button_width = [this](HWND control, int minimum, int padding) {
            return ButtonWidthForLabelDip(control, minimum, padding, dpi_);
        };

        const int margin = 24;
        const int content_width = std::max(320, client_width - (margin * 2));
        const int intro_height = StaticHeightForTextDip(intro_, content_width, 48, 8, dpi_);
        const int action_height = 42;
        const int action_y = std::max(0, client_height - margin - action_height);
        const int replace_y = action_y - 48;
        const int mode_y = replace_y - 42;
        const int selection_y = mode_y - 50;
        const int list_y = margin + intro_height + 16;
        const int list_height = std::max(120, selection_y - list_y - 16);

        move(intro_, margin, margin, content_width, intro_height);
        move(listbox_, margin, list_y, content_width, list_height);

        const int select_all_width = button_width(select_all_, 132, 36);
        const int channels_width = button_width(channels_only_, 156, 36);
        const int clear_width = button_width(clear_button_, 96, 36);
        int selection_x = margin;
        move(select_all_, selection_x, selection_y, select_all_width, 38);
        selection_x += select_all_width + 12;
        move(channels_only_, selection_x, selection_y, channels_width, 38);
        selection_x += channels_width + 12;
        move(clear_button_, selection_x, selection_y, clear_width, 38);
        selection_x += clear_width + 20;
        move(count_label_, selection_x, selection_y + 4,
             std::max(120, client_width - margin - selection_x), 30);

        const int soundcheck_width = button_width(mode_soundcheck_, 140, 36);
        const int record_width = button_width(mode_record_, 110, 36);
        move(mode_soundcheck_, margin, mode_y, soundcheck_width, 34);
        move(mode_record_, margin + soundcheck_width + 16, mode_y, record_width, 34);
        move(replace_checkbox_, margin, replace_y, std::max(240, content_width / 2), 34);

        const int apply_width = button_width(apply_button_, 150, 40);
        const int cancel_width = button_width(cancel_button_, 100, 36);
        const int cancel_x = client_width - margin - cancel_width;
        move(cancel_button_, cancel_x, action_y, cancel_width, action_height);
        move(apply_button_, cancel_x - 12 - apply_width, action_y, apply_width, action_height);
    }

    LRESULT OnSize(int width, int height) {
        if (width > 0 && height > 0) {
            LayoutControls(width, height);
        }
        return 0;
    }

    LRESULT OnGetMinMaxInfo(MINMAXINFO* info) {
        SetMonitorClampedMinimumTrackSize(hwnd_, info, 720, 540, dpi_);
        return 0;
    }

    LRESULT OnDpiChanged(UINT dpi, const RECT* suggested_rect) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        if (!RecreateFonts(new_dpi)) {
            dpi_ = new_dpi;
        }
        if (suggested_rect) {
            SetWindowPos(hwnd_, nullptr,
                         suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left,
                         suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    LRESULT OnSystemSettingsChanged() {
        const UINT new_dpi = WindowDpi(hwnd_);
        if (!RecreateFonts(new_dpi)) {
            dpi_ = new_dpi;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    LRESULT OnCtlColor(HDC hdc) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(30, 30, 30));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    void PopulateList() {
        for (size_t i = 0; i < result_.channels.size(); ++i) {
            const auto& source = result_.channels[i];
            std::string kind = "SRC";
            switch (source.kind) {
                case SourceKind::Channel: kind = "CH"; break;
                case SourceKind::Bus: kind = "BUS"; break;
                case SourceKind::Main: kind = "MAIN"; break;
                case SourceKind::Matrix: kind = "MTX"; break;
            }
            std::string name = source.name.empty() ? (kind + " " + std::to_string(source.source_number)) : source.name;
            std::string line = kind + " " + std::to_string(source.source_number) + "  |  " + name;
            if (!source.source_group.empty() && source.source_input > 0) {
                line += "  |  " + source.source_group + ":" + std::to_string(source.source_input);
            }
            if (!source.soundcheck_capable) {
                line += "  |  Record only";
            }
            const std::wstring wide = ToWide(line);
            SendMessageW(listbox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
            if (source.selected) {
                SendMessageW(listbox_, LB_SETSEL, TRUE, static_cast<LPARAM>(i));
            }
        }
        UpdateSelectionCount();
    }

    void UpdateSelectionCount() {
        const LRESULT selected = SendMessageW(listbox_, LB_GETSELCOUNT, 0, 0);
        wchar_t buffer[96];
        std::swprintf(buffer, sizeof(buffer) / sizeof(wchar_t), L"%ld sources selected", static_cast<long>(selected));
        SetWindowTextW(count_label_, buffer);
    }

    void SelectMatching(bool channels_only) {
        const int count = static_cast<int>(result_.channels.size());
        for (int i = 0; i < count; ++i) {
            const bool enable = !channels_only || result_.channels[static_cast<size_t>(i)].kind == SourceKind::Channel;
            SendMessageW(listbox_, LB_SETSEL, enable ? TRUE : FALSE, i);
        }
        UpdateSelectionCount();
    }

    void CommitSelection() {
        std::vector<int> selected_indices(result_.channels.size());
        const LRESULT selected_count = SendMessageW(listbox_, LB_GETSELITEMS,
                                                   static_cast<WPARAM>(selected_indices.size()),
                                                   reinterpret_cast<LPARAM>(selected_indices.data()));
        std::set<int> selected_set;
        for (LRESULT i = 0; i < selected_count; ++i) {
            selected_set.insert(selected_indices[static_cast<size_t>(i)]);
        }
        for (size_t i = 0; i < result_.channels.size(); ++i) {
            result_.channels[i].selected = selected_set.count(static_cast<int>(i)) > 0;
        }
        result_.setup_soundcheck =
            (IsDlgButtonChecked(hwnd_, kIdSourceModeSoundcheck) == BST_CHECKED);
        result_.replace_existing =
            (SendMessageW(replace_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        result_.confirmed = true;
        done_ = true;
    }

    LRESULT OnCommand(WORD id, WORD notify_code) {
        switch (id) {
            case kIdSourceSelectAll:
                SelectMatching(false);
                return 0;
            case kIdSourceSelectChannels:
                SelectMatching(true);
                return 0;
            case kIdSourceClear:
                SendMessageW(listbox_, LB_SETSEL, FALSE, -1);
                UpdateSelectionCount();
                return 0;
            case kIdSourceList:
                if (notify_code == LBN_SELCHANGE) {
                    UpdateSelectionCount();
                }
                return 0;
            case kIdSourceOk:
                CommitSelection();
                return 0;
            case kIdSourceCancel:
                done_ = true;
                return 0;
            default:
                return 0;
        }
    }

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND listbox_ = nullptr;
    HWND replace_checkbox_ = nullptr;
    HWND count_label_ = nullptr;
    HWND intro_ = nullptr;
    HWND select_all_ = nullptr;
    HWND channels_only_ = nullptr;
    HWND clear_button_ = nullptr;
    HWND mode_soundcheck_ = nullptr;
    HWND mode_record_ = nullptr;
    HWND apply_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    UINT dpi_ = WindowsUi::kBaseDpi;
    HFONT font_ = nullptr;
    HFONT title_font_ = nullptr;
    bool done_ = false;
    SourcePickerResult result_;
};

class DebugLogPopup {
public:
    DebugLogPopup() = default;

    void Show(HWND owner, const std::wstring& initial_text) {
        owner_ = owner;
        RegisterClass();
        if (!hwnd_) {
            // The popup owns its DPI and font; borrowing the main window font
            // would make mixed-DPI monitor moves render at the wrong scale.
            dpi_ = WindowDpi(owner_);
            const RECT fitted = GetFittedDialogRect(owner_, 760, 420, dpi_);
            hwnd_ = CreateWindowExW(
                WS_EX_TOOLWINDOW,
                kDebugLogClassName,
                L"WINGuard Debug Log",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                fitted.left, fitted.top, fitted.right - fitted.left, fitted.bottom - fitted.top,
                owner_,
                nullptr,
                g_hInst,
                this);
        } else {
            RecreateFont(WindowDpi(hwnd_));
        }
        if (!hwnd_) {
            return;
        }
        SetText(initial_text);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }

    void SetText(const std::wstring& text) {
        latest_text_ = text;
        if (!edit_) {
            return;
        }
        if (ReadWindowText(edit_) != latest_text_) {
            SetWindowTextW(edit_, latest_text_.c_str());
            const int length = GetWindowTextLengthW(edit_);
            SendMessageW(edit_, EM_SETSEL, length, length);
            SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
        }
    }

    bool IsVisible() const {
        return hwnd_ && IsWindowVisible(hwnd_);
    }

private:
    static void RegisterClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = &DebugLogPopup::WndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = kDebugLogClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        DebugLogPopup* self = reinterpret_cast<DebugLogPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<DebugLogPopup*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) {
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        switch (msg) {
            case WM_CREATE: return self->OnCreate();
            case WM_SIZE: return self->OnSize(LOWORD(lparam), HIWORD(lparam));
            case WM_GETMINMAXINFO: return self->OnGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(lparam));
            case WM_DPICHANGED:
                return self->OnDpiChanged(HIWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            case WM_SETTINGCHANGE:
                return self->OnSystemSettingsChanged();
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                self->ReleaseFont();
                self->hwnd_ = nullptr;
                self->edit_ = nullptr;
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    LRESULT OnCreate() {
        RecreateFont(WindowDpi(hwnd_));
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                12, 12, 840, 380,
                                hwnd_, nullptr, g_hInst, nullptr);
        ApplyFont();
        SetText(latest_text_);
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    LRESULT OnSize(int width, int height) {
        if (width > 0 && height > 0) {
            LayoutControls(width, height);
        }
        return 0;
    }

    void LayoutControls(int width, int height) {
        if (!edit_) {
            return;
        }
        const int margin = WindowsUi::ScaleDip(12, dpi_);
        MoveWindow(edit_, margin, margin,
                   std::max(WindowsUi::ScaleDip(100, dpi_), width - (margin * 2)),
                   std::max(WindowsUi::ScaleDip(100, dpi_), height - (margin * 2)),
                   TRUE);
    }

    void ApplyFont() {
        if (edit_ && font_) {
            SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }

    bool RecreateFont(UINT dpi) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        HFONT replacement = CreateUiFontForDpi(new_dpi, 100, FW_NORMAL, true);
        if (!replacement) {
            return false;
        }
        HFONT old_font = font_;
        dpi_ = new_dpi;
        font_ = replacement;
        ApplyFont();
        if (old_font) {
            DeleteObject(old_font);
        }
        return true;
    }

    void ReleaseFont() {
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
    }

    LRESULT OnGetMinMaxInfo(MINMAXINFO* info) {
        SetMonitorClampedMinimumTrackSize(hwnd_, info, 480, 300, dpi_);
        return 0;
    }

    LRESULT OnDpiChanged(UINT dpi, const RECT* suggested_rect) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        if (!RecreateFont(new_dpi)) {
            dpi_ = new_dpi;
        }
        if (suggested_rect) {
            SetWindowPos(hwnd_, nullptr,
                         suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left,
                         suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    LRESULT OnSystemSettingsChanged() {
        const UINT new_dpi = WindowDpi(hwnd_);
        if (!RecreateFont(new_dpi)) {
            dpi_ = new_dpi;
        }
        return 0;
    }

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    UINT dpi_ = WindowsUi::kBaseDpi;
    HFONT font_ = nullptr;
    std::wstring latest_text_;
};

class AdoptionEditorDialog {
public:
    AdoptionEditorDialog(const std::vector<AdoptionEditorRow>& rows,
                         const std::vector<int>& available_channels,
                         const char* initial_output_mode)
        : rows_(rows),
          available_channels_(available_channels),
          output_mode_(initial_output_mode && std::string(initial_output_mode) == "CARD" ? "CARD" : "USB"),
          slot_overrides_(rows.size()) {}

    bool Run(std::string& output_mode_out,
             std::string& channel_overrides_spec_out,
             std::string& slot_overrides_spec_out,
             bool& apply_now_out) {
        RegisterClass();
        dpi_ = WindowDpi(g_hwndParent);
        const RECT fitted = GetFittedDialogRect(g_hwndParent, 900, 660, dpi_);
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                kAdoptionDialogClassName,
                                L"WINGuard Adoption Review",
                                WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_THICKFRAME,
                                fitted.left, fitted.top, fitted.right - fitted.left, fitted.bottom - fitted.top,
                                g_hwndParent,
                                nullptr,
                                g_hInst,
                                this);
        if (!hwnd_) {
            return false;
        }

        // Adoption is intentionally review-first. Keep the editor modal so the
        // parent cannot initiate a competing setup action during review.
        EnableWindow(g_hwndParent, FALSE);
        MSG msg{};
        while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (g_hwndParent) {
            EnableWindow(g_hwndParent, TRUE);
            SetForegroundWindow(g_hwndParent);
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        ReleaseFonts();
        if (!apply_now_) {
            apply_now_out = false;
            return false;
        }
        output_mode_out = output_mode_;
        channel_overrides_spec_out = BuildChannelOverrides();
        slot_overrides_spec_out = BuildSlotOverrides();
        apply_now_out = true;
        return true;
    }

private:
    enum : int {
        kIdModeUsb = 9001,
        kIdModeCard,
        kIdRowList,
        kIdChannelCombo,
        kIdSlotEdit,
        kIdClearSlot,
        kIdApply,
        kIdCancel
    };

    static void RegisterClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = &AdoptionEditorDialog::WndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = kAdoptionDialogClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        AdoptionEditorDialog* self = reinterpret_cast<AdoptionEditorDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<AdoptionEditorDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) {
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        switch (msg) {
            case WM_CREATE: return self->OnCreate();
            case WM_COMMAND: return self->OnCommand(LOWORD(wparam), HIWORD(wparam));
            case WM_SIZE: return self->OnSize(LOWORD(lparam), HIWORD(lparam));
            case WM_GETMINMAXINFO: return self->OnGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(lparam));
            case WM_DPICHANGED:
                return self->OnDpiChanged(HIWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            case WM_SETTINGCHANGE:
                return self->OnSystemSettingsChanged();
            case WM_CLOSE:
                self->done_ = true;
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    LRESULT OnCreate() {
        RecreateFonts(WindowDpi(hwnd_));

        title_label_ = CreateWindowW(L"STATIC", L"Adopt Existing REAPER Project For Virtual Soundcheck", WS_CHILD | WS_VISIBLE,
                      24, 20, 620, 30, hwnd_, nullptr, g_hInst, nullptr);
        intro_label_ = CreateWindowW(L"STATIC",
                      L"Review or override the proposed channel mapping before applying. Slot overrides are optional; leave them empty to keep automatic placement.",
                      WS_CHILD | WS_VISIBLE,
                      24, 56, 900, 42, hwnd_, nullptr, g_hInst, nullptr);
        connection_label_ = CreateWindowW(L"STATIC", L"Connection", WS_CHILD | WS_VISIBLE,
                      24, 112, 160, 24, hwnd_, nullptr, g_hInst, nullptr);
        const std::wstring connection_text = BuildConnectionSummary();
        connection_status_ = CreateWindowW(L"STATIC", connection_text.c_str(), WS_CHILD | WS_VISIBLE,
                                           200, 112, 720, 42, hwnd_, nullptr, g_hInst, nullptr);
        output_label_ = CreateWindowW(L"STATIC", L"Output Mode", WS_CHILD | WS_VISIBLE,
                      24, 166, 160, 24, hwnd_, nullptr, g_hInst, nullptr);
        mode_usb_ = CreateWindowW(L"BUTTON", L"USB", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                                  200, 162, 100, 30, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdModeUsb)), g_hInst, nullptr);
        mode_card_ = CreateWindowW(L"BUTTON", L"CARD", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                   314, 162, 110, 30, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdModeCard)), g_hInst, nullptr);
        CheckRadioButton(hwnd_, kIdModeUsb, kIdModeCard, output_mode_ == "CARD" ? kIdModeCard : kIdModeUsb);

        tracks_label_ = CreateWindowW(L"STATIC", L"Tracks To Adopt", WS_CHILD | WS_VISIBLE,
                      24, 218, 180, 24, hwnd_, nullptr, g_hInst, nullptr);
        row_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                                    24, 250, 560, 400, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRowList)), g_hInst, nullptr);
        channel_label_ = CreateWindowW(L"STATIC", L"WING Channel", WS_CHILD | WS_VISIBLE,
                      612, 250, 140, 24, hwnd_, nullptr, g_hInst, nullptr);
        channel_combo_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       612, 280, 240, 240, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChannelCombo)), g_hInst, nullptr);
        slot_label_ = CreateWindowW(L"STATIC", L"Playback Slot Override", WS_CHILD | WS_VISIBLE,
                      612, 340, 220, 24, hwnd_, nullptr, g_hInst, nullptr);
        slot_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                     612, 372, 160, 32, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSlotEdit)), g_hInst, nullptr);
        clear_slot_button_ = CreateWindowW(L"BUTTON", L"Clear Override", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                           782, 372, 160, 32, hwnd_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdClearSlot)), g_hInst, nullptr);
        assignment_hint_ = CreateWindowW(L"STATIC",
                                         L"Choose a row, then adjust the WING channel or enter a slot like 9 or 9-10. Stereo rows should use an odd-start pair.",
                                         WS_CHILD | WS_VISIBLE,
                                         612, 426, 320, 72, hwnd_, nullptr, g_hInst, nullptr);
        apply_button_ = CreateWindowW(L"BUTTON", L"Apply Adoption", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                      700, 674, 150, 42, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdApply)), g_hInst, nullptr);
        cancel_button_ = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       864, 674, 92, 42, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), g_hInst, nullptr);

        ApplyFonts();

        for (int channel_number : available_channels_) {
            const std::wstring label = L"CH" + std::to_wstring(channel_number);
            SendMessageW(channel_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        PopulateRows();
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    void ApplyFonts() {
        SetWindowFontRecursive(hwnd_, font_);
        SendMessageW(title_label_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        SendMessageW(connection_status_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_font_), TRUE);
        SendMessageW(row_list_, LB_SETITEMHEIGHT, 0, WindowsUi::ScaleDip(24, dpi_));
        SendMessageW(apply_button_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    }

    bool RecreateFonts(UINT dpi) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        const std::array<HFONT, 3> replacements = {
            CreateUiFontForDpi(new_dpi, 105),
            CreateUiFontForDpi(new_dpi, 125, FW_SEMIBOLD),
            CreateUiFontForDpi(new_dpi, 100, FW_NORMAL, true)
        };
        if (std::any_of(replacements.begin(), replacements.end(), [](HFONT font) { return !font; })) {
            for (HFONT replacement : replacements) {
                if (replacement) {
                    DeleteObject(replacement);
                }
            }
            return false;
        }

        const std::array<HFONT, 3> old_fonts = {font_, title_font_, mono_font_};
        dpi_ = new_dpi;
        font_ = replacements[0];
        title_font_ = replacements[1];
        mono_font_ = replacements[2];
        ApplyFonts();
        for (HFONT old_font : old_fonts) {
            if (old_font) {
                DeleteObject(old_font);
            }
        }
        return true;
    }

    void ReleaseFonts() {
        const std::array<HFONT*, 3> fonts = {&font_, &title_font_, &mono_font_};
        for (HFONT* font : fonts) {
            if (*font) {
                DeleteObject(*font);
                *font = nullptr;
            }
        }
    }

    void LayoutControls(int client_width_pixels, int client_height_pixels) {
        if (!hwnd_ || !title_label_) {
            return;
        }
        const int client_width = UnscalePixels(client_width_pixels, dpi_);
        const int client_height = UnscalePixels(client_height_pixels, dpi_);
        const auto move = [this](HWND control, int x, int y, int width, int height) {
            MoveWindow(control,
                       WindowsUi::ScaleDip(x, dpi_),
                       WindowsUi::ScaleDip(y, dpi_),
                       WindowsUi::ScaleDip(width, dpi_),
                       WindowsUi::ScaleDip(height, dpi_),
                       TRUE);
        };
        const auto button_width = [this](HWND control, int minimum, int padding) {
            return ButtonWidthForLabelDip(control, minimum, padding, dpi_);
        };

        const int margin = 24;
        const int content_width = std::max(320, client_width - (margin * 2));
        const int title_y = 20;
        const int intro_y = 58;
        const int intro_height = StaticHeightForTextDip(intro_label_, content_width, 42, 6, dpi_);
        const int connection_y = intro_y + intro_height + 12;
        const int connection_label_width = 140;
        const int connection_x = margin + connection_label_width + 16;
        const int connection_width = std::max(160, client_width - margin - connection_x);
        const int connection_height = StaticHeightForTextDip(connection_status_, connection_width, 42, 6, dpi_);
        const int output_y = connection_y + connection_height + 12;
        const int tracks_y = output_y + 48;
        const int list_y = tracks_y + 30;
        const int action_height = 42;
        const int action_y = std::max(0, client_height - margin - action_height);
        const int list_height = std::max(120, action_y - list_y - 24);
        const int panel_gap = 24;
        const int right_minimum = 260;
        const int list_width = std::max(300, std::min(560, content_width - panel_gap - right_minimum));
        const int panel_x = margin + list_width + panel_gap;
        const int panel_width = std::max(right_minimum, client_width - margin - panel_x);

        move(title_label_, margin, title_y, content_width, 30);
        move(intro_label_, margin, intro_y, content_width, intro_height);
        move(connection_label_, margin, connection_y, connection_label_width, 28);
        move(connection_status_, connection_x, connection_y, connection_width, connection_height);
        move(output_label_, margin, output_y, connection_label_width, 30);
        move(mode_usb_, connection_x, output_y - 2, 88, 32);
        move(mode_card_, connection_x + 104, output_y - 2, 96, 32);
        move(tracks_label_, margin, tracks_y, list_width, 26);
        move(row_list_, margin, list_y, list_width, list_height);

        move(channel_label_, panel_x, list_y, panel_width, 26);
        move(channel_combo_, panel_x, list_y + 30, panel_width, 200);
        move(slot_label_, panel_x, list_y + 88, panel_width, 26);
        const int clear_width = button_width(clear_slot_button_, 128, 34);
        const int edit_width = std::max(80, panel_width - clear_width - 12);
        move(slot_edit_, panel_x, list_y + 118, edit_width, 34);
        move(clear_slot_button_, panel_x + edit_width + 12, list_y + 118, clear_width, 34);
        const int hint_y = list_y + 168;
        const int hint_height = StaticHeightForTextDip(assignment_hint_, panel_width, 64, 6, dpi_);
        move(assignment_hint_, panel_x, hint_y, panel_width,
             std::min(hint_height, std::max(36, action_y - hint_y - 12)));

        const int apply_width = button_width(apply_button_, 156, 40);
        const int cancel_width = button_width(cancel_button_, 100, 36);
        const int cancel_x = client_width - margin - cancel_width;
        move(cancel_button_, cancel_x, action_y, cancel_width, action_height);
        move(apply_button_, cancel_x - 12 - apply_width, action_y, apply_width, action_height);
    }

    LRESULT OnSize(int width, int height) {
        if (width > 0 && height > 0) {
            LayoutControls(width, height);
        }
        return 0;
    }

    LRESULT OnGetMinMaxInfo(MINMAXINFO* info) {
        SetMonitorClampedMinimumTrackSize(hwnd_, info, 760, 560, dpi_);
        return 0;
    }

    LRESULT OnDpiChanged(UINT dpi, const RECT* suggested_rect) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        if (!RecreateFonts(new_dpi)) {
            dpi_ = new_dpi;
        }
        if (suggested_rect) {
            SetWindowPos(hwnd_, nullptr,
                         suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left,
                         suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    LRESULT OnSystemSettingsChanged() {
        const UINT new_dpi = WindowDpi(hwnd_);
        if (!RecreateFonts(new_dpi)) {
            dpi_ = new_dpi;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        return 0;
    }

    void PopulateRows() {
        SendMessageW(row_list_, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < rows_.size(); ++i) {
            SendMessageW(row_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(BuildRowLabel(i).c_str()));
        }
        if (!rows_.empty()) {
            SendMessageW(row_list_, LB_SETCURSEL, 0, 0);
            selected_index_ = 0;
            SyncSelectionControls();
        }
    }

    std::wstring BuildConnectionSummary() const {
        auto& extension = ReaperExtension::Instance();
        const auto& config = extension.GetConfig();
        if (extension.IsConnected()) {
            return L"Connected to WING at " + ToWide(config.wing_ip.empty() ? std::string("(unknown)") : config.wing_ip) +
                   L". Adoption review uses live channel metadata.";
        }
        return L"No live WING connection is active. Close this review and reconnect before applying adoption.";
    }

    std::wstring BuildSuggestedSlotLabel(const AdoptionEditorRow& row) const {
        if (row.suggested_slot_start <= 0) {
            return L"Auto";
        }
        if (row.suggested_slot_end > row.suggested_slot_start) {
            return std::to_wstring(row.suggested_slot_start) + L"-" + std::to_wstring(row.suggested_slot_end);
        }
        return std::to_wstring(row.suggested_slot_start);
    }

    std::wstring BuildRowLabel(size_t index) const {
        const auto& row = rows_[index];
        std::wstring line = std::to_wstring(row.track_index) + L". " + ToWide(row.track_name);
        line += row.stereo_like ? L" | Stereo" : L" | Mono";
        line += L" | Suggest CH" + std::to_wstring(row.suggested_channel);
        line += L" | Now CH" + std::to_wstring(row.assigned_channel);
        line += L" | Slot ";
        line += slot_overrides_[index].empty() ? BuildSuggestedSlotLabel(row) : slot_overrides_[index];
        return line;
    }

    void SyncSelectionControls() {
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(rows_.size())) {
            return;
        }
        const auto& row = rows_[static_cast<size_t>(selected_index_)];
        int combo_index = 0;
        for (size_t i = 0; i < available_channels_.size(); ++i) {
            if (available_channels_[i] == row.assigned_channel) {
                combo_index = static_cast<int>(i);
                break;
            }
        }
        SendMessageW(channel_combo_, CB_SETCURSEL, combo_index, 0);
        SetWindowTextW(slot_edit_, slot_overrides_[static_cast<size_t>(selected_index_)].c_str());
    }

    void UpdateSelectedRowLabel() {
        if (selected_index_ < 0 || selected_index_ >= static_cast<int>(rows_.size())) {
            return;
        }
        SendMessageW(row_list_, LB_DELETESTRING, selected_index_, 0);
        SendMessageW(row_list_, LB_INSERTSTRING, selected_index_, reinterpret_cast<LPARAM>(BuildRowLabel(static_cast<size_t>(selected_index_)).c_str()));
        SendMessageW(row_list_, LB_SETCURSEL, selected_index_, 0);
    }

    std::string BuildChannelOverrides() const {
        std::ostringstream out;
        bool first = true;
        for (const auto& row : rows_) {
            if (row.assigned_channel == row.suggested_channel) {
                continue;
            }
            if (!first) {
                out << ";";
            }
            out << row.track_index << "=CH" << row.assigned_channel;
            first = false;
        }
        return out.str();
    }

    std::string BuildSlotOverrides() const {
        std::ostringstream out;
        bool first = true;
        for (size_t i = 0; i < rows_.size(); ++i) {
            const std::wstring& override_value = slot_overrides_[i];
            if (override_value.empty()) {
                continue;
            }
            if (!first) {
                out << ";";
            }
            out << rows_[i].track_index << "=" << ToUtf8(override_value);
            first = false;
        }
        return out.str();
    }

    LRESULT OnCommand(WORD id, WORD notify_code) {
        switch (id) {
            case kIdModeUsb:
                output_mode_ = "USB";
                return 0;
            case kIdModeCard:
                output_mode_ = "CARD";
                return 0;
            case kIdRowList:
                if (notify_code == LBN_SELCHANGE) {
                    selected_index_ = static_cast<int>(SendMessageW(row_list_, LB_GETCURSEL, 0, 0));
                    SyncSelectionControls();
                }
                return 0;
            case kIdChannelCombo:
                if (notify_code == CBN_SELCHANGE && selected_index_ >= 0) {
                    const int combo_index = static_cast<int>(SendMessageW(channel_combo_, CB_GETCURSEL, 0, 0));
                    if (combo_index >= 0 && combo_index < static_cast<int>(available_channels_.size())) {
                        rows_[static_cast<size_t>(selected_index_)].assigned_channel = available_channels_[static_cast<size_t>(combo_index)];
                        UpdateSelectedRowLabel();
                    }
                }
                return 0;
            case kIdSlotEdit:
                if (notify_code == EN_CHANGE && selected_index_ >= 0) {
                    slot_overrides_[static_cast<size_t>(selected_index_)] = ReadWindowText(slot_edit_);
                    UpdateSelectedRowLabel();
                }
                return 0;
            case kIdClearSlot:
                if (selected_index_ >= 0) {
                    slot_overrides_[static_cast<size_t>(selected_index_)].clear();
                    SetWindowTextW(slot_edit_, L"");
                    UpdateSelectedRowLabel();
                }
                return 0;
            case kIdApply:
                apply_now_ = true;
                done_ = true;
                return 0;
            case kIdCancel:
                apply_now_ = false;
                done_ = true;
                return 0;
            default:
                return 0;
        }
    }

    HWND hwnd_ = nullptr;
    HWND title_label_ = nullptr;
    HWND intro_label_ = nullptr;
    HWND connection_label_ = nullptr;
    HWND connection_status_ = nullptr;
    HWND output_label_ = nullptr;
    HWND mode_usb_ = nullptr;
    HWND mode_card_ = nullptr;
    HWND tracks_label_ = nullptr;
    HWND row_list_ = nullptr;
    HWND channel_label_ = nullptr;
    HWND channel_combo_ = nullptr;
    HWND slot_label_ = nullptr;
    HWND slot_edit_ = nullptr;
    HWND clear_slot_button_ = nullptr;
    HWND assignment_hint_ = nullptr;
    HWND apply_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    UINT dpi_ = WindowsUi::kBaseDpi;
    HFONT font_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    std::vector<AdoptionEditorRow> rows_;
    std::vector<int> available_channels_;
    std::vector<std::wstring> slot_overrides_;
    std::string output_mode_;
    int selected_index_ = -1;
    bool done_ = false;
    bool apply_now_ = false;
};

class WingConnectorWindowsDialog {
public:
    static void Show() {
        if (!instance_) {
            instance_ = new WingConnectorWindowsDialog();
        }
        instance_->ShowInternal();
    }

private:
    struct PageLayoutState {
        // Page content, scroll offsets, and decorative geometry are stored in
        // DIPs. Only MoveWindow and WM_ERASEBKGND convert them to pixels.
        HWND hwnd = nullptr;
        int content_height = 0;
        int scroll_y = 0;
        RECT intro_rect{};
        int divider_y = -1;
    };

    struct PageContext {
        WingConnectorWindowsDialog* owner = nullptr;
        PageLayoutState* state = nullptr;
    };

    struct StatusSnapshot {
        HeaderStatusVisual console;
        HeaderStatusVisual validation;
        HeaderStatusVisual recorder;
        HeaderStatusVisual midi;
        HeaderStatusVisual console_tab;
        HeaderStatusVisual reaper_tab;
        HeaderStatusVisual wing_tab;
        HeaderStatusVisual control_tab;
        std::wstring pending_summary;
        COLORREF pending_color = RGB(110, 110, 110);
        std::wstring readiness_detail;
        COLORREF readiness_color = RGB(110, 110, 110);
        std::wstring footer;
        bool can_apply = false;
        bool can_discard = false;
        bool can_toggle = false;
        std::wstring apply_label;
        std::wstring toggle_label;
    };

    static WingConnectorWindowsDialog* instance_;

    static void RegisterClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        WNDCLASSW wc{};
        wc.lpfnWndProc = &WingConnectorWindowsDialog::WndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = kDialogClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassW(&wc);

        WNDCLASSW page_wc{};
        page_wc.lpfnWndProc = &WingConnectorWindowsDialog::PageWndProc;
        page_wc.hInstance = g_hInst;
        page_wc.lpszClassName = kPageClassName;
        page_wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        page_wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassW(&page_wc);

        WNDCLASSW logo_wc{};
        logo_wc.lpfnWndProc = &WingConnectorWindowsDialog::LogoWndProc;
        logo_wc.hInstance = g_hInst;
        logo_wc.lpszClassName = kLogoClassName;
        logo_wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        logo_wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassW(&logo_wc);
        registered = true;
    }

    static LRESULT CALLBACK PageWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* context = reinterpret_cast<PageContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            context = reinterpret_cast<PageContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        }
        // Page windows own scrolling and painting, but commands and control
        // colors stay centralized in the main dialog state machine.
        HWND parent = GetParent(hwnd);
        switch (msg) {
            case WM_VSCROLL:
            case WM_MOUSEWHEEL:
            case WM_SIZE:
                if (context && context->owner && context->state) {
                    return context->owner->HandlePageMessage(context->state, msg, wparam, lparam);
                }
                break;
            case WM_ERASEBKGND:
                if (context && context->owner && context->state) {
                    return context->owner->OnPageEraseBackground(context->state, reinterpret_cast<HDC>(wparam));
                }
                break;
            case WM_COMMAND:
            case WM_NOTIFY:
            case WM_CTLCOLORSTATIC:
                if (parent) {
                    return SendMessageW(parent, msg, wparam, lparam);
                }
                break;
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    static LRESULT CALLBACK LogoWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rect{};
                GetClientRect(hwnd, &rect);

                HBRUSH background = CreateSolidBrush(RGB(242, 242, 242));
                FillRect(hdc, &rect, background);
                DeleteObject(background);

                const std::wstring logo_path = ResolveLogoPath();
                bool image_drawn = false;
                if (!logo_path.empty() && EnsureGdiplusToken() != 0) {
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    Gdiplus::Image image(logo_path.c_str());
                    if (image.GetLastStatus() == Gdiplus::Ok) {
                        graphics.DrawImage(&image,
                                           0,
                                           0,
                                           rect.right - rect.left,
                                           rect.bottom - rect.top);
                        image_drawn = true;
                    }
                }

                if (!image_drawn) {
                    RECT inner = rect;
                    InflateRect(&inner, -8, -8);
                    HBRUSH accent = CreateSolidBrush(RGB(28, 114, 184));
                    FillRect(hdc, &inner, accent);
                    DeleteObject(accent);

                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    HFONT fallback_font = CreateUiFont(hwnd, 170, FW_BOLD);
                    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, fallback_font));
                    DrawTextW(hdc, L"WG", -1, &inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, old_font);
                    DeleteObject(fallback_font);
                }

                EndPaint(hwnd, &ps);
                return 0;
            }
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<WingConnectorWindowsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<WingConnectorWindowsDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) {
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        switch (msg) {
            case WM_CREATE: return self->OnCreate();
            case WM_COMMAND: return self->OnCommand(LOWORD(wparam), HIWORD(wparam));
            case WM_NOTIFY: return self->OnNotify(reinterpret_cast<NMHDR*>(lparam));
            case WM_TIMER: return self->OnTimer(static_cast<UINT_PTR>(wparam));
            case kMsgAsyncScanComplete: return self->OnAsyncScanComplete(reinterpret_cast<AsyncScanResult*>(lparam));
            case kMsgAsyncConnectComplete: return self->OnAsyncConnectComplete(reinterpret_cast<AsyncConnectResult*>(lparam));
            case kMsgAsyncSourcesComplete: return self->OnAsyncSourcesComplete(reinterpret_cast<AsyncSourcesResult*>(lparam));
            case kMsgAsyncApplyPlanComplete: return self->OnAsyncApplyPlanComplete(reinterpret_cast<AsyncApplyPlanResult*>(lparam));
            case kMsgAsyncToggleComplete: return self->OnAsyncToggleComplete(reinterpret_cast<AsyncToggleResult*>(lparam));
            case kMsgAsyncValidationComplete: return self->OnAsyncValidationComplete(reinterpret_cast<AsyncValidationResult*>(lparam));
            case WM_CTLCOLORSTATIC: return self->OnCtlColor(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));
            case WM_ERASEBKGND: return self->OnEraseBackground(reinterpret_cast<HDC>(wparam));
            case WM_SIZE: return self->OnSize(LOWORD(lparam), HIWORD(lparam));
            case WM_GETMINMAXINFO: return self->OnGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(lparam));
            case WM_DPICHANGED:
                return self->OnDpiChanged(HIWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            case WM_SETTINGCHANGE:
                return self->OnSystemSettingsChanged();
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd, kRefreshTimerId);
                ReaperExtension::Instance().SetLogCallback({});
                self->ReleaseUiResources();
                self->hwnd_ = nullptr;
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    void ShowInternal() {
        RegisterClass();
        if (!hwnd_) {
            const RECT preferred = GetPreferredWindowRect(WindowDpi(g_hwndParent));
            hwnd_ = CreateWindowExW(
                WS_EX_TOOLWINDOW,
                kDialogClassName,
                L"WINGuard",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
                preferred.left, preferred.top,
                preferred.right - preferred.left,
                preferred.bottom - preferred.top,
                g_hwndParent,
                nullptr,
                g_hInst,
                this);
        }
        if (!hwnd_) {
            ShowMessageBox("Failed to create the Windows WINGuard dialog.",
                           "WINGuard",
                           0);
            return;
        }
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        RefreshAll();
    }

    void ApplyFonts() {
        SetWindowFontRecursive(hwnd_, font_);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_font_), TRUE);
        SendMessageW(subtitle_, WM_SETFONT, reinterpret_cast<WPARAM>(subtle_font_), TRUE);
        SendMessageW(header_console_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(header_validation_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(header_recorder_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(header_midi_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(tab_, WM_SETFONT, reinterpret_cast<WPARAM>(tab_font_), TRUE);
        SendMessageW(console_section_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(reaper_section_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(auto_trigger_section_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(wing_section_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(control_section_icon_, WM_SETFONT, reinterpret_cast<WPARAM>(icon_font_), TRUE);
        SendMessageW(console_section_header_, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
        SendMessageW(reaper_section_header_, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
        SendMessageW(auto_trigger_header_, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
        SendMessageW(wing_section_header_, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
        SendMessageW(control_section_header_, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
        SendMessageW(support_section_header_, WM_SETFONT, reinterpret_cast<WPARAM>(section_font_), TRUE);
        SendMessageW(tab_status_console_, WM_SETFONT, reinterpret_cast<WPARAM>(small_bold_font_), TRUE);
        SendMessageW(tab_status_reaper_, WM_SETFONT, reinterpret_cast<WPARAM>(small_bold_font_), TRUE);
        SendMessageW(tab_status_wing_, WM_SETFONT, reinterpret_cast<WPARAM>(small_bold_font_), TRUE);
        SendMessageW(tab_status_control_, WM_SETFONT, reinterpret_cast<WPARAM>(small_bold_font_), TRUE);
        SendMessageW(header_console_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(header_validation_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(header_recorder_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(header_midi_status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        for (HWND intro : {console_intro_, reaper_intro_, wing_intro_, control_intro_}) {
            SendMessageW(intro, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        for (HWND helper : {console_help_discovery_, console_help_manual_, console_footer_,
                            reaper_output_help_, reaper_toggle_help_, auto_trigger_detail_,
                            wing_placeholder_body_, control_placeholder_body_, midi_summary_,
                            midi_detail_, support_detail_}) {
            SendMessageW(helper, WM_SETFONT, reinterpret_cast<WPARAM>(subtle_font_), TRUE);
        }
        SendMessageW(footer_status_, WM_SETFONT, reinterpret_cast<WPARAM>(subtle_font_), TRUE);
        SendMessageW(wing_placeholder_body_, WM_SETFONT, reinterpret_cast<WPARAM>(subtle_font_), TRUE);
        SendMessageW(control_placeholder_body_, WM_SETFONT, reinterpret_cast<WPARAM>(subtle_font_), TRUE);
        SendMessageW(support_detail_, WM_SETFONT, reinterpret_cast<WPARAM>(subtle_font_), TRUE);
        SendMessageW(debug_log_view_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_font_), TRUE);
        SendMessageW(auto_trigger_meter_label_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_font_), TRUE);
    }

    bool RecreateFonts(UINT dpi) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        // Replace the complete role set transactionally. Controls are rebound
        // before old GDI handles are deleted, preventing stale-font handles if
        // one allocation fails during a DPI transition.
        const std::array<HFONT, 8> replacement_fonts = {
            CreateUiFontForDpi(new_dpi, 105),
            CreateUiFontForDpi(new_dpi, 170, FW_SEMIBOLD),
            CreateUiFontForDpi(new_dpi, 105, FW_SEMIBOLD),
            CreateUiFontForDpi(new_dpi, 125, FW_SEMIBOLD),
            CreateUiFontForDpi(new_dpi, 110, FW_SEMIBOLD),
            CreateUiFontForDpi(new_dpi, 100),
            CreateUiFontForDpi(new_dpi, 100, FW_NORMAL, true),
            CreateSymbolFontForDpi(new_dpi, 14, FW_SEMIBOLD)
        };
        if (std::any_of(replacement_fonts.begin(), replacement_fonts.end(), [](HFONT font) { return !font; })) {
            for (HFONT replacement : replacement_fonts) {
                if (replacement) {
                    DeleteObject(replacement);
                }
            }
            return false;
        }

        const std::array<HFONT, 8> old_fonts = {
            font_, bold_font_, small_bold_font_, section_font_,
            tab_font_, subtle_font_, mono_font_, icon_font_
        };
        dpi_ = new_dpi;
        font_ = replacement_fonts[0];
        bold_font_ = replacement_fonts[1];
        small_bold_font_ = replacement_fonts[2];
        section_font_ = replacement_fonts[3];
        tab_font_ = replacement_fonts[4];
        subtle_font_ = replacement_fonts[5];
        mono_font_ = replacement_fonts[6];
        icon_font_ = replacement_fonts[7];
        ApplyFonts();

        for (HFONT old : old_fonts) {
            if (old) {
                DeleteObject(old);
            }
        }
        return true;
    }

    void ReleaseUiResources() {
        const std::array<HFONT*, 8> fonts = {
            &font_, &bold_font_, &small_bold_font_, &section_font_,
            &tab_font_, &subtle_font_, &mono_font_, &icon_font_
        };
        for (HFONT* font : fonts) {
            if (*font) {
                DeleteObject(*font);
                *font = nullptr;
            }
        }
        const std::array<HBRUSH*, 5> brushes = {
            &banner_brush_, &status_panel_brush_, &card_brush_, &body_brush_, &border_brush_
        };
        for (HBRUSH* brush : brushes) {
            if (*brush) {
                DeleteObject(*brush);
                *brush = nullptr;
            }
        }
    }

    LRESULT OnDpiChanged(UINT dpi, const RECT* suggested_rect) {
        const UINT new_dpi = dpi == 0 ? WindowsUi::kBaseDpi : dpi;
        if (!RecreateFonts(new_dpi)) {
            dpi_ = new_dpi;
        }
        if (suggested_rect) {
            SetWindowPos(hwnd_,
                         nullptr,
                         suggested_rect->left,
                         suggested_rect->top,
                         suggested_rect->right - suggested_rect->left,
                         suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    }

    LRESULT OnSystemSettingsChanged() {
        const UINT new_dpi = WindowDpi(hwnd_);
        if (!RecreateFonts(new_dpi)) {
            dpi_ = new_dpi;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    }

    LRESULT OnCreate() {
        dpi_ = WindowDpi(hwnd_);
        RecreateFonts(dpi_);
        banner_brush_ = CreateSolidBrush(RGB(242, 242, 242));
        status_panel_brush_ = CreateSolidBrush(RGB(250, 250, 250));
        card_brush_ = CreateSolidBrush(RGB(246, 246, 246));
        body_brush_ = CreateSolidBrush(RGB(255, 255, 255));
        border_brush_ = CreateSolidBrush(RGB(219, 219, 219));

        banner_group_ = CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE,
                                      12, 10, 820, 156, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBannerGroup)), g_hInst, nullptr);
        logo_ = CreateWindowW(kLogoClassName, L"", WS_CHILD | WS_VISIBLE,
                              36, 30, 144, 144, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLogo)), g_hInst, nullptr);
        title_ = CreateWindowW(L"STATIC", L"WINGuard", WS_CHILD | WS_VISIBLE,
                               194, 48, 360, 44, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTitle)), g_hInst, nullptr);
        subtitle_ = CreateWindowW(L"STATIC", L"Guard every take. Faster setup, safer record(w)ing!",
                                  WS_CHILD | WS_VISIBLE,
                                  194, 102, 520, 34, hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSubtitle)), g_hInst, nullptr);

        status_group_ = CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE,
                                      472, 26, 340, 96, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatusGroup)), g_hInst, nullptr);
        header_console_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                             490, 42, 20, 18, hwnd_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderConsoleIcon)), g_hInst, nullptr);
        header_console_status_ = CreateWindowW(L"STATIC", L"Console: Offline", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                               514, 42, 278, 18, hwnd_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderConsoleStatus)), g_hInst, nullptr);
        header_validation_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                                490, 62, 20, 18, hwnd_,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderValidationIcon)), g_hInst, nullptr);
        header_validation_status_ = CreateWindowW(L"STATIC", L"REAPER: Not Ready", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                                  514, 62, 278, 18, hwnd_,
                                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderValidationStatus)), g_hInst, nullptr);
        header_recorder_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                              490, 82, 20, 18, hwnd_,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderRecorderIcon)), g_hInst, nullptr);
        header_recorder_status_ = CreateWindowW(L"STATIC", L"WING recorder: Off", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                                514, 82, 278, 18, hwnd_,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderRecorderStatus)), g_hInst, nullptr);
        header_midi_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                          490, 102, 20, 18, hwnd_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderMidiIcon)), g_hInst, nullptr);
        header_midi_status_ = CreateWindowW(L"STATIC", L"Control: Off", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                            514, 102, 278, 18, hwnd_,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeaderMidiStatus)), g_hInst, nullptr);

        tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FIXEDWIDTH,
                               12, 154, 820, 560, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTab)), g_hInst, nullptr);
        for (const wchar_t* title : {L"Console", L"Reaper", L"Wing", L"Control Integration"}) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(title);
            TabCtrl_InsertItem(tab_, TabCtrl_GetItemCount(tab_), &item);
        }
        page_frame_ = CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE,
                                    12, 206, 820, 560, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPageFrame)), g_hInst, nullptr);
        CreatePages();
        CreateConsolePage();
        CreateReaperPage();
        CreateWingPlaceholderPage();
        CreateControlPlaceholderPage();

        footer_status_ = CreateWindowW(L"STATIC", L"Windows Phase 1: native connection and REAPER setup surfaces now match the macOS layout direction.",
                                       WS_CHILD | WS_VISIBLE,
                                       16, 724, 804, 20, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdFooterStatus)), g_hInst, nullptr);

        ApplyFonts();
        ShowWindow(console_section_icon_, SW_HIDE);
        ShowWindow(reaper_section_icon_, SW_HIDE);
        ShowWindow(auto_trigger_section_icon_, SW_HIDE);
        ShowWindow(wing_section_icon_, SW_HIDE);
        ShowWindow(control_section_icon_, SW_HIDE);
        ShowWindow(banner_group_, SW_HIDE);
        ShowWindow(status_group_, SW_HIDE);
        ShowWindow(page_frame_, SW_HIDE);

        SyncPendingSettingsFromConfig();
        SyncAutoTriggerFromConfig();
        SelectOutputMode(ReaperExtension::Instance().GetConfig().soundcheck_output_mode);
        SyncAutoTriggerControlsFromPending();
        SyncWingControlsFromPending();
        SyncControlTabFromPending();
        ReaperExtension::Instance().SetLogCallback([this](const std::string& message) {
            std::lock_guard<std::mutex> lock(log_buffer_mutex_);
            pending_log_buffer_ += CleanLogMessage(message);
            if (!pending_log_buffer_.empty() && pending_log_buffer_.back() != L'\n') {
                pending_log_buffer_ += L"\r\n";
            }
        });
        pending_output_mode_ = CurrentOutputMode();
        SetTimer(hwnd_, kRefreshTimerId, kRefreshTimerMs, nullptr);
        RECT client_rect{};
        GetClientRect(hwnd_, &client_rect);
        LayoutControls(client_rect.right - client_rect.left, client_rect.bottom - client_rect.top);
        SelectTab(0);
        RefreshDiscoveryControls(false);
        RefreshAll();
        if (!ReaperExtension::Instance().IsConnected()) {
            RunScan(false);
        }
        return 0;
    }

    void CreatePages() {
        const int width = 100;
        const int height = 100;
        page_contexts_[0] = {this, &console_page_state_};
        page_contexts_[1] = {this, &reaper_page_state_};
        page_contexts_[2] = {this, &wing_page_state_};
        page_contexts_[3] = {this, &control_page_state_};
        page_console_ = CreateWindowW(kPageClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                      0, 0, width, height,
                                      hwnd_, nullptr, g_hInst, &page_contexts_[0]);
        page_reaper_ = CreateWindowW(kPageClassName, L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
                                     0, 0, width, height,
                                     hwnd_, nullptr, g_hInst, &page_contexts_[1]);
        page_wing_ = CreateWindowW(kPageClassName, L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
                                   0, 0, width, height,
                                   hwnd_, nullptr, g_hInst, &page_contexts_[2]);
        page_control_ = CreateWindowW(kPageClassName, L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
                                      0, 0, width, height,
                                      hwnd_, nullptr, g_hInst, &page_contexts_[3]);
        console_page_state_.hwnd = page_console_;
        reaper_page_state_.hwnd = page_reaper_;
        wing_page_state_.hwnd = page_wing_;
        control_page_state_.hwnd = page_control_;
    }

    void CreateConsolePage() {
        console_intro_ = CreateWindowW(L"STATIC",
                                       L"Connect to a Wing, choose where recording channels go, and get live or soundcheck playback ready without cable gymnastics.",
                                       WS_CHILD | WS_VISIBLE,
                                       20, 18, 740, 36, page_console_, nullptr, g_hInst, nullptr);
        console_section_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                              20, 64, 18, 18, page_console_,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdConsoleSectionIcon)), g_hInst, nullptr);

        console_section_header_ = CreateWindowW(L"STATIC", L"\U0001F310 Connection", WS_CHILD | WS_VISIBLE,
                                                46, 64, 200, 20, page_console_, nullptr, g_hInst, nullptr);
        tab_status_console_ = CreateWindowW(L"STATIC", L"Inactive", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                            640, 64, 120, 20, page_console_,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdConsoleStatusChip)), g_hInst, nullptr);

        console_label_ = CreateWindowW(L"STATIC", L"Wing Console:", WS_CHILD | WS_VISIBLE,
                                       20, 108, 110, 20, page_console_, nullptr, g_hInst, nullptr);
        wing_combo_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                    140, 104, 470, 240, page_console_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdWingCombo)), g_hInst, nullptr);
        scan_button_ = CreateWindowW(L"BUTTON", L"Scan", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     630, 104, 120, 28, page_console_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdScanButton)), g_hInst, nullptr);

        console_help_discovery_ = CreateWindowW(L"STATIC", L"Pick a discovered Wing to fill the connection details automatically.",
                                                WS_CHILD | WS_VISIBLE,
                                                140, 136, 540, 18, page_console_, nullptr, g_hInst, nullptr);

        console_manual_ip_label_ = CreateWindowW(L"STATIC", L"Manual IP:", WS_CHILD | WS_VISIBLE,
                                                 20, 182, 110, 20, page_console_, nullptr, g_hInst, nullptr);
        manual_ip_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                          140, 178, 260, 24, page_console_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdManualIpEdit)), g_hInst, nullptr);
        console_help_manual_ = CreateWindowW(L"STATIC",
                                             L"If you already know the console IP, skip the scan and connect directly.",
                                             WS_CHILD | WS_VISIBLE,
                                             140, 208, 540, 18, page_console_, nullptr, g_hInst, nullptr);

        connect_button_ = CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                        630, 246, 120, 30, page_console_,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdConnectButton)), g_hInst, nullptr);

        console_footer_ = CreateWindowW(L"STATIC",
                                        L"Console connection and recording-readiness status stay pinned in the header above, visible from every tab.",
                                        WS_CHILD | WS_VISIBLE,
                                        20, 248, 560, 24, page_console_, nullptr, g_hInst, nullptr);
    }

    void CreateReaperPage() {
        reaper_intro_ = CreateWindowW(L"STATIC",
                                      L"Prepare REAPER for live recording and virtual soundcheck here: choose USB or CARD routing, stage and apply the source layout, switch prepared channels between live inputs and playback, and use Auto Trigger when you want signal-driven starts.",
                                      WS_CHILD | WS_VISIBLE,
                                      20, 18, 760, 40, page_reaper_, nullptr, g_hInst, nullptr);
        reaper_section_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                             20, 66, 18, 18, page_reaper_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReaperSectionIcon)), g_hInst, nullptr);

        reaper_section_header_ = CreateWindowW(L"STATIC", L"\U0001F39A Recording and Soundcheck", WS_CHILD | WS_VISIBLE,
                                               46, 66, 280, 20, page_reaper_, nullptr, g_hInst, nullptr);
        tab_status_reaper_ = CreateWindowW(L"STATIC", L"Inactive", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                           640, 66, 120, 20, page_reaper_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReaperStatusChip)), g_hInst, nullptr);

        reaper_output_label_ = CreateWindowW(L"STATIC", L"Recording I/O Mode:", WS_CHILD | WS_VISIBLE,
                                             20, 108, 130, 20, page_reaper_, nullptr, g_hInst, nullptr);
        output_usb_radio_ = CreateWindowW(L"BUTTON", L"USB", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                          200, 106, 80, 22, page_reaper_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReaperOutputUsb)), g_hInst, nullptr);
        output_card_radio_ = CreateWindowW(L"BUTTON", L"CARD", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                           290, 106, 80, 22, page_reaper_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReaperOutputCard)), g_hInst, nullptr);
        reaper_output_help_ = CreateWindowW(L"STATIC",
                                            L"Choose where the Wing sends the recording channels. USB is the usual direct-to-computer path; CARD uses the Wing audio card route.",
                                            WS_CHILD | WS_VISIBLE,
                                            200, 134, 520, 30, page_reaper_, nullptr, g_hInst, nullptr);

        pending_summary_ = CreateWindowW(L"STATIC",
                                         L"No pending setup changes. Choose sources for a new setup, or change recording mode to stage a rebuild of the current managed setup.",
                                         WS_CHILD | WS_VISIBLE,
                                         200, 184, 520, 54, page_reaper_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPendingSummary)), g_hInst, nullptr);
        readiness_detail_ = CreateWindowW(L"STATIC",
                                          L"Connect to a Wing to validate the current managed setup, then rebuild it only when routing or recording mode needs to change.",
                                          WS_CHILD | WS_VISIBLE,
                                          200, 250, 520, 70, page_reaper_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReadinessDetail)), g_hInst, nullptr);

        choose_sources_button_ = CreateWindowW(L"BUTTON", L"Choose Sources...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               200, 336, 160, 32, page_reaper_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChooseSourcesButton)), g_hInst, nullptr);
        apply_setup_button_ = CreateWindowW(L"BUTTON", L"Apply Setup", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                            380, 336, 180, 32, page_reaper_,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdApplySetupButton)), g_hInst, nullptr);
        discard_setup_button_ = CreateWindowW(L"BUTTON", L"Discard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                              570, 336, 120, 32, page_reaper_,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDiscardSetupButton)), g_hInst, nullptr);
        toggle_soundcheck_button_ = CreateWindowW(L"BUTTON", L"Live Mode", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                  200, 388, 220, 32, page_reaper_,
                                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdToggleSoundcheckButton)), g_hInst, nullptr);
        reaper_toggle_help_ = CreateWindowW(L"STATIC",
                                            L"After setup is validated, this flips prepared channels between live inputs and REAPER playback. One button, less panic.",
                                            WS_CHILD | WS_VISIBLE,
                                            200, 428, 520, 30, page_reaper_, nullptr, g_hInst, nullptr);

        auto_trigger_section_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                                   20, 500, 18, 18, page_reaper_,
                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerSectionIcon)), g_hInst, nullptr);
        auto_trigger_header_ = CreateWindowW(L"STATIC", L"\u26A1 Auto Trigger", WS_CHILD | WS_VISIBLE,
                                             46, 500, 240, 24, page_reaper_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerHeader)), g_hInst, nullptr);
        auto_trigger_detail_ = CreateWindowW(L"STATIC",
                                             L"Trigger controls wake up after live setup validates, because they depend on the prepared recording path.",
                                             WS_CHILD | WS_VISIBLE,
                                             200, 540, 540, 34, page_reaper_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerDetail)), g_hInst, nullptr);
        auto_trigger_enable_label_ = CreateWindowW(L"STATIC", L"Enable Trigger:", WS_CHILD | WS_VISIBLE,
                                                   48, 596, 150, 24, page_reaper_, nullptr, g_hInst, nullptr);
        auto_trigger_enable_off_ = CreateWindowW(L"BUTTON", L"OFF", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                                 200, 594, 84, 28, page_reaper_,
                                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerEnableOff)), g_hInst, nullptr);
        auto_trigger_enable_on_ = CreateWindowW(L"BUTTON", L"ON", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                                296, 594, 84, 28, page_reaper_,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerEnableOn)), g_hInst, nullptr);
        auto_trigger_monitor_label_ = CreateWindowW(L"STATIC", L"Monitor Track:", WS_CHILD | WS_VISIBLE,
                                                    48, 644, 150, 24, page_reaper_, nullptr, g_hInst, nullptr);
        auto_trigger_monitor_combo_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                                    200, 640, 260, 240, page_reaper_,
                                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerMonitorTrackCombo)), g_hInst, nullptr);
        auto_trigger_mode_label_ = CreateWindowW(L"STATIC", L"Trigger Mode:", WS_CHILD | WS_VISIBLE,
                                                 48, 692, 150, 24, page_reaper_, nullptr, g_hInst, nullptr);
        auto_trigger_mode_warning_ = CreateWindowW(L"BUTTON", L"WARNING", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                                   200, 690, 120, 28, page_reaper_,
                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerModeWarning)), g_hInst, nullptr);
        auto_trigger_mode_record_ = CreateWindowW(L"BUTTON", L"RECORD", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                                  332, 690, 110, 28, page_reaper_,
                                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerModeRecord)), g_hInst, nullptr);
        auto_trigger_threshold_label_ = CreateWindowW(L"STATIC", L"Trigger Threshold:", WS_CHILD | WS_VISIBLE,
                                                      48, 740, 150, 24, page_reaper_, nullptr, g_hInst, nullptr);
        auto_trigger_threshold_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                       200, 736, 100, 30, page_reaper_,
                                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerThresholdEdit)), g_hInst, nullptr);
        auto_trigger_hold_label_ = CreateWindowW(L"STATIC", L"Hold Time:", WS_CHILD | WS_VISIBLE,
                                                 332, 740, 120, 24, page_reaper_, nullptr, g_hInst, nullptr);
        auto_trigger_hold_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                  456, 736, 100, 30, page_reaper_,
                                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerHoldEdit)), g_hInst, nullptr);
        auto_trigger_meter_label_ = CreateWindowW(L"STATIC", L"Trigger level: -- dBFS", WS_CHILD | WS_VISIBLE,
                                                  200, 776, 260, 24, page_reaper_,
                                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerMeterLabel)), g_hInst, nullptr);
        apply_auto_trigger_button_ = CreateWindowW(L"BUTTON", L"Apply Auto Trigger Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                   200, 816, 250, 40, page_reaper_,
                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdApplyAutoTriggerButton)), g_hInst, nullptr);
        discard_auto_trigger_button_ = CreateWindowW(L"BUTTON", L"Discard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                     464, 816, 140, 40, page_reaper_,
                                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDiscardAutoTriggerButton)), g_hInst, nullptr);
        auto_trigger_hint_ = CreateWindowW(L"STATIC",
                                           L"Pending Auto Trigger changes stay parked until you apply them.",
                                           WS_CHILD | WS_VISIBLE,
                                           200, 872, 540, 52, page_reaper_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoTriggerHint)), g_hInst, nullptr);
    }

    void CreateWingPlaceholderPage() {
        wing_intro_ = CreateWindowW(L"STATIC",
                                    L"Manage the Wing-side recorder behavior here: target selection, source feed, and whether the recorder follows REAPER-triggered automation.",
                                    WS_CHILD | WS_VISIBLE,
                                    24, 24, 760, 40, page_wing_, nullptr, g_hInst, nullptr);
        wing_section_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                           24, 88, 18, 18, page_wing_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdWingSectionIcon)), g_hInst, nullptr);
        wing_section_header_ = CreateWindowW(L"STATIC", L"\U0001F4FC Recorder Coordination", WS_CHILD | WS_VISIBLE,
                                             48, 88, 300, 26, page_wing_, nullptr, g_hInst, nullptr);
        tab_status_wing_ = CreateWindowW(L"STATIC", L"Inactive", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                         640, 88, 120, 20, page_wing_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdWingStatusChip)), g_hInst, nullptr);
        wing_enable_label_ = CreateWindowW(L"STATIC", L"Recorder Control:", WS_CHILD | WS_VISIBLE,
                                           48, 144, 160, 22, page_wing_, nullptr, g_hInst, nullptr);
        wing_enable_off_ = CreateWindowW(L"BUTTON", L"OFF", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                         220, 142, 76, 26, page_wing_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderEnableOff)), g_hInst, nullptr);
        wing_enable_on_ = CreateWindowW(L"BUTTON", L"ON", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                        304, 142, 76, 26, page_wing_,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderEnableOn)), g_hInst, nullptr);
        wing_target_label_ = CreateWindowW(L"STATIC", L"Recorder Target:", WS_CHILD | WS_VISIBLE,
                                           48, 198, 160, 22, page_wing_, nullptr, g_hInst, nullptr);
        wing_target_wlive_ = CreateWindowW(L"BUTTON", L"SD (WING-LIVE)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                           220, 196, 150, 26, page_wing_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderTargetWLive)), g_hInst, nullptr);
        wing_target_usb_ = CreateWindowW(L"BUTTON", L"USB Recorder", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                         384, 196, 150, 26, page_wing_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderTargetUsb)), g_hInst, nullptr);
        wing_pair_label_ = CreateWindowW(L"STATIC", L"Recorder Source Pair:", WS_CHILD | WS_VISIBLE,
                                         48, 252, 160, 22, page_wing_, nullptr, g_hInst, nullptr);
        wing_pair_1_ = CreateWindowW(L"BUTTON", L"MAIN 1/2", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                     220, 250, 110, 26, page_wing_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderPair1)), g_hInst, nullptr);
        wing_pair_3_ = CreateWindowW(L"BUTTON", L"MAIN 3/4", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                     338, 250, 110, 26, page_wing_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderPair3)), g_hInst, nullptr);
        wing_pair_5_ = CreateWindowW(L"BUTTON", L"MAIN 5/6", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                     456, 250, 110, 26, page_wing_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderPair5)), g_hInst, nullptr);
        wing_pair_7_ = CreateWindowW(L"BUTTON", L"MAIN 7/8", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                     574, 250, 110, 26, page_wing_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderPair7)), g_hInst, nullptr);
        wing_follow_label_ = CreateWindowW(L"STATIC", L"Follow Auto-Trigger:", WS_CHILD | WS_VISIBLE,
                                           48, 306, 160, 22, page_wing_, nullptr, g_hInst, nullptr);
        wing_follow_off_ = CreateWindowW(L"BUTTON", L"OFF", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                         220, 304, 76, 26, page_wing_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderFollowOff)), g_hInst, nullptr);
        wing_follow_on_ = CreateWindowW(L"BUTTON", L"ON", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                        304, 304, 76, 26, page_wing_,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderFollowOn)), g_hInst, nullptr);
        wing_placeholder_body_ = CreateWindowW(L"STATIC",
                                               L"Recorder coordination is using the currently applied settings.",
                                               WS_CHILD | WS_VISIBLE,
                                               220, 356, 620, 52, page_wing_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRecorderDetail)), g_hInst, nullptr);
        apply_recorder_button_ = CreateWindowW(L"BUTTON", L"Apply Recorder Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               220, 430, 220, 38, page_wing_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdApplyRecorderButton)), g_hInst, nullptr);
        discard_recorder_button_ = CreateWindowW(L"BUTTON", L"Discard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                 454, 430, 140, 38, page_wing_,
                                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDiscardRecorderButton)), g_hInst, nullptr);
    }

    void CreateControlPlaceholderPage() {
        control_intro_ = CreateWindowW(L"STATIC",
                                       L"Map Wing controls into REAPER here, and keep diagnostics close by when you need to verify what the plugin is doing.",
                                       WS_CHILD | WS_VISIBLE,
                                       24, 24, 760, 44, page_control_, nullptr, g_hInst, nullptr);
        control_section_icon_ = CreateWindowW(L"STATIC", L"\x25CF", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                              24, 88, 18, 18, page_control_,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdControlSectionIcon)), g_hInst, nullptr);
        control_section_header_ = CreateWindowW(L"STATIC", L"\U0001F39B Wing Control Integration", WS_CHILD | WS_VISIBLE,
                                                48, 88, 340, 26, page_control_, nullptr, g_hInst, nullptr);
        tab_status_control_ = CreateWindowW(L"STATIC", L"Inactive", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                            640, 88, 120, 20, page_control_,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdControlStatusChip)), g_hInst, nullptr);
        control_enable_label_ = CreateWindowW(L"STATIC", L"Wing Control Enabled:", WS_CHILD | WS_VISIBLE,
                                              48, 144, 170, 22, page_control_, nullptr, g_hInst, nullptr);
        midi_actions_off_ = CreateWindowW(L"BUTTON", L"OFF", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP,
                                          220, 142, 76, 26, page_control_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdMidiActionsOff)), g_hInst, nullptr);
        midi_actions_on_ = CreateWindowW(L"BUTTON", L"ON", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
                                         304, 142, 76, 26, page_control_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdMidiActionsOn)), g_hInst, nullptr);
        midi_summary_ = CreateWindowW(L"STATIC", L"No pending MIDI shortcut changes.", WS_CHILD | WS_VISIBLE,
                                      220, 198, 620, 42, page_control_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdMidiSummary)), g_hInst, nullptr);
        midi_detail_ = CreateWindowW(L"STATIC", L"MIDI shortcuts are disabled.", WS_CHILD | WS_VISIBLE,
                                     220, 252, 620, 52, page_control_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdMidiDetail)), g_hInst, nullptr);
        warning_layer_label_ = CreateWindowW(L"STATIC", L"Warning CC Layer:", WS_CHILD | WS_VISIBLE,
                                             48, 328, 170, 22, page_control_, nullptr, g_hInst, nullptr);
        warning_layer_combo_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                             220, 324, 180, 260, page_control_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdWarningLayerCombo)), g_hInst, nullptr);
        for (int i = 1; i <= 16; ++i) {
            std::wstring label = L"Layer " + std::to_wstring(i);
            SendMessageW(warning_layer_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        control_placeholder_body_ = CreateWindowW(L"STATIC",
                                                  L"Bridge routing still uses the applied config, MIDI output selection, and mappings from the existing workflow. This tab now exposes the main REAPER control integration state instead of a placeholder.",
                                                  WS_CHILD | WS_VISIBLE,
                                                  220, 370, 620, 64, page_control_, nullptr, g_hInst, nullptr);
        apply_midi_button_ = CreateWindowW(L"BUTTON", L"Apply MIDI Shortcuts", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                           220, 454, 200, 38, page_control_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdApplyMidiButton)), g_hInst, nullptr);
        discard_midi_button_ = CreateWindowW(L"BUTTON", L"Discard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                             434, 454, 140, 38, page_control_,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDiscardMidiButton)), g_hInst, nullptr);
        support_section_header_ = CreateWindowW(L"STATIC", L"\U0001F6E0 Support and Diagnostics", WS_CHILD | WS_VISIBLE,
                                                48, 526, 340, 26, page_control_, nullptr, g_hInst, nullptr);
        support_detail_ = CreateWindowW(L"STATIC",
                                        L"Use the debug log when things get weird, or when you want receipts for discovery, routing, validation, and recorder activity.",
                                        WS_CHILD | WS_VISIBLE,
                                        220, 564, 620, 42, page_control_, nullptr, g_hInst, nullptr);
        open_debug_log_button_ = CreateWindowW(L"BUTTON", L"Open Debug Log", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               220, 620, 170, 38, page_control_,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOpenDebugLogButton)), g_hInst, nullptr);
        clear_debug_log_button_ = CreateWindowW(L"BUTTON", L"Clear Log", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                404, 620, 130, 38, page_control_,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdClearDebugLogButton)), g_hInst, nullptr);
        debug_log_view_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                          220, 676, 620, 220, page_control_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDebugLogView)), g_hInst, nullptr);
    }

    LRESULT OnGetMinMaxInfo(MINMAXINFO* info) {
        SetMonitorClampedMinimumTrackSize(hwnd_, info,
                                         WindowsUi::kMainMinWindowWidthDip,
                                         WindowsUi::kMainMinWindowHeightDip,
                                         WindowDpi(hwnd_));
        return 0;
    }

    LRESULT OnSize(int width, int height) {
        if (!hwnd_ || width <= 0 || height <= 0) {
            return 0;
        }
        LayoutControls(width, height);
        return 0;
    }

    LRESULT OnEraseBackground(HDC hdc) {
        if (!hdc || !hwnd_) {
            return 0;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        FillRect(hdc, &client, body_brush_);
        FillRect(hdc, &banner_rect_, banner_brush_);
        // Custom painting is deliberately limited to decorative surfaces.
        // Interactive controls remain native for focus, keyboard, theme, and
        // accessibility behavior.
        HPEN border_pen = CreatePen(PS_SOLID, std::max(1, WindowsUi::ScaleDip(1, dpi_)), RGB(219, 219, 219));
        HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, border_pen));
        HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, status_panel_brush_));
        const int radius = WindowsUi::ScaleDip(10, dpi_);
        RoundRect(hdc,
                  status_panel_rect_.left,
                  status_panel_rect_.top,
                  status_panel_rect_.right,
                  status_panel_rect_.bottom,
                  radius,
                  radius);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(border_pen);
        return 1;
    }

    LRESULT OnPageEraseBackground(PageLayoutState* page, HDC hdc) {
        if (!page || !page->hwnd || !hdc) {
            return 0;
        }
        RECT client{};
        GetClientRect(page->hwnd, &client);
        FillRect(hdc, &client, body_brush_);

        // Card/divider positions live in document coordinates and must move in
        // lockstep with child controls when the page scrolls.
        RECT intro = page->intro_rect;
        intro.top -= page->scroll_y;
        intro.bottom -= page->scroll_y;
        intro.left = WindowsUi::ScaleDip(intro.left, dpi_);
        intro.top = WindowsUi::ScaleDip(intro.top, dpi_);
        intro.right = WindowsUi::ScaleDip(intro.right, dpi_);
        intro.bottom = WindowsUi::ScaleDip(intro.bottom, dpi_);
        HPEN border_pen = CreatePen(PS_SOLID, std::max(1, WindowsUi::ScaleDip(1, dpi_)), RGB(219, 219, 219));
        HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, border_pen));
        HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, card_brush_));
        const int radius = WindowsUi::ScaleDip(8, dpi_);
        RoundRect(hdc, intro.left, intro.top, intro.right, intro.bottom, radius, radius);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(border_pen);

        if (page->divider_y >= 0) {
            RECT divider{
                WindowsUi::ScaleDip(20, dpi_),
                WindowsUi::ScaleDip(page->divider_y - page->scroll_y, dpi_),
                client.right - WindowsUi::ScaleDip(20, dpi_),
                WindowsUi::ScaleDip(page->divider_y - page->scroll_y + 1, dpi_)
            };
            FillRect(hdc, &divider, border_brush_);
        }
        return 1;
    }

    LRESULT HandlePageMessage(PageLayoutState* page, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (!page || !page->hwnd) {
            return 0;
        }
        if (msg == WM_SIZE) {
            return 0;
        }

        // Scroll ranges stay in DIPs. ScrollWindowEx receives the scaled pixel
        // delta so Windows moves existing child windows without accumulating
        // rounding drift in the logical scroll position.
        int next_scroll = page->scroll_y;
        const int max_scroll = std::max(0, page->content_height - PageViewportHeight(*page));
        if (msg == WM_MOUSEWHEEL) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            next_scroll = std::clamp(page->scroll_y - ((delta / WHEEL_DELTA) * kScrollLineStep * 2), 0, max_scroll);
        } else if (msg == WM_VSCROLL) {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(page->hwnd, SB_VERT, &info);
            next_scroll = page->scroll_y;
            switch (LOWORD(wparam)) {
                case SB_LINEUP: next_scroll -= kScrollLineStep; break;
                case SB_LINEDOWN: next_scroll += kScrollLineStep; break;
                case SB_PAGEUP: next_scroll -= static_cast<int>(info.nPage); break;
                case SB_PAGEDOWN: next_scroll += static_cast<int>(info.nPage); break;
                case SB_THUMBPOSITION:
                case SB_THUMBTRACK: next_scroll = HIWORD(wparam); break;
                case SB_TOP: next_scroll = 0; break;
                case SB_BOTTOM: next_scroll = max_scroll; break;
                default: break;
            }
            next_scroll = std::clamp(next_scroll, 0, max_scroll);
        }

        if (next_scroll != page->scroll_y) {
            const int delta_y = page->scroll_y - next_scroll;
            page->scroll_y = next_scroll;
            UpdatePageScroll(*page, PageViewportHeight(*page));
            ScrollWindowEx(page->hwnd,
                           0,
                           WindowsUi::ScaleDip(delta_y, dpi_),
                           nullptr,
                           nullptr,
                           nullptr,
                           nullptr,
                           SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN);
        }
        return 0;
    }

    int PageViewportHeight(const PageLayoutState& page) const {
        RECT rect{};
        GetClientRect(page.hwnd, &rect);
        const int height = UnscalePixels(static_cast<int>(rect.bottom - rect.top), dpi_);
        return std::max(0, height);
    }

    void UpdatePageScroll(PageLayoutState& page, int viewport_height) {
        const int max_scroll = std::max(0, page.content_height - viewport_height);
        page.scroll_y = std::clamp(page.scroll_y, 0, max_scroll);
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = std::max(0, page.content_height - 1);
        info.nPage = static_cast<UINT>(std::max(1, viewport_height));
        info.nPos = page.scroll_y;
        SetScrollInfo(page.hwnd, SB_VERT, &info, TRUE);
    }

    int PageY(const PageLayoutState& page, int content_y) const {
        return content_y - page.scroll_y;
    }

    void MoveWindowDip(HWND control, int x, int y, int width, int height, BOOL repaint = TRUE) const {
        MoveWindow(control,
                   WindowsUi::ScaleDip(x, dpi_),
                   WindowsUi::ScaleDip(y, dpi_),
                   WindowsUi::ScaleDip(width, dpi_),
                   WindowsUi::ScaleDip(height, dpi_),
                   repaint);
    }

    bool ShouldRefreshValidation() const {
        if (validation_in_progress_) {
            return false;
        }
        const DWORD now = GetTickCount();
        if (!validation_snapshot_ready_) {
            return true;
        }
        return (now - last_validation_tick_) >= kValidationRefreshIntervalMs;
    }

    void InvalidateValidationSnapshot() {
        validation_in_progress_ = false;
        ++validation_generation_;
        validation_snapshot_ready_ = false;
        last_validation_tick_ = 0;
        latest_validation_state_ = ValidationState::NotReady;
        latest_validation_details_.clear();
    }

    void LayoutControls(int client_width_pixels, int client_height_pixels) {
        // Convert the Win32 client rectangle once; every constant below is a
        // logical design token shared with the macOS hierarchy.
        const int client_width = UnscalePixels(client_width_pixels, dpi_);
        const int client_height = UnscalePixels(client_height_pixels, dpi_);
        const auto MoveWindow = [this](HWND control, int x, int y, int width, int height, BOOL repaint) {
            MoveWindowDip(control, x, y, width, height, repaint);
        };
        const auto ButtonWidthForLabel = [this](HWND control, int minimum, int padding = 34) {
            return ButtonWidthForLabelDip(control, minimum, padding, dpi_);
        };
        const auto StaticHeightForText = [this](HWND control, int width, int minimum, int padding = 0) {
            return StaticHeightForTextDip(control, width, minimum, padding, dpi_);
        };

        const int outer_margin = 12;
        const auto vertical = WindowsUi::CalculateMainVerticalLayout(client_height, WindowsUi::kBaseDpi);
        const int header_y = vertical.header_y;
        const int header_height = vertical.header_height;
        const int header_width = std::max(320, client_width);
        const int status_panel_width = 340;
        const int status_panel_height = 80;
        const int status_panel_x = header_width - status_panel_width - 12;
        const int status_panel_y = header_y + 16;
        banner_rect_ = RECT{
            0,
            WindowsUi::ScaleDip(header_y, dpi_),
            WindowsUi::ScaleDip(header_width, dpi_),
            WindowsUi::ScaleDip(header_y + header_height, dpi_)
        };
        status_panel_rect_ = RECT{
            WindowsUi::ScaleDip(status_panel_x, dpi_),
            WindowsUi::ScaleDip(status_panel_y, dpi_),
            WindowsUi::ScaleDip(status_panel_x + status_panel_width, dpi_),
            WindowsUi::ScaleDip(status_panel_y + status_panel_height, dpi_)
        };

        MoveWindow(banner_group_, 0, header_y, header_width, header_height, TRUE);
        MoveWindow(logo_, 20, header_y + 30, 40, 40, TRUE);
        const int brand_x = 70;
        const int brand_width = std::max(120, status_panel_x - brand_x - 14);
        MoveWindow(title_, brand_x, header_y + 27, brand_width, 28, TRUE);
        MoveWindow(subtitle_, brand_x, header_y + 58, brand_width, 22, TRUE);
        ShowWindow(subtitle_, brand_width >= 260 ? SW_SHOW : SW_HIDE);
        MoveWindow(status_group_, status_panel_x, status_panel_y, status_panel_width, status_panel_height, TRUE);

        const int status_icon_x = status_panel_x + 12;
        const int status_text_x = status_panel_x + 34;
        const int status_text_width = status_panel_width - 46;
        MoveWindow(header_console_icon_, status_icon_x, status_panel_y + 4, 16, 18, TRUE);
        MoveWindow(header_console_status_, status_text_x, status_panel_y + 4, status_text_width, 18, TRUE);
        MoveWindow(header_validation_icon_, status_icon_x, status_panel_y + 22, 16, 18, TRUE);
        MoveWindow(header_validation_status_, status_text_x, status_panel_y + 22, status_text_width, 18, TRUE);
        MoveWindow(header_recorder_icon_, status_icon_x, status_panel_y + 40, 16, 18, TRUE);
        MoveWindow(header_recorder_status_, status_text_x, status_panel_y + 40, status_text_width, 18, TRUE);
        MoveWindow(header_midi_icon_, status_icon_x, status_panel_y + 58, 16, 18, TRUE);
        MoveWindow(header_midi_status_, status_text_x, status_panel_y + 58, status_text_width, 18, TRUE);

        const int footer_height = vertical.footer_height;
        const int footer_y = vertical.footer_y;
        MoveWindow(footer_status_, outer_margin + 4, footer_y, std::max(100, client_width - (outer_margin * 2) - 8), footer_height, TRUE);

        const int tab_y = vertical.tab_y;
        const int tab_button_height = vertical.tab_height;
        const int page_y = vertical.page_y;
        const int page_height = vertical.page_height;
        // Use a real tab control rather than painted surrogate buttons. This is
        // the native Windows equivalent of NSTabView and preserves accessibility.
        MoveWindow(tab_, outer_margin, tab_y, std::max(100, client_width - (outer_margin * 2)), tab_button_height, TRUE);
        TabCtrl_SetItemSize(tab_, (std::max(400, client_width - (outer_margin * 2))) / 4, tab_button_height - 4);
        const int shell_width = std::max(100, client_width - (outer_margin * 2));
        MoveWindow(page_frame_, outer_margin, page_y, shell_width, page_height, TRUE);

        const int page_x = outer_margin + 10;
        const int inner_page_y = page_y + 10;
        const int page_width = std::max(100, shell_width - 20);
        const int inner_page_height = std::max(0, page_height - 20);

        MoveWindow(page_console_, page_x, inner_page_y, page_width, inner_page_height, TRUE);
        MoveWindow(page_reaper_, page_x, inner_page_y, page_width, inner_page_height, TRUE);
        MoveWindow(page_wing_, page_x, inner_page_y, page_width, inner_page_height, TRUE);
        MoveWindow(page_control_, page_x, inner_page_y, page_width, inner_page_height, TRUE);

        // Cross-platform form contract: 20-DIP margin, 180-DIP label column,
        // and controls beginning at x=220 inside the compact content surface.
        const int page_margin = 20;
        const int label_x = 20;
        const int control_x = 220;
        const int page_right = page_width - page_margin;
        const int status_chip_w = 140;
        const int status_chip_x = page_right - status_chip_w;
        const int content_w = page_width - (page_margin * 2);
        const int viewport_height = inner_page_height;
        console_page_state_.content_height = std::max(viewport_height, 760);
        reaper_page_state_.content_height = std::max(viewport_height, 1680);
        wing_page_state_.content_height = std::max(viewport_height, 780);
        control_page_state_.content_height = std::max(viewport_height, 1150);
        UpdatePageScroll(console_page_state_, viewport_height);
        UpdatePageScroll(reaper_page_state_, viewport_height);
        UpdatePageScroll(wing_page_state_, viewport_height);
        UpdatePageScroll(control_page_state_, viewport_height);

        const int standard_text_width = std::max(160, page_width - control_x - 40);
        const int button_row_gap = 18;
        const int section_gap = 32;
        const int line_gap = 16;

        int console_y = 28;
        const int console_intro_h = StaticHeightForText(console_intro_, content_w - 28, 56, 8);
        console_page_state_.intro_rect = RECT{page_margin, console_y, page_margin + content_w, console_y + console_intro_h};
        MoveWindow(console_intro_, page_margin + 14, PageY(console_page_state_, console_y + 8), content_w - 28, console_intro_h - 16, TRUE);
        console_y += console_intro_h + 26;
        MoveWindow(console_section_icon_, page_margin, PageY(console_page_state_, console_y + 4), 22, 22, TRUE);
        MoveWindow(console_section_header_, page_margin, PageY(console_page_state_, console_y), 330, 34, TRUE);
        MoveWindow(tab_status_console_, status_chip_x, PageY(console_page_state_, console_y), status_chip_w, 30, TRUE);
        console_y += 52;
        MoveWindow(console_label_, label_x, PageY(console_page_state_, console_y + 6), 180, 30, TRUE);
        const int scan_width = ButtonWidthForLabel(scan_button_, 96, 30);
        const int wing_combo_width = std::max(360, page_width - control_x - scan_width - 64);
        MoveWindow(wing_combo_, control_x, PageY(console_page_state_, console_y), wing_combo_width, 360, TRUE);
        MoveWindow(scan_button_, control_x + wing_combo_width + 12, PageY(console_page_state_, console_y), scan_width, 32, TRUE);
        console_y += 42;
        const int discovery_h = StaticHeightForText(console_help_discovery_, standard_text_width, 34, 6);
        MoveWindow(console_help_discovery_, control_x, PageY(console_page_state_, console_y), standard_text_width, discovery_h, TRUE);
        console_y += discovery_h + line_gap;
        MoveWindow(console_manual_ip_label_, label_x, PageY(console_page_state_, console_y + 6), 180, 30, TRUE);
        const int manual_ip_width = std::min(480, page_width - control_x - 240);
        MoveWindow(manual_ip_edit_, control_x, PageY(console_page_state_, console_y), manual_ip_width, 30, TRUE);
        console_y += 42;
        const int manual_h = StaticHeightForText(console_help_manual_, standard_text_width, 52, 8);
        MoveWindow(console_help_manual_, control_x, PageY(console_page_state_, console_y), standard_text_width, manual_h, TRUE);
        console_y += manual_h + 24;
        const int connect_width = ButtonWidthForLabel(connect_button_, 112, 30);
        MoveWindow(connect_button_, control_x + manual_ip_width - connect_width, PageY(console_page_state_, console_y), connect_width, 32, TRUE);
        console_y += 48;
        const int console_footer_h = StaticHeightForText(console_footer_, page_width - 220, 72, 10);
        MoveWindow(console_footer_, page_margin, PageY(console_page_state_, console_y), page_width - 220, console_footer_h, TRUE);
        console_page_state_.content_height = console_y + console_footer_h + 48;

        int reaper_y = 28;
        const int reaper_intro_h = StaticHeightForText(reaper_intro_, content_w - 28, 56, 8);
        reaper_page_state_.intro_rect = RECT{page_margin, reaper_y, page_margin + content_w, reaper_y + reaper_intro_h};
        MoveWindow(reaper_intro_, page_margin + 14, PageY(reaper_page_state_, reaper_y + 8), content_w - 28, reaper_intro_h - 16, TRUE);
        reaper_y += reaper_intro_h + 24;
        MoveWindow(reaper_section_icon_, page_margin, PageY(reaper_page_state_, reaper_y + 4), 22, 22, TRUE);
        MoveWindow(reaper_section_header_, page_margin, PageY(reaper_page_state_, reaper_y), 420, 34, TRUE);
        MoveWindow(tab_status_reaper_, status_chip_x, PageY(reaper_page_state_, reaper_y), status_chip_w, 30, TRUE);
        reaper_y += 52;
        MoveWindow(reaper_output_label_, label_x, PageY(reaper_page_state_, reaper_y + 6), 210, 30, TRUE);
        const int output_usb_width = ButtonWidthForLabel(output_usb_radio_, 92, 30);
        MoveWindow(output_usb_radio_, control_x, PageY(reaper_page_state_, reaper_y), output_usb_width, 30, TRUE);
        MoveWindow(output_card_radio_, control_x + output_usb_width, PageY(reaper_page_state_, reaper_y), ButtonWidthForLabel(output_card_radio_, 96, 30), 30, TRUE);
        reaper_y += 46;
        const int reaper_help_h = StaticHeightForText(reaper_output_help_, standard_text_width, 58, 8);
        MoveWindow(reaper_output_help_, control_x, PageY(reaper_page_state_, reaper_y), standard_text_width, reaper_help_h, TRUE);
        reaper_y += reaper_help_h + 24;
        const int pending_h = StaticHeightForText(pending_summary_, standard_text_width, 92, 10);
        MoveWindow(pending_summary_, control_x, PageY(reaper_page_state_, reaper_y), standard_text_width, pending_h, TRUE);
        reaper_y += pending_h + 20;
        const int readiness_h = StaticHeightForText(readiness_detail_, standard_text_width, 128, 10);
        MoveWindow(readiness_detail_, control_x, PageY(reaper_page_state_, reaper_y), standard_text_width, readiness_h, TRUE);
        reaper_y += readiness_h + 28;
        const int choose_width = ButtonWidthForLabel(choose_sources_button_, 150);
        const int apply_width = ButtonWidthForLabel(apply_setup_button_, 120);
        const int discard_width = ButtonWidthForLabel(discard_setup_button_, 84);
        const int toggle_width = ButtonWidthForLabel(toggle_soundcheck_button_, 176);
        MoveWindow(choose_sources_button_, control_x, PageY(reaper_page_state_, reaper_y), choose_width, 32, TRUE);
        MoveWindow(apply_setup_button_, control_x + choose_width + 10, PageY(reaper_page_state_, reaper_y), apply_width, 32, TRUE);
        MoveWindow(discard_setup_button_, control_x + choose_width + apply_width + 20, PageY(reaper_page_state_, reaper_y), discard_width, 32, TRUE);
        reaper_y += 46;
        MoveWindow(toggle_soundcheck_button_, control_x, PageY(reaper_page_state_, reaper_y), toggle_width, 32, TRUE);
        reaper_y += 46;
        const int toggle_help_h = StaticHeightForText(reaper_toggle_help_, standard_text_width, 54, 8);
        MoveWindow(reaper_toggle_help_, control_x, PageY(reaper_page_state_, reaper_y), standard_text_width, toggle_help_h, TRUE);
        reaper_y += toggle_help_h + section_gap;
        reaper_page_state_.divider_y = reaper_y - 16;
        MoveWindow(auto_trigger_section_icon_, page_margin, PageY(reaper_page_state_, reaper_y + 4), 22, 22, TRUE);
        MoveWindow(auto_trigger_header_, page_margin, PageY(reaper_page_state_, reaper_y), 360, 34, TRUE);
        reaper_y += 52;
        const int auto_detail_h = StaticHeightForText(auto_trigger_detail_, page_width - control_x - 56, 50, 8);
        MoveWindow(auto_trigger_detail_, control_x, PageY(reaper_page_state_, reaper_y), page_width - control_x - 56, auto_detail_h, TRUE);
        reaper_y += auto_detail_h + 20;
        MoveWindow(auto_trigger_enable_label_, label_x, PageY(reaper_page_state_, reaper_y + 6), 210, 32, TRUE);
        const int auto_toggle_width = 110;
        MoveWindow(auto_trigger_enable_off_, control_x, PageY(reaper_page_state_, reaper_y), auto_toggle_width, 34, TRUE);
        MoveWindow(auto_trigger_enable_on_, control_x + auto_toggle_width, PageY(reaper_page_state_, reaper_y), auto_toggle_width, 34, TRUE);
        reaper_y += 54;
        MoveWindow(auto_trigger_monitor_label_, label_x, PageY(reaper_page_state_, reaper_y + 6), 210, 32, TRUE);
        MoveWindow(auto_trigger_monitor_combo_, control_x, PageY(reaper_page_state_, reaper_y), 420, 280, TRUE);
        reaper_y += 58;
        MoveWindow(auto_trigger_mode_label_, label_x, PageY(reaper_page_state_, reaper_y + 6), 210, 32, TRUE);
        const int warning_width = ButtonWidthForLabel(auto_trigger_mode_warning_, 154);
        const int record_width = ButtonWidthForLabel(auto_trigger_mode_record_, 144);
        MoveWindow(auto_trigger_mode_warning_, control_x, PageY(reaper_page_state_, reaper_y), warning_width, 34, TRUE);
        MoveWindow(auto_trigger_mode_record_, control_x + warning_width, PageY(reaper_page_state_, reaper_y), record_width, 34, TRUE);
        reaper_y += 56;
        MoveWindow(auto_trigger_threshold_label_, label_x, PageY(reaper_page_state_, reaper_y + 6), 210, 32, TRUE);
        MoveWindow(auto_trigger_threshold_edit_, control_x, PageY(reaper_page_state_, reaper_y), 140, 36, TRUE);
        MoveWindow(auto_trigger_hold_label_, control_x + 170, PageY(reaper_page_state_, reaper_y + 6), 150, 32, TRUE);
        MoveWindow(auto_trigger_hold_edit_, control_x + 334, PageY(reaper_page_state_, reaper_y), 140, 36, TRUE);
        reaper_y += 50;
        MoveWindow(auto_trigger_meter_label_, control_x, PageY(reaper_page_state_, reaper_y), 420, 30, TRUE);
        reaper_y += 50;
        const int apply_auto_width = ButtonWidthForLabel(apply_auto_trigger_button_, 310);
        const int discard_auto_width = ButtonWidthForLabel(discard_auto_trigger_button_, 170);
        MoveWindow(apply_auto_trigger_button_, control_x, PageY(reaper_page_state_, reaper_y), apply_auto_width, 46, TRUE);
        MoveWindow(discard_auto_trigger_button_, control_x + apply_auto_width + button_row_gap, PageY(reaper_page_state_, reaper_y), discard_auto_width, 46, TRUE);
        reaper_y += 66;
        const int auto_hint_h = StaticHeightForText(auto_trigger_hint_, page_width - control_x - 56, 72, 8);
        MoveWindow(auto_trigger_hint_, control_x, PageY(reaper_page_state_, reaper_y), page_width - control_x - 56, auto_hint_h, TRUE);
        reaper_page_state_.content_height = reaper_y + auto_hint_h + 54;

        int wing_y = 28;
        const int wing_intro_h = StaticHeightForText(wing_intro_, content_w - 28, 56, 8);
        wing_page_state_.intro_rect = RECT{page_margin, wing_y, page_margin + content_w, wing_y + wing_intro_h};
        MoveWindow(wing_intro_, page_margin + 14, PageY(wing_page_state_, wing_y + 8), content_w - 28, wing_intro_h - 16, TRUE);
        wing_y += wing_intro_h + 26;
        MoveWindow(wing_section_icon_, page_margin, PageY(wing_page_state_, wing_y + 4), 22, 22, TRUE);
        MoveWindow(wing_section_header_, page_margin, PageY(wing_page_state_, wing_y), 340, 34, TRUE);
        MoveWindow(tab_status_wing_, status_chip_x, PageY(wing_page_state_, wing_y), status_chip_w, 30, TRUE);
        wing_y += 52;
        MoveWindow(wing_enable_label_, label_x, PageY(wing_page_state_, wing_y + 4), 170, 30, TRUE);
        const int recorder_toggle_width = ButtonWidthForLabel(wing_enable_off_, 92, 30);
        MoveWindow(wing_enable_off_, control_x, PageY(wing_page_state_, wing_y), recorder_toggle_width, 32, TRUE);
        MoveWindow(wing_enable_on_, control_x + recorder_toggle_width, PageY(wing_page_state_, wing_y), recorder_toggle_width, 32, TRUE);
        wing_y += 54;
        MoveWindow(wing_target_label_, label_x, PageY(wing_page_state_, wing_y + 4), 170, 30, TRUE);
        const int recorder_target_width = ButtonWidthForLabel(wing_target_wlive_, 156, 30);
        MoveWindow(wing_target_wlive_, control_x, PageY(wing_page_state_, wing_y), recorder_target_width, 32, TRUE);
        MoveWindow(wing_target_usb_, control_x + recorder_target_width, PageY(wing_page_state_, wing_y), recorder_target_width, 32, TRUE);
        wing_y += 54;
        MoveWindow(wing_pair_label_, label_x, PageY(wing_page_state_, wing_y + 4), 170, 30, TRUE);
        const int pair_width = ButtonWidthForLabel(wing_pair_1_, 110, 28);
        MoveWindow(wing_pair_1_, control_x, PageY(wing_page_state_, wing_y), pair_width, 34, TRUE);
        MoveWindow(wing_pair_3_, control_x + pair_width, PageY(wing_page_state_, wing_y), pair_width, 34, TRUE);
        MoveWindow(wing_pair_5_, control_x + (pair_width * 2), PageY(wing_page_state_, wing_y), pair_width, 34, TRUE);
        MoveWindow(wing_pair_7_, control_x + (pair_width * 3), PageY(wing_page_state_, wing_y), pair_width, 34, TRUE);
        wing_y += 58;
        MoveWindow(wing_follow_label_, label_x, PageY(wing_page_state_, wing_y + 4), 170, 30, TRUE);
        MoveWindow(wing_follow_off_, control_x, PageY(wing_page_state_, wing_y), recorder_toggle_width, 32, TRUE);
        MoveWindow(wing_follow_on_, control_x + recorder_toggle_width, PageY(wing_page_state_, wing_y), recorder_toggle_width, 32, TRUE);
        wing_y += 58;
        const int wing_detail_h = StaticHeightForText(wing_placeholder_body_, standard_text_width, 80, 8);
        MoveWindow(wing_placeholder_body_, control_x, PageY(wing_page_state_, wing_y), standard_text_width, wing_detail_h, TRUE);
        wing_y += wing_detail_h + 28;
        const int apply_recorder_width = ButtonWidthForLabel(apply_recorder_button_, 248);
        const int discard_recorder_width = ButtonWidthForLabel(discard_recorder_button_, 150);
        MoveWindow(apply_recorder_button_, control_x, PageY(wing_page_state_, wing_y), apply_recorder_width, 44, TRUE);
        MoveWindow(discard_recorder_button_, control_x + apply_recorder_width + 16, PageY(wing_page_state_, wing_y), discard_recorder_width, 44, TRUE);
        wing_page_state_.content_height = wing_y + 66;

        int control_y = 28;
        const int control_intro_h = StaticHeightForText(control_intro_, content_w - 28, 56, 8);
        control_page_state_.intro_rect = RECT{page_margin, control_y, page_margin + content_w, control_y + control_intro_h};
        MoveWindow(control_intro_, page_margin + 14, PageY(control_page_state_, control_y + 8), content_w - 28, control_intro_h - 16, TRUE);
        control_y += control_intro_h + 26;
        MoveWindow(control_section_icon_, page_margin, PageY(control_page_state_, control_y + 4), 22, 22, TRUE);
        MoveWindow(control_section_header_, page_margin, PageY(control_page_state_, control_y), 380, 34, TRUE);
        MoveWindow(tab_status_control_, status_chip_x, PageY(control_page_state_, control_y), status_chip_w, 30, TRUE);
        control_y += 52;
        MoveWindow(control_enable_label_, label_x, PageY(control_page_state_, control_y + 4), 200, 30, TRUE);
        const int midi_toggle_width = ButtonWidthForLabel(midi_actions_off_, 92, 30);
        MoveWindow(midi_actions_off_, control_x, PageY(control_page_state_, control_y), midi_toggle_width, 32, TRUE);
        MoveWindow(midi_actions_on_, control_x + midi_toggle_width, PageY(control_page_state_, control_y), midi_toggle_width, 32, TRUE);
        control_y += 54;
        const int midi_summary_h = StaticHeightForText(midi_summary_, standard_text_width, 54, 8);
        MoveWindow(midi_summary_, control_x, PageY(control_page_state_, control_y), standard_text_width, midi_summary_h, TRUE);
        control_y += midi_summary_h + 16;
        const int midi_detail_h = StaticHeightForText(midi_detail_, standard_text_width, 68, 8);
        MoveWindow(midi_detail_, control_x, PageY(control_page_state_, control_y), standard_text_width, midi_detail_h, TRUE);
        control_y += midi_detail_h + 20;
        MoveWindow(warning_layer_label_, label_x, PageY(control_page_state_, control_y + 4), 200, 30, TRUE);
        MoveWindow(warning_layer_combo_, control_x, PageY(control_page_state_, control_y), 240, 260, TRUE);
        control_y += 58;
        const int control_placeholder_h = StaticHeightForText(control_placeholder_body_, standard_text_width, 84, 8);
        MoveWindow(control_placeholder_body_, control_x, PageY(control_page_state_, control_y), standard_text_width, control_placeholder_h, TRUE);
        control_y += control_placeholder_h + 24;
        const int apply_midi_width = ButtonWidthForLabel(apply_midi_button_, 226);
        const int discard_midi_width = ButtonWidthForLabel(discard_midi_button_, 150);
        MoveWindow(apply_midi_button_, control_x, PageY(control_page_state_, control_y), apply_midi_width, 44, TRUE);
        MoveWindow(discard_midi_button_, control_x + apply_midi_width + 16, PageY(control_page_state_, control_y), discard_midi_width, 44, TRUE);
        control_y += 76;
        control_page_state_.divider_y = control_y - 24;
        MoveWindow(support_section_header_, page_margin, PageY(control_page_state_, control_y), 360, 34, TRUE);
        control_y += 52;
        const int support_h = StaticHeightForText(support_detail_, standard_text_width, 54, 8);
        MoveWindow(support_detail_, control_x, PageY(control_page_state_, control_y), standard_text_width, support_h, TRUE);
        control_y += support_h + 20;
        const int open_log_width = ButtonWidthForLabel(open_debug_log_button_, 180);
        const int clear_log_width = ButtonWidthForLabel(clear_debug_log_button_, 136);
        MoveWindow(open_debug_log_button_, control_x, PageY(control_page_state_, control_y), open_log_width, 44, TRUE);
        MoveWindow(clear_debug_log_button_, control_x + open_log_width + 16, PageY(control_page_state_, control_y), clear_log_width, 44, TRUE);
        control_y += 64;
        MoveWindow(debug_log_view_, control_x, PageY(control_page_state_, control_y), standard_text_width, 180, TRUE);
        control_page_state_.content_height = control_y + 210;

        UpdatePageScroll(console_page_state_, viewport_height);
        UpdatePageScroll(reaper_page_state_, viewport_height);
        UpdatePageScroll(wing_page_state_, viewport_height);
        UpdatePageScroll(control_page_state_, viewport_height);
    }

    void ShowActivePage(int tab_index) {
        ShowWindow(page_console_, tab_index == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(page_reaper_, tab_index == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(page_wing_, tab_index == 2 ? SW_SHOW : SW_HIDE);
        ShowWindow(page_control_, tab_index == 3 ? SW_SHOW : SW_HIDE);
        current_tab_index_ = tab_index;
    }

    void SelectTab(int tab_index) {
        tab_index = std::clamp(tab_index, 0, 3);
        TabCtrl_SetCurSel(tab_, tab_index);
        ShowActivePage(tab_index);
    }

    LRESULT OnNotify(NMHDR* hdr) {
        if (hdr && hdr->hwndFrom == tab_ && hdr->code == TCN_SELCHANGE) {
            const int selected = TabCtrl_GetCurSel(tab_);
            if (selected >= 0) {
                ShowActivePage(selected);
            }
        }
        return 0;
    }

    LRESULT OnAsyncScanComplete(AsyncScanResult* result_ptr) {
        std::unique_ptr<AsyncScanResult> result(result_ptr);
        scan_in_progress_ = false;
        discovered_wings_ = result ? std::move(result->wings) : std::vector<WingInfo>{};
        RefreshDiscoveryControls(true);
        auto& extension = ReaperExtension::Instance();
        if (discovered_wings_.empty()) {
            footer_message_ = L"Scan finished. No WING consoles were discovered on the network.";
            if (result && result->show_feedback) {
                ShowMessageBox("No WING consoles were discovered. Enter a manual IP if the console is on a reachable network path.",
                               "WINGuard",
                               0);
            }
        } else {
            wchar_t buffer[160];
            std::swprintf(buffer, sizeof(buffer) / sizeof(wchar_t),
                          L"Scan finished. Found %zu WING console(s).", discovered_wings_.size());
            footer_message_ = buffer;
            if (discovered_wings_.size() == 1 && !extension.IsConnected()) {
                StartConnectAsync(L"auto-connect", L"Connected to WING.");
            }
        }
        RefreshAll();
        return 0;
    }

    LRESULT OnAsyncConnectComplete(AsyncConnectResult* result_ptr) {
        std::unique_ptr<AsyncConnectResult> result(result_ptr);
        connect_in_progress_ = false;
        if (result && result->success) {
            InvalidateValidationSnapshot();
            footer_message_ = result->success_footer.empty() ? L"Connected to WING." : result->success_footer;
            if (pending_choose_sources_after_connect_) {
                pending_choose_sources_after_connect_ = false;
                StartChooseSourcesAsync();
                return 0;
            }
            if (pending_apply_after_connect_) {
                pending_apply_after_connect_ = false;
                StartApplySetupAsync();
                return 0;
            }
            if (pending_toggle_after_connect_) {
                pending_toggle_after_connect_ = false;
                StartToggleSoundcheckAsync();
                return 0;
            }
        } else {
            pending_choose_sources_after_connect_ = false;
            pending_apply_after_connect_ = false;
            pending_toggle_after_connect_ = false;
            std::string message = "WINGuard could not connect to the configured WING.";
            if (result && !result->failure_detail.empty()) {
                message += "\n\nFailure detail:\n" + result->failure_detail;
            }
            ShowMessageBox(message.c_str(), "WINGuard", 0);
            footer_message_ = L"Connection to WING failed.";
        }
        RefreshAll();
        return 0;
    }

    LRESULT OnAsyncSourcesComplete(AsyncSourcesResult* result_ptr) {
        std::unique_ptr<AsyncSourcesResult> result(result_ptr);
        source_load_in_progress_ = false;
        if (!result || !result->success || result->channels.empty()) {
            std::string message = "Connected, but no selectable sources were discovered.";
            if (result && !result->failure_detail.empty()) {
                message += "\n\nFailure detail:\n" + result->failure_detail;
            }
            ShowMessageBox(message.c_str(), "WINGuard", 0);
            footer_message_ = L"No selectable sources were discovered.";
            RefreshAll();
            return 0;
        }

        if (!result->used_pending_draft) {
            ApplyPendingSelectionOverlay(result->channels);
        }

        SourcePickerDialog picker(hwnd_,
                                  std::move(result->channels),
                                  pending_setup_soundcheck_,
                                  pending_replace_existing_);
        SourcePickerResult picker_result = picker.Run();
        if (!picker_result.confirmed) {
            footer_message_ = L"Source review cancelled.";
            RefreshAll();
            return 0;
        }
        size_t selected_count = 0;
        for (const auto& channel : picker_result.channels) {
            if (channel.selected) {
                ++selected_count;
            }
        }
        if (selected_count == 0) {
            ShowMessageBox("No sources were selected for the next apply.", "WINGuard", 0);
            footer_message_ = L"No sources were staged.";
            RefreshAll();
            return 0;
        }
        has_pending_setup_draft_ = true;
        pending_setup_channels_ = std::move(picker_result.channels);
        pending_setup_soundcheck_ = picker_result.setup_soundcheck;
        pending_replace_existing_ = picker_result.replace_existing;
        pending_output_mode_ = CurrentOutputMode();
        InvalidateValidationSnapshot();
        footer_message_ = L"Live setup draft staged. Review the summary and click Apply Setup when ready.";
        RefreshAll();
        return 0;
    }

    LRESULT OnAsyncApplyPlanComplete(AsyncApplyPlanResult* result_ptr) {
        std::unique_ptr<AsyncApplyPlanResult> result(result_ptr);
        apply_plan_in_progress_ = false;
        if (!result || !result->success || result->prepared_channels.empty()) {
            std::string message = "WINGuard could not prepare the live setup plan.";
            if (result && !result->failure_detail.empty()) {
                message += "\n\nFailure detail:\n" + result->failure_detail;
            }
            ShowMessageBox(message.c_str(), "WINGuard", 0);
            footer_message_ = L"Live setup preparation failed.";
            RefreshAll();
            return 0;
        }

        auto& extension = ReaperExtension::Instance();
        extension.PauseAutoRecordForSetup();
        extension.GetConfig().soundcheck_output_mode = result->output_mode;
        if (extension.SetupSoundcheckFromPlan(result->prepared_channels,
                                              result->prepared_allocations,
                                              result->output_mode,
                                              result->setup_soundcheck,
                                              result->replace_existing)) {
            has_pending_setup_draft_ = false;
            pending_setup_channels_.clear();
            pending_output_mode_ = ToWide(extension.GetConfig().soundcheck_output_mode);
            SaveConfigIfPossible(extension);
            InvalidateValidationSnapshot();
            footer_message_ = L"Live recording setup applied.";
        } else {
            InvalidateValidationSnapshot();
            footer_message_ = L"Setup apply returned without success confirmation.";
        }
        RefreshAll();
        return 0;
    }

    LRESULT OnAsyncToggleComplete(AsyncToggleResult* result_ptr) {
        std::unique_ptr<AsyncToggleResult> result(result_ptr);
        toggle_in_progress_ = false;
        if (!result || !result->success) {
            std::string message = "WINGuard could not change soundcheck mode.";
            if (result && !result->failure_detail.empty()) {
                message += "\n\nFailure detail:\n" + result->failure_detail;
            }
            ShowMessageBox(message.c_str(), "WINGuard", 0);
            footer_message_ = L"Soundcheck mode change failed.";
            RefreshAll();
            return 0;
        }
        InvalidateValidationSnapshot();
        footer_message_ = result->enabled
            ? L"Soundcheck mode enabled."
            : L"Live mode restored.";
        RefreshAll();
        return 0;
    }

    LRESULT OnAsyncValidationComplete(AsyncValidationResult* result_ptr) {
        std::unique_ptr<AsyncValidationResult> result(result_ptr);
        validation_in_progress_ = false;
        if (!result || result->generation != validation_generation_) {
            return 0;
        }
        latest_validation_state_ = result->state;
        latest_validation_details_ = std::move(result->details);
        last_validation_tick_ = GetTickCount();
        validation_snapshot_ready_ = true;
        RefreshAll();
        return 0;
    }

    LRESULT OnTimer(UINT_PTR timer_id) {
        if (timer_id == kRefreshTimerId) {
            RefreshAll();
        }
        return 0;
    }

    LRESULT OnCommand(WORD id, WORD notify_code) {
        switch (id) {
            case kIdTabConsoleButton:
                SelectTab(0);
                return 0;
            case kIdTabReaperButton:
                SelectTab(1);
                return 0;
            case kIdTabWingButton:
                SelectTab(2);
                return 0;
            case kIdTabControlButton:
                SelectTab(3);
                return 0;
            case kIdScanButton:
                RunScan();
                return 0;
            case kIdWingCombo:
                if (notify_code == CBN_SELCHANGE) {
                    OnWingSelectionChanged();
                }
                return 0;
            case kIdManualIpEdit:
                if (notify_code == EN_CHANGE) {
                    OnManualIpChanged();
                }
                return 0;
            case kIdConnectButton:
                OnConnectClicked();
                return 0;
            case kIdReaperOutputUsb:
            case kIdReaperOutputCard:
                OnOutputModeChanged();
                return 0;
            case kIdChooseSourcesButton:
                OnChooseSources();
                return 0;
            case kIdApplySetupButton:
                OnApplySetup();
                return 0;
            case kIdDiscardSetupButton:
                OnDiscardSetup();
                return 0;
            case kIdToggleSoundcheckButton:
                OnToggleSoundcheck();
                return 0;
            case kIdAutoTriggerEnableOff:
            case kIdAutoTriggerEnableOn:
            case kIdAutoTriggerModeWarning:
            case kIdAutoTriggerModeRecord:
                OnAutoTriggerSettingsChanged();
                return 0;
            case kIdAutoTriggerThresholdEdit:
            case kIdAutoTriggerHoldEdit:
                if (notify_code == EN_CHANGE) {
                    OnAutoTriggerSettingsChanged();
                }
                return 0;
            case kIdAutoTriggerMonitorTrackCombo:
                if (notify_code == CBN_SELCHANGE) {
                    OnAutoTriggerSettingsChanged();
                }
                return 0;
            case kIdApplyAutoTriggerButton:
                OnApplyAutoTriggerSettings();
                return 0;
            case kIdDiscardAutoTriggerButton:
                OnDiscardAutoTriggerSettings();
                return 0;
            case kIdRecorderEnableOff:
            case kIdRecorderEnableOn:
            case kIdRecorderTargetWLive:
            case kIdRecorderTargetUsb:
            case kIdRecorderPair1:
            case kIdRecorderPair3:
            case kIdRecorderPair5:
            case kIdRecorderPair7:
            case kIdRecorderFollowOff:
            case kIdRecorderFollowOn:
                OnRecorderSettingsChanged();
                return 0;
            case kIdApplyRecorderButton:
                OnApplyRecorderSettings();
                return 0;
            case kIdDiscardRecorderButton:
                OnDiscardRecorderSettings();
                return 0;
            case kIdMidiActionsOff:
            case kIdMidiActionsOn:
                OnMidiActionsChanged();
                return 0;
            case kIdWarningLayerCombo:
                if (notify_code == CBN_SELCHANGE) {
                    OnMidiActionsChanged();
                }
                return 0;
            case kIdApplyMidiButton:
                OnApplyMidiActions();
                return 0;
            case kIdDiscardMidiButton:
                OnDiscardMidiActions();
                return 0;
            case kIdOpenDebugLogButton:
                OnOpenDebugLog();
                return 0;
            case kIdClearDebugLogButton:
                OnClearDebugLog();
                return 0;
            default:
                return 0;
        }
    }

    LRESULT OnCtlColor(HDC hdc, HWND control) {
        SetBkMode(hdc, TRANSPARENT);
        const int id = GetDlgCtrlID(control);
        COLORREF text_color = GetSysColor(COLOR_WINDOWTEXT);
        if (control == header_console_icon_) {
            text_color = current_snapshot_.console.color;
        } else if (control == header_validation_icon_) {
            text_color = current_snapshot_.validation.color;
        } else if (control == header_recorder_icon_) {
            text_color = current_snapshot_.recorder.color;
        } else if (control == header_midi_icon_) {
            text_color = current_snapshot_.midi.color;
        } else if (control == console_section_icon_) {
            text_color = RGB(28, 114, 184);
        } else if (control == reaper_section_icon_) {
            text_color = RGB(40, 140, 70);
        } else if (control == auto_trigger_section_icon_) {
            text_color = RGB(215, 135, 30);
        } else if (control == wing_section_icon_) {
            text_color = RGB(153, 84, 187);
        } else if (control == control_section_icon_) {
            text_color = RGB(80, 80, 80);
        } else if (id == kIdConsoleStatusChip) {
            text_color = current_snapshot_.console_tab.color;
        } else if (id == kIdReaperStatusChip) {
            text_color = current_snapshot_.reaper_tab.color;
        } else if (id == kIdWingStatusChip) {
            text_color = current_snapshot_.wing_tab.color;
        } else if (id == kIdControlStatusChip) {
            text_color = current_snapshot_.control_tab.color;
        } else if (id == kIdPendingSummary) {
            text_color = current_snapshot_.pending_color;
        } else if (id == kIdReadinessDetail) {
            text_color = current_snapshot_.readiness_color;
        } else if (id == kIdFooterStatus) {
            text_color = RGB(80, 80, 80);
        } else if (control == subtitle_ ||
                   control == console_help_discovery_ ||
                   control == console_help_manual_ ||
                   control == console_footer_ ||
                   control == reaper_output_help_ ||
                   control == reaper_toggle_help_ ||
                   control == wing_placeholder_body_ ||
                   control == control_placeholder_body_ ||
                   control == support_detail_ ||
                   control == midi_summary_ ||
                   control == midi_detail_ ||
                   control == console_intro_ ||
                   control == reaper_intro_ ||
                   control == wing_intro_ ||
                   control == control_intro_) {
            text_color = RGB(92, 98, 104);
        } else if (control == auto_trigger_detail_) {
            text_color = RGB(92, 98, 104);
        } else if (control == auto_trigger_hint_) {
            text_color = RGB(28, 114, 184);
        } else if (control == auto_trigger_meter_label_) {
            text_color = RGB(80, 80, 80);
        }
        SetTextColor(hdc, text_color);
        if (control == title_ ||
            control == subtitle_) {
            return reinterpret_cast<LRESULT>(banner_brush_);
        }
        if (control == header_console_icon_ ||
            control == header_console_status_ ||
            control == header_validation_icon_ ||
            control == header_validation_status_ ||
            control == header_recorder_icon_ ||
            control == header_recorder_status_ ||
            control == header_midi_icon_ ||
            control == header_midi_status_) {
            return reinterpret_cast<LRESULT>(status_panel_brush_);
        }
        if (control == console_intro_ ||
            control == reaper_intro_ ||
            control == wing_intro_ ||
            control == control_intro_) {
            return reinterpret_cast<LRESULT>(card_brush_);
        }
        return reinterpret_cast<LRESULT>(body_brush_);
    }

    void UpdateWingTabUI() {
        auto& extension = ReaperExtension::Instance();
        std::wstring detail;
        if (recorder_settings_dirty_) {
            detail = L"Recorder coordination changes are staged. Apply them to update the target recorder, the MAIN source pair, and follow behavior.";
        } else if (!extension.GetConfig().recorder_coordination_enabled) {
            detail = L"Recorder coordination is off. Turn Recorder Control on to prepare a Wing recorder and optionally follow auto-trigger recordings.";
        } else if (!extension.IsConnected()) {
            detail = L"Recorder coordination can be staged now, but a connected Wing is required before recorder routing can actually be pushed.";
        } else {
            detail = L"Recorder coordination is aligned with the current setup and ready to be used.";
        }
        SetWindowTextIfChanged(wing_placeholder_body_, detail);
        EnableWindow(apply_recorder_button_, recorder_settings_dirty_ ? TRUE : FALSE);
        EnableWindow(discard_recorder_button_, recorder_settings_dirty_ ? TRUE : FALSE);
    }

    void RefreshMonitorTrackDropdown() {
        if (!auto_trigger_monitor_combo_) {
            return;
        }
        const int track_count = ReaperExtension::Instance().GetProjectTrackCount();
        if (track_count != last_monitor_track_count_) {
            SendMessageW(auto_trigger_monitor_combo_, CB_RESETCONTENT, 0, 0);
            SendMessageW(auto_trigger_monitor_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto (Armed+Monitored)"));
            for (int i = 1; i <= track_count; ++i) {
                wchar_t label[64];
                std::swprintf(label, sizeof(label) / sizeof(wchar_t), L"Track %d", i);
                SendMessageW(auto_trigger_monitor_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            }
            last_monitor_track_count_ = track_count;
        }
        auto& config = ReaperExtension::Instance().GetConfig();
        int wanted = std::max(0, pending_auto_record_monitor_track_);
        if (wanted > track_count) {
            wanted = 0;
            pending_auto_record_monitor_track_ = 0;
            if (config.auto_record_monitor_track > track_count) {
                config.auto_record_monitor_track = 0;
            }
        }
        const LRESULT current_selection = SendMessageW(auto_trigger_monitor_combo_, CB_GETCURSEL, 0, 0);
        if (current_selection != wanted) {
            SendMessageW(auto_trigger_monitor_combo_, CB_SETCURSEL, wanted, 0);
        }
    }

    void SyncAutoTriggerFromConfig() {
        const auto& config = ReaperExtension::Instance().GetConfig();
        pending_auto_record_enabled_ = config.auto_record_enabled;
        pending_auto_record_warning_only_ = config.auto_record_warning_only;
        pending_auto_record_threshold_db_ = config.auto_record_threshold_db;
        pending_auto_record_hold_ms_ = config.auto_record_hold_ms;
        pending_auto_record_monitor_track_ = std::max(0, config.auto_record_monitor_track);
        auto_trigger_dirty_ = false;
    }

    void SyncAutoTriggerControlsFromPending() {
        CheckRadioButton(page_reaper_, kIdAutoTriggerEnableOff, kIdAutoTriggerEnableOn,
                         pending_auto_record_enabled_ ? kIdAutoTriggerEnableOn : kIdAutoTriggerEnableOff);
        CheckRadioButton(page_reaper_, kIdAutoTriggerModeWarning, kIdAutoTriggerModeRecord,
                         pending_auto_record_warning_only_ ? kIdAutoTriggerModeWarning : kIdAutoTriggerModeRecord);
        wchar_t threshold_text[32];
        std::swprintf(threshold_text, sizeof(threshold_text) / sizeof(wchar_t), L"%.1f", pending_auto_record_threshold_db_);
        SetWindowTextW(auto_trigger_threshold_edit_, threshold_text);
        wchar_t hold_text[32];
        std::swprintf(hold_text, sizeof(hold_text) / sizeof(wchar_t), L"%.1f", pending_auto_record_hold_ms_ / 1000.0);
        SetWindowTextW(auto_trigger_hold_edit_, hold_text);
        RefreshMonitorTrackDropdown();
    }

    void OnAutoTriggerSettingsChanged() {
        pending_auto_record_enabled_ = (IsDlgButtonChecked(page_reaper_, kIdAutoTriggerEnableOn) == BST_CHECKED);
        pending_auto_record_warning_only_ = (IsDlgButtonChecked(page_reaper_, kIdAutoTriggerModeWarning) == BST_CHECKED);
        pending_auto_record_threshold_db_ = std::wcstod(ReadWindowText(auto_trigger_threshold_edit_).c_str(), nullptr);
        if (!std::isfinite(pending_auto_record_threshold_db_)) {
            pending_auto_record_threshold_db_ = ReaperExtension::Instance().GetConfig().auto_record_threshold_db;
        }
        const double hold_seconds = std::wcstod(ReadWindowText(auto_trigger_hold_edit_).c_str(), nullptr);
        pending_auto_record_hold_ms_ = std::max(0, static_cast<int>(std::lround((std::isfinite(hold_seconds) ? hold_seconds : 0.0) * 1000.0)));
        const LRESULT selection = SendMessageW(auto_trigger_monitor_combo_, CB_GETCURSEL, 0, 0);
        pending_auto_record_monitor_track_ = (selection >= 0) ? static_cast<int>(selection) : 0;
        const auto& config = ReaperExtension::Instance().GetConfig();
        auto_trigger_dirty_ =
            pending_auto_record_enabled_ != config.auto_record_enabled ||
            pending_auto_record_warning_only_ != config.auto_record_warning_only ||
            std::fabs(pending_auto_record_threshold_db_ - config.auto_record_threshold_db) > 0.05 ||
            pending_auto_record_hold_ms_ != config.auto_record_hold_ms ||
            pending_auto_record_monitor_track_ != config.auto_record_monitor_track;
        footer_message_ = auto_trigger_dirty_
            ? L"Auto Trigger changes staged."
            : L"Auto Trigger matches the applied settings.";
        RefreshAll();
    }

    void OnApplyAutoTriggerSettings() {
        auto& extension = ReaperExtension::Instance();
        auto& config = extension.GetConfig();
        config.auto_record_enabled = pending_auto_record_enabled_;
        config.auto_record_warning_only = pending_auto_record_warning_only_;
        config.auto_record_threshold_db = pending_auto_record_threshold_db_;
        config.auto_record_hold_ms = pending_auto_record_hold_ms_;
        config.auto_record_monitor_track = pending_auto_record_monitor_track_;
        SaveConfigIfPossible(extension);
        extension.ApplyAutoRecordSettings();
        auto_trigger_dirty_ = false;
        footer_message_ = L"Auto Trigger settings applied.";
        RefreshAll();
    }

    void OnDiscardAutoTriggerSettings() {
        SyncAutoTriggerFromConfig();
        SyncAutoTriggerControlsFromPending();
        footer_message_ = L"Auto Trigger changes discarded.";
        RefreshAll();
    }

    void UpdateAutoTriggerUI() {
        auto& extension = ReaperExtension::Instance();
        const auto& config = extension.GetConfig();
        const bool pending_apply = has_pending_setup_draft_ || pending_output_mode_ != ToWide(config.soundcheck_output_mode);
        const bool live_setup_controls_enabled = (latest_validation_state_ == ValidationState::Ready) && !pending_apply;
        const bool auto_trigger_enabled = live_setup_controls_enabled && pending_auto_record_enabled_;

        std::wstring detail;
        if (auto_trigger_dirty_) {
            detail = L"Auto Trigger settings changed. Apply them to resume trigger monitoring with the staged mode, threshold, hold time, and monitor track.";
        } else if (pending_apply) {
            detail = L"Auto Trigger is blocked by pending setup changes. Apply the staged setup or rebuild the current managed setup first.";
        } else if (latest_validation_state_ != ValidationState::Ready) {
            detail = L"Auto Trigger is blocked until the live recording setup validates against the current WING and REAPER state.";
        } else if (extension.IsSoundcheckModeEnabled()) {
            detail = L"Auto Trigger is paused while Soundcheck Mode is active on the managed channels.";
        } else if (!config.auto_record_enabled) {
            detail = L"Auto Trigger is currently off. Turn it on and apply the change to start signal-based monitoring again.";
        } else {
            detail = L"Auto Trigger is clear to run with the current live recording setup.";
        }
        std::wstring hint = auto_trigger_dirty_
            ? L"Pending changes stay parked until you click Apply Auto Trigger Settings."
            : L"Warning mode flashes controls when triggered; Record mode starts and stops recording automatically.";
        SetWindowTextIfChanged(auto_trigger_detail_, detail);
        SetWindowTextIfChanged(auto_trigger_hint_, hint);

        EnableWindow(auto_trigger_enable_off_, live_setup_controls_enabled ? TRUE : FALSE);
        EnableWindow(auto_trigger_enable_on_, live_setup_controls_enabled ? TRUE : FALSE);
        EnableWindow(auto_trigger_monitor_combo_, auto_trigger_enabled ? TRUE : FALSE);
        EnableWindow(auto_trigger_mode_warning_, auto_trigger_enabled ? TRUE : FALSE);
        EnableWindow(auto_trigger_mode_record_, auto_trigger_enabled ? TRUE : FALSE);
        EnableWindow(auto_trigger_threshold_edit_, auto_trigger_enabled ? TRUE : FALSE);
        EnableWindow(auto_trigger_hold_edit_, auto_trigger_enabled ? TRUE : FALSE);
        EnableWindow(apply_auto_trigger_button_, (live_setup_controls_enabled && auto_trigger_dirty_) ? TRUE : FALSE);
        EnableWindow(discard_auto_trigger_button_, auto_trigger_dirty_ ? TRUE : FALSE);
    }

    void UpdateControlTabUI() {
        auto& extension = ReaperExtension::Instance();
        std::wstring summary = midi_actions_dirty_
            ? (pending_midi_actions_enabled_ ? L"Pending control integration enable." : L"Pending control integration disable.")
            : L"No pending MIDI shortcut changes.";
        std::wstring detail;
        if (midi_actions_dirty_) {
            detail = L"These changes stay staged until you apply them. When enabled, Wing user controls can trigger REAPER transport and warning feedback.";
        } else if (extension.IsMidiActionsEnabled()) {
            detail = L"Wing control integration is active. Warning flash layer and transport mappings are using the applied settings.";
        } else {
            detail = L"MIDI shortcuts are disabled. Enable them after live setup is validated if you want hands-on transport control from the console.";
        }
        SetWindowTextIfChanged(midi_summary_, summary);
        SetWindowTextIfChanged(midi_detail_, detail);
        EnableWindow(apply_midi_button_, midi_actions_dirty_ ? TRUE : FALSE);
        EnableWindow(discard_midi_button_, midi_actions_dirty_ ? TRUE : FALSE);
    }

    void FlushPendingLogBuffer() {
        if (!debug_log_view_) {
            return;
        }
        std::wstring chunk;
        {
            std::lock_guard<std::mutex> lock(log_buffer_mutex_);
            if (pending_log_buffer_.empty()) {
                return;
            }
            chunk.swap(pending_log_buffer_);
        }

        constexpr size_t kMaxLogChars = 32000;
        const int current_length = GetWindowTextLengthW(debug_log_view_);
        if (current_length > 0 &&
            (static_cast<size_t>(current_length) + chunk.size()) <= kMaxLogChars) {
            SendMessageW(debug_log_view_, EM_SETSEL, current_length, current_length);
            SendMessageW(debug_log_view_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(chunk.c_str()));
            SendMessageW(debug_log_view_, EM_SCROLLCARET, 0, 0);
            debug_log_popup_.SetText(CurrentLogText());
            return;
        }

        std::wstring combined = ReadWindowText(debug_log_view_) + chunk;
        if (combined.size() > kMaxLogChars) {
            combined = combined.substr(combined.size() - kMaxLogChars);
        }
        SetWindowTextW(debug_log_view_, combined.c_str());
        const int length = GetWindowTextLengthW(debug_log_view_);
        SendMessageW(debug_log_view_, EM_SETSEL, length, length);
        SendMessageW(debug_log_view_, EM_SCROLLCARET, 0, 0);
        debug_log_popup_.SetText(combined);
    }

    std::wstring CurrentLogText() const {
        return debug_log_view_ ? ReadWindowText(debug_log_view_) : std::wstring();
    }

    void UpdateAutoTriggerMeterPreview() {
        if (!auto_trigger_meter_label_) {
            return;
        }
        const double lin = ReaperExtension::Instance().ReadCurrentTriggerLevel();
        std::wstring text;
        if (lin <= 0.0000001 || !std::isfinite(lin)) {
            text = L"Trigger level: -inf dBFS";
        } else {
            const double db = 20.0 * std::log10(lin);
            wchar_t buffer[64];
            std::swprintf(buffer, sizeof(buffer) / sizeof(wchar_t), L"Trigger level: %.1f dBFS", db);
            text = buffer;
        }
        if (ReadWindowText(auto_trigger_meter_label_) != text) {
            SetWindowTextW(auto_trigger_meter_label_, text.c_str());
        }
    }

    void RefreshAll() {
        if (!hwnd_) {
            return;
        }
        const StatusSnapshot previous_snapshot = current_snapshot_;
        auto snapshot = BuildSnapshot();
        current_snapshot_ = snapshot;
        auto redraw_control = [](HWND control) {
            if (control) {
                RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE);
            }
        };

        SetWindowTextIfChanged(header_console_status_, snapshot.console.text);
        SetWindowTextIfChanged(header_validation_status_, snapshot.validation.text);
        SetWindowTextIfChanged(header_recorder_status_, snapshot.recorder.text);
        SetWindowTextIfChanged(header_midi_status_, snapshot.midi.text);
        SetWindowTextIfChanged(tab_status_console_, snapshot.console_tab.text);
        SetWindowTextIfChanged(tab_status_reaper_, snapshot.reaper_tab.text);
        SetWindowTextIfChanged(tab_status_wing_, snapshot.wing_tab.text);
        SetWindowTextIfChanged(tab_status_control_, snapshot.control_tab.text);
        SetWindowTextIfChanged(pending_summary_, snapshot.pending_summary);
        SetWindowTextIfChanged(readiness_detail_, snapshot.readiness_detail);
        SetWindowTextIfChanged(footer_status_, snapshot.footer);
        SetWindowTextIfChanged(apply_setup_button_, snapshot.apply_label);
        SetWindowTextIfChanged(toggle_soundcheck_button_, snapshot.toggle_label);
        UpdateAutoTriggerMeterPreview();
        FlushPendingLogBuffer();
        RefreshMonitorTrackDropdown();
        UpdateAutoTriggerUI();
        UpdateWingTabUI();
        UpdateControlTabUI();
        const bool async_busy = scan_in_progress_ || connect_in_progress_ || source_load_in_progress_ ||
                                apply_plan_in_progress_ || toggle_in_progress_;
        EnableWindow(scan_button_, async_busy ? FALSE : TRUE);
        EnableWindow(wing_combo_, async_busy ? FALSE : TRUE);
        EnableWindow(manual_ip_edit_, async_busy ? FALSE : TRUE);
        EnableWindow(connect_button_, async_busy ? FALSE : TRUE);
        EnableWindow(choose_sources_button_, async_busy ? FALSE : TRUE);
        EnableWindow(apply_setup_button_, (!async_busy && snapshot.can_apply) ? TRUE : FALSE);
        EnableWindow(discard_setup_button_, (!async_busy && snapshot.can_discard) ? TRUE : FALSE);
        EnableWindow(toggle_soundcheck_button_, (!async_busy && snapshot.can_toggle) ? TRUE : FALSE);
        if (scan_in_progress_) {
            SetWindowTextIfChanged(scan_button_, L"Scanning...");
        } else {
            SetWindowTextIfChanged(scan_button_, L"Scan");
        }
        if (connect_in_progress_) {
            SetWindowTextIfChanged(connect_button_, L"Connecting...");
        } else {
            SetWindowTextIfChanged(connect_button_, ReaperExtension::Instance().IsConnected() ? L"Disconnect" : L"Connect");
        }
        if (apply_plan_in_progress_) {
            SetWindowTextIfChanged(apply_setup_button_, L"Preparing...");
        } else {
            SetWindowTextIfChanged(apply_setup_button_, snapshot.apply_label);
        }
        if (toggle_in_progress_) {
            SetWindowTextIfChanged(toggle_soundcheck_button_, L"Switching...");
        } else {
            SetWindowTextIfChanged(toggle_soundcheck_button_, snapshot.toggle_label);
        }

        if (previous_snapshot.console.color != snapshot.console.color || previous_snapshot.console.text != snapshot.console.text) {
            redraw_control(header_console_icon_);
            redraw_control(header_console_status_);
        }
        if (previous_snapshot.validation.color != snapshot.validation.color || previous_snapshot.validation.text != snapshot.validation.text) {
            redraw_control(header_validation_icon_);
            redraw_control(header_validation_status_);
        }
        if (previous_snapshot.recorder.color != snapshot.recorder.color || previous_snapshot.recorder.text != snapshot.recorder.text) {
            redraw_control(header_recorder_icon_);
            redraw_control(header_recorder_status_);
        }
        if (previous_snapshot.midi.color != snapshot.midi.color || previous_snapshot.midi.text != snapshot.midi.text) {
            redraw_control(header_midi_icon_);
            redraw_control(header_midi_status_);
        }
        if (previous_snapshot.console_tab.color != snapshot.console_tab.color || previous_snapshot.console_tab.text != snapshot.console_tab.text) {
            redraw_control(tab_status_console_);
        }
        if (previous_snapshot.reaper_tab.color != snapshot.reaper_tab.color || previous_snapshot.reaper_tab.text != snapshot.reaper_tab.text) {
            redraw_control(tab_status_reaper_);
        }
        if (previous_snapshot.wing_tab.color != snapshot.wing_tab.color || previous_snapshot.wing_tab.text != snapshot.wing_tab.text) {
            redraw_control(tab_status_wing_);
        }
        if (previous_snapshot.control_tab.color != snapshot.control_tab.color || previous_snapshot.control_tab.text != snapshot.control_tab.text) {
            redraw_control(tab_status_control_);
        }
        if (previous_snapshot.pending_color != snapshot.pending_color || previous_snapshot.pending_summary != snapshot.pending_summary) {
            redraw_control(pending_summary_);
        }
        if (previous_snapshot.readiness_color != snapshot.readiness_color || previous_snapshot.readiness_detail != snapshot.readiness_detail) {
            redraw_control(readiness_detail_);
        }
        if (previous_snapshot.footer != snapshot.footer) {
            redraw_control(footer_status_);
        }
    }

    HeaderStatusVisual MakeStatus(std::wstring text, COLORREF color) const {
        HeaderStatusVisual visual;
        visual.text = std::move(text);
        visual.color = color;
        return visual;
    }

    std::wstring CurrentOutputMode() const {
        return (IsDlgButtonChecked(page_reaper_, kIdReaperOutputCard) == BST_CHECKED) ? L"CARD" : L"USB";
    }

    void SelectOutputMode(const std::string& mode) {
        const bool card = mode == "CARD";
        CheckRadioButton(page_reaper_, kIdReaperOutputUsb, kIdReaperOutputCard,
                         card ? kIdReaperOutputCard : kIdReaperOutputUsb);
    }

    std::wstring SelectedOrManualIp() const {
        const int selection = static_cast<int>(SendMessageW(wing_combo_, CB_GETCURSEL, 0, 0));
        if (selection >= 0 && selection < static_cast<int>(discovered_wings_.size())) {
            return ToWide(discovered_wings_[static_cast<size_t>(selection)].console_ip);
        }
        const std::wstring typed = ReadWindowText(manual_ip_edit_);
        return typed;
    }

    bool EnsureConnected(const wchar_t* context) {
        auto& extension = ReaperExtension::Instance();
        if (extension.IsConnected()) {
            return true;
        }
        std::wstring ip = SelectedOrManualIp();
        if (ip.empty()) {
            wchar_t message[256];
            std::swprintf(message, sizeof(message) / sizeof(wchar_t),
                          L"No WING IP is selected. Scan or enter a manual IP before %ls.", context);
            ShowMessageBox(ToUtf8(message).c_str(), "WINGuard", 0);
            return false;
        }
        extension.GetConfig().wing_ip = ToUtf8(ip);
        SaveConfigIfPossible(extension);
        if (!extension.ConnectToWing()) {
            std::string detail = extension.GetLastConnectionFailureDetail();
            std::string message = "WINGuard could not connect to the configured WING.";
            if (!detail.empty()) {
                message += "\n\nFailure detail:\n" + detail;
            }
            ShowMessageBox(message.c_str(), "WINGuard", 0);
            return false;
        }
        footer_message_ = L"Connected to WING.";
        return true;
    }

    void StartConnectAsync(const wchar_t* context, std::wstring success_footer) {
        if (connect_in_progress_) {
            return;
        }
        std::wstring ip = SelectedOrManualIp();
        if (ip.empty()) {
            wchar_t message[256];
            std::swprintf(message, sizeof(message) / sizeof(wchar_t),
                          L"No WING IP is selected. Scan or enter a manual IP before %ls.", context);
            ShowMessageBox(ToUtf8(message).c_str(), "WINGuard", 0);
            return;
        }
        auto& extension = ReaperExtension::Instance();
        extension.GetConfig().wing_ip = ToUtf8(ip);
        SaveConfigIfPossible(extension);
        connect_in_progress_ = true;
        footer_message_ = std::wstring(L"Connecting to WING for ") + context + L"...";
        RefreshAll();
        HWND target = hwnd_;
        const std::string ip_utf8 = ToUtf8(ip);
        std::thread([target, ip_utf8, success_footer = std::move(success_footer)]() {
            auto result = std::make_unique<AsyncConnectResult>();
            auto& extension = ReaperExtension::Instance();
            extension.GetConfig().wing_ip = ip_utf8;
            result->success = extension.ConnectToWing();
            result->ip = ip_utf8;
            result->success_footer = success_footer;
            if (!result->success) {
                result->failure_detail = extension.GetLastConnectionFailureDetail();
            }
            if (!PostMessageW(target, kMsgAsyncConnectComplete, 0, reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    }

    void StartChooseSourcesAsync() {
        source_load_in_progress_ = true;
        footer_message_ = L"Loading selectable sources...";
        RefreshAll();
        HWND target = hwnd_;
        const bool use_pending_draft = has_pending_setup_draft_ && !pending_setup_channels_.empty();
        const auto pending_channels = pending_setup_channels_;
        std::thread([target, use_pending_draft, pending_channels]() {
            auto result = std::make_unique<AsyncSourcesResult>();
            result->used_pending_draft = use_pending_draft;
            auto& extension = ReaperExtension::Instance();
            if (!extension.IsConnected()) {
                result->failure_detail = "Not connected to a WING.";
            } else if (use_pending_draft) {
                result->channels = pending_channels;
                result->success = !result->channels.empty();
            } else {
                result->channels = extension.GetAvailableSources();
                result->success = !result->channels.empty();
            }
            if (!PostMessageW(target, kMsgAsyncSourcesComplete, 0, reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    }

    void StartApplySetupAsync() {
        if (apply_plan_in_progress_) {
            return;
        }
        apply_plan_in_progress_ = true;
        footer_message_ = L"Preparing live setup...";
        RefreshAll();
        HWND target = hwnd_;
        const bool has_pending_draft = has_pending_setup_draft_;
        const auto pending_channels = pending_setup_channels_;
        const bool setup_soundcheck = pending_setup_soundcheck_;
        const bool replace_existing = pending_replace_existing_;
        const std::string output_mode = ToUtf8(pending_output_mode_);
        std::thread([target, has_pending_draft, pending_channels, setup_soundcheck, replace_existing, output_mode]() {
            auto result = std::make_unique<AsyncApplyPlanResult>();
            auto& extension = ReaperExtension::Instance();
            std::vector<SourceSelectionInfo> channels_to_apply;
            if (has_pending_draft) {
                channels_to_apply = pending_channels;
            } else {
                channels_to_apply = extension.GetAvailableSources();
            }
            size_t selected_count = 0;
            for (const auto& channel : channels_to_apply) {
                if (channel.selected) {
                    ++selected_count;
                }
            }
            if (selected_count == 0) {
                result->failure_detail = "No sources are staged for apply.";
            } else if (!extension.PrepareSoundcheckPlan(channels_to_apply,
                                                        result->prepared_channels,
                                                        result->prepared_allocations,
                                                        result->failure_detail)) {
                // failure_detail already filled by PrepareSoundcheckPlan.
            } else {
                result->success = true;
                result->setup_soundcheck = setup_soundcheck;
                result->replace_existing = replace_existing;
                result->output_mode = output_mode;
            }
            if (!PostMessageW(target, kMsgAsyncApplyPlanComplete, 0, reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    }

    void StartToggleSoundcheckAsync() {
        if (toggle_in_progress_) {
            return;
        }
        toggle_in_progress_ = true;
        footer_message_ = L"Toggling soundcheck mode...";
        RefreshAll();
        HWND target = hwnd_;
        const bool enable = !ReaperExtension::Instance().IsSoundcheckModeEnabled();
        std::thread([target, enable]() {
            auto result = std::make_unique<AsyncToggleResult>();
            std::string failure_detail;
            result->enabled = enable;
            result->success = ReaperExtension::Instance().SetSoundcheckModeEnabled(enable, &failure_detail);
            result->failure_detail = failure_detail;
            if (!PostMessageW(target, kMsgAsyncToggleComplete, 0, reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    }

    void StartValidationAsync() {
        if (validation_in_progress_ || !hwnd_) {
            return;
        }
        validation_in_progress_ = true;
        const unsigned long long generation = validation_generation_;
        HWND target = hwnd_;
        std::thread([target, generation]() {
            auto result = std::make_unique<AsyncValidationResult>();
            result->generation = generation;
            result->state = ReaperExtension::Instance().ValidateLiveRecordingSetup(result->details);
            if (!PostMessageW(target, kMsgAsyncValidationComplete, 0, reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    }

    StatusSnapshot BuildSnapshot() {
        // Project all applied, staged, asynchronous, and validation state into
        // one immutable UI snapshot. Header rows, tab badges, button enablement,
        // and helper copy must agree for a given refresh tick.
        StatusSnapshot snapshot;
        auto& extension = ReaperExtension::Instance();
        auto& config = extension.GetConfig();
        const bool connected = extension.IsConnected();
        const std::wstring applied_output = ToWide(config.soundcheck_output_mode);
        const std::wstring staged_output = pending_output_mode_.empty() ? applied_output : pending_output_mode_;
        const bool should_validate_now = connected && !has_pending_setup_draft_ && staged_output == applied_output;

        if (should_validate_now && ShouldRefreshValidation()) {
            StartValidationAsync();
        } else if (!should_validate_now) {
            if (validation_snapshot_ready_ || validation_in_progress_ || !latest_validation_details_.empty()) {
                InvalidateValidationSnapshot();
            }
        }

        snapshot.console = connected
            ? MakeStatus(L"Console: Connected", RGB(40, 140, 70))
            : MakeStatus(L"Console: Not Connected", RGB(110, 110, 110));

        if (has_pending_setup_draft_ || staged_output != applied_output || auto_trigger_dirty_) {
            snapshot.validation = MakeStatus(L"Reaper Recorder: Pending", RGB(215, 135, 30));
        } else if (connected && validation_in_progress_ && !validation_snapshot_ready_) {
            snapshot.validation = MakeStatus(L"Reaper Recorder: Checking...", RGB(215, 135, 30));
        } else if (latest_validation_state_ == ValidationState::Ready) {
            if (config.auto_record_enabled) {
                snapshot.validation = MakeStatus(
                    config.auto_record_warning_only ? L"Reaper Recorder: Ready + Warning"
                                                    : L"Reaper Recorder: Ready + Record",
                    config.auto_record_warning_only ? RGB(215, 135, 30) : RGB(40, 140, 70));
            } else {
                snapshot.validation = MakeStatus(L"Reaper Recorder: Ready", RGB(215, 135, 30));
            }
        } else if (connected) {
            snapshot.validation = MakeStatus(L"Reaper Recorder: Review", RGB(215, 135, 30));
        } else {
            snapshot.validation = MakeStatus(L"Reaper Recorder: Not Ready", RGB(110, 110, 110));
        }

        if (recorder_settings_dirty_) {
            snapshot.recorder = MakeStatus(L"Wing Recorder: Pending", RGB(215, 135, 30));
        } else if (config.recorder_coordination_enabled && config.sd_auto_record_with_reaper && config.auto_record_enabled) {
            snapshot.recorder = MakeStatus(L"Wing Recorder: Auto", RGB(40, 140, 70));
        } else if (config.recorder_coordination_enabled) {
            snapshot.recorder = MakeStatus(L"Wing Recorder: Enabled", RGB(215, 135, 30));
        } else {
            snapshot.recorder = MakeStatus(L"Wing Recorder: Disabled", RGB(110, 110, 110));
        }

        if (midi_actions_dirty_) {
            snapshot.midi = MakeStatus(L"Wing control integration: Pending", RGB(215, 135, 30));
        } else if (extension.IsMidiActionsEnabled()) {
            snapshot.midi = MakeStatus(L"Wing control integration: Enabled", RGB(40, 140, 70));
        } else {
            snapshot.midi = MakeStatus(L"Wing control integration: Disabled", RGB(110, 110, 110));
        }

        snapshot.console_tab = connected
            ? MakeStatus(L"Connected", RGB(40, 140, 70))
            : MakeStatus(L"Inactive", RGB(110, 110, 110));

        if (has_pending_setup_draft_ || staged_output != applied_output || auto_trigger_dirty_) {
            snapshot.reaper_tab = MakeStatus(L"Pending", RGB(215, 135, 30));
        } else if (latest_validation_state_ == ValidationState::Ready) {
            snapshot.reaper_tab = MakeStatus(L"Ready", RGB(40, 140, 70));
        } else if (connected) {
            snapshot.reaper_tab = MakeStatus(L"Attention", RGB(215, 135, 30));
        } else {
            snapshot.reaper_tab = MakeStatus(L"Inactive", RGB(110, 110, 110));
        }

        if (recorder_settings_dirty_) {
            snapshot.wing_tab = MakeStatus(L"Pending", RGB(215, 135, 30));
        } else if (config.recorder_coordination_enabled && config.sd_auto_record_with_reaper && config.auto_record_enabled) {
            snapshot.wing_tab = MakeStatus(L"Ready", RGB(40, 140, 70));
        } else if (config.recorder_coordination_enabled) {
            snapshot.wing_tab = MakeStatus(L"Enabled", RGB(215, 135, 30));
        } else {
            snapshot.wing_tab = MakeStatus(L"Inactive", RGB(110, 110, 110));
        }

        snapshot.control_tab = midi_actions_dirty_
            ? MakeStatus(L"Pending", RGB(215, 135, 30))
            : (extension.IsMidiActionsEnabled()
                ? MakeStatus(L"Ready", RGB(40, 140, 70))
                : MakeStatus(L"Inactive", RGB(110, 110, 110)));

        if (has_pending_setup_draft_ || staged_output != applied_output) {
            const size_t selected_count = SelectedPendingCount();
            if (!has_pending_setup_draft_ && staged_output != applied_output) {
                snapshot.pending_summary =
                    std::wstring(L"Recording I/O mode change staged. Click Rebuild Current Setup to reuse the current managed selection in ") +
                    staged_output + L" mode.";
            } else if (selected_count == 0) {
                snapshot.pending_summary =
                    std::wstring(L"Current managed setup staged for rebuild in ") + staged_output +
                    L" mode. Click Rebuild Current Setup to reuse the saved selection and rewrite routing.";
            } else {
                wchar_t buffer[256];
                std::swprintf(buffer, sizeof(buffer) / sizeof(wchar_t),
                              L"Changes staged for %zu sources in %ls mode. Review if needed, then click Apply Setup.",
                              selected_count,
                              staged_output.c_str());
                snapshot.pending_summary = buffer;
            }
            snapshot.pending_color = RGB(215, 135, 30);
        } else {
            snapshot.pending_summary =
                L"No pending setup changes. Choose sources for a new setup, or change recording mode to stage a rebuild of the current managed setup.";
            snapshot.pending_color = RGB(110, 110, 110);
        }

        if (has_pending_setup_draft_ || staged_output != applied_output) {
            snapshot.readiness_detail =
                L"Pending setup changes are staged. Applying them will update WING routing, REAPER tracks, and playback inputs for the selected sources.\r\n"
                L"Next step: review the staged draft, then click Apply Setup.";
            snapshot.readiness_color = RGB(215, 135, 30);
        } else if (validation_in_progress_ && !validation_snapshot_ready_) {
            snapshot.readiness_detail =
                L"Checking the current live setup against WING and REAPER.\r\n"
                L"Next step: wait for validation to finish, then review whether the managed setup is ready or needs rebuild.";
            snapshot.readiness_color = RGB(95, 95, 95);
        } else if (!latest_validation_details_.empty()) {
            snapshot.readiness_detail = ToWide(latest_validation_details_);
            if (latest_validation_state_ == ValidationState::Ready) {
                snapshot.readiness_detail += L"\r\nSetup is ready. Use Live Mode / Soundcheck Mode to switch the validated setup now, or change recording mode to stage a rebuild of the current managed setup.";
                snapshot.readiness_color = RGB(40, 140, 70);
            } else {
                snapshot.readiness_detail += L"\r\nNext step: review the validation warning. Rebuild the current managed setup if routing changed, or use Choose Sources only if you want a different selection.";
                snapshot.readiness_color = RGB(215, 135, 30);
            }
        } else {
            snapshot.readiness_detail =
                L"Connect to a Wing to validate the current managed setup, then rebuild it only when routing or recording mode needs to change.\r\n"
                L"Next step: connect to a WING, validate the managed setup, then use Choose Sources only when you need a different selection.";
            snapshot.readiness_color = RGB(110, 110, 110);
        }

        const bool can_apply = (has_pending_setup_draft_ && SelectedPendingCount() > 0) ||
                               (!has_pending_setup_draft_ && staged_output != applied_output);
        snapshot.can_apply = can_apply;
        snapshot.can_discard = has_pending_setup_draft_ || staged_output != applied_output;
        snapshot.can_toggle = connected &&
                              !has_pending_setup_draft_ &&
                              staged_output == applied_output &&
                              latest_validation_state_ == ValidationState::Ready;
        snapshot.apply_label = (!has_pending_setup_draft_ && staged_output != applied_output)
            ? L"Rebuild Current Setup"
            : L"Apply Setup";
        snapshot.toggle_label = extension.IsSoundcheckModeEnabled() ? L"Soundcheck Mode" : L"Live Mode";
        snapshot.footer = footer_message_.empty()
            ? L"Connect first, then use the Reaper tab to confirm whether the managed setup is ready, needs rebuild, or needs a different source selection."
            : footer_message_;

        return snapshot;
    }

    size_t SelectedPendingCount() const {
        size_t count = 0;
        for (const auto& channel : pending_setup_channels_) {
            if (channel.selected) {
                ++count;
            }
        }
        return count;
    }

    void RefreshDiscoveryControls(bool keep_selection) {
        std::wstring previous_ip;
        if (keep_selection) {
            previous_ip = SelectedOrManualIp();
        }
        SendMessageW(wing_combo_, CB_RESETCONTENT, 0, 0);
        for (const auto& wing : discovered_wings_) {
            std::string title = wing.name.empty() ? wing.console_ip : wing.name + " (" + wing.console_ip + ")";
            const std::wstring wide = ToWide(title);
            SendMessageW(wing_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        }
        if (!discovered_wings_.empty()) {
            int selection_index = 0;
            if (!previous_ip.empty()) {
                const std::string prev_utf8 = ToUtf8(previous_ip);
                for (size_t i = 0; i < discovered_wings_.size(); ++i) {
                    if (discovered_wings_[i].console_ip == prev_utf8) {
                        selection_index = static_cast<int>(i);
                        break;
                    }
                }
            }
            SendMessageW(wing_combo_, CB_SETCURSEL, selection_index, 0);
            SetWindowTextW(manual_ip_edit_, ToWide(discovered_wings_[static_cast<size_t>(selection_index)].console_ip).c_str());
        }
    }

    void StartScanAsync(bool show_feedback = true) {
        if (scan_in_progress_) {
            return;
        }
        scan_in_progress_ = true;
        footer_message_ = L"Scanning for WING consoles...";
        RefreshAll();
        HWND target = hwnd_;
        std::thread([target, show_feedback]() {
            auto result = std::make_unique<AsyncScanResult>();
            result->wings = ReaperExtension::Instance().DiscoverWings(1500);
            result->show_feedback = show_feedback;
            if (!PostMessageW(target, kMsgAsyncScanComplete, 0, reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    }

    void RunScan(bool show_feedback = true) {
        StartScanAsync(show_feedback);
    }

    void OnWingSelectionChanged() {
        const int index = static_cast<int>(SendMessageW(wing_combo_, CB_GETCURSEL, 0, 0));
        if (index >= 0 && index < static_cast<int>(discovered_wings_.size())) {
            const auto& wing = discovered_wings_[static_cast<size_t>(index)];
            SetWindowTextW(manual_ip_edit_, ToWide(wing.console_ip).c_str());
            auto& extension = ReaperExtension::Instance();
            extension.GetConfig().wing_ip = wing.console_ip;
            SaveConfigIfPossible(extension);
            footer_message_ = L"Selected discovered WING target.";
            RefreshAll();
        }
    }

    void OnManualIpChanged() {
        auto& extension = ReaperExtension::Instance();
        extension.GetConfig().wing_ip = ToUtf8(ReadWindowText(manual_ip_edit_));
        SaveConfigIfPossible(extension);
        footer_message_ = L"Manual WING IP updated.";
        RefreshAll();
    }

    void OnConnectClicked() {
        auto& extension = ReaperExtension::Instance();
        if (connect_in_progress_ || source_load_in_progress_) {
            return;
        }
        pending_choose_sources_after_connect_ = false;
        if (extension.IsConnected()) {
            extension.DisconnectFromWing();
            InvalidateValidationSnapshot();
            footer_message_ = L"Disconnected from WING.";
            RefreshAll();
            return;
        }
        StartConnectAsync(L"connecting", L"Connected to WING.");
    }

    void OnOutputModeChanged() {
        const std::wstring current_mode = CurrentOutputMode();
        const std::wstring applied_mode = ToWide(ReaperExtension::Instance().GetConfig().soundcheck_output_mode);
        pending_output_mode_ = current_mode;
        InvalidateValidationSnapshot();
        if (!has_pending_setup_draft_ && pending_output_mode_ == applied_mode) {
            footer_message_ = L"Recording mode matches the applied setup.";
        } else if (!has_pending_setup_draft_) {
            footer_message_ = L"Recording mode change staged. Rebuild Current Setup will reuse the managed selection.";
        } else {
            footer_message_ = L"Recording mode updated for the staged setup draft.";
        }
        RefreshAll();
    }

    void ApplyPendingSelectionOverlay(std::vector<SourceSelectionInfo>& channels) {
        if (!has_pending_setup_draft_ || pending_setup_channels_.empty()) {
            return;
        }
        std::set<std::pair<int, int>> selected;
        for (const auto& channel : pending_setup_channels_) {
            if (channel.selected) {
                selected.insert({static_cast<int>(channel.kind), channel.source_number});
            }
        }
        for (auto& channel : channels) {
            channel.selected = selected.count({static_cast<int>(channel.kind), channel.source_number}) > 0;
        }
    }

    void OnChooseSources() {
        if (connect_in_progress_ || source_load_in_progress_ || scan_in_progress_) {
            return;
        }
        if (!ReaperExtension::Instance().IsConnected()) {
            pending_choose_sources_after_connect_ = true;
            StartConnectAsync(L"loading sources", L"Connected to WING. Loading selectable sources...");
            return;
        }
        StartChooseSourcesAsync();
    }

    void OnApplySetup() {
        if (connect_in_progress_ || source_load_in_progress_ || scan_in_progress_ || apply_plan_in_progress_ || toggle_in_progress_) {
            return;
        }
        if (!ReaperExtension::Instance().IsConnected()) {
            pending_apply_after_connect_ = true;
            StartConnectAsync(L"applying setup", L"Connected to WING. Preparing live setup...");
            return;
        }
        StartApplySetupAsync();
    }

    void OnDiscardSetup() {
        has_pending_setup_draft_ = false;
        pending_setup_channels_.clear();
        pending_setup_soundcheck_ = true;
        pending_replace_existing_ = true;
        pending_output_mode_ = ToWide(ReaperExtension::Instance().GetConfig().soundcheck_output_mode);
        SelectOutputMode(ToUtf8(pending_output_mode_));
        InvalidateValidationSnapshot();
        footer_message_ = L"Staged setup changes discarded.";
        RefreshAll();
    }

    void OnToggleSoundcheck() {
        if (connect_in_progress_ || source_load_in_progress_ || scan_in_progress_ || apply_plan_in_progress_ || toggle_in_progress_) {
            return;
        }
        if (!ReaperExtension::Instance().IsConnected()) {
            pending_toggle_after_connect_ = true;
            StartConnectAsync(L"toggling soundcheck mode", L"Connected to WING. Toggling soundcheck mode...");
            return;
        }
        StartToggleSoundcheckAsync();
    }

    void SyncPendingSettingsFromConfig() {
        const auto& config = ReaperExtension::Instance().GetConfig();
        pending_recorder_enabled_ = config.recorder_coordination_enabled;
        pending_recorder_target_usb_ = (config.recorder_target == "USBREC");
        pending_recorder_follow_ = config.sd_auto_record_with_reaper;
        pending_recorder_pair_left_ = std::clamp(std::max(1, config.sd_lr_left_input), 1, 7);
        if ((pending_recorder_pair_left_ % 2) == 0) {
            --pending_recorder_pair_left_;
        }
        pending_midi_actions_enabled_ = ReaperExtension::Instance().IsMidiActionsEnabled();
        pending_warning_layer_ = std::clamp(config.warning_flash_cc_layer, 1, 16);
        recorder_settings_dirty_ = false;
        midi_actions_dirty_ = false;
    }

    void SyncWingControlsFromPending() {
        CheckRadioButton(page_wing_, kIdRecorderEnableOff, kIdRecorderEnableOn,
                         pending_recorder_enabled_ ? kIdRecorderEnableOn : kIdRecorderEnableOff);
        CheckRadioButton(page_wing_, kIdRecorderTargetWLive, kIdRecorderTargetUsb,
                         pending_recorder_target_usb_ ? kIdRecorderTargetUsb : kIdRecorderTargetWLive);
        const int pair_id = (pending_recorder_pair_left_ <= 1) ? kIdRecorderPair1 :
                            (pending_recorder_pair_left_ <= 3) ? kIdRecorderPair3 :
                            (pending_recorder_pair_left_ <= 5) ? kIdRecorderPair5 :
                                                                kIdRecorderPair7;
        CheckRadioButton(page_wing_, kIdRecorderPair1, kIdRecorderPair7, pair_id);
        CheckRadioButton(page_wing_, kIdRecorderFollowOff, kIdRecorderFollowOn,
                         pending_recorder_follow_ ? kIdRecorderFollowOn : kIdRecorderFollowOff);
    }

    void SyncControlTabFromPending() {
        CheckRadioButton(page_control_, kIdMidiActionsOff, kIdMidiActionsOn,
                         pending_midi_actions_enabled_ ? kIdMidiActionsOn : kIdMidiActionsOff);
        SendMessageW(warning_layer_combo_, CB_SETCURSEL, std::max(0, pending_warning_layer_ - 1), 0);
    }

    void OnRecorderSettingsChanged() {
        pending_recorder_enabled_ = (IsDlgButtonChecked(page_wing_, kIdRecorderEnableOn) == BST_CHECKED);
        pending_recorder_target_usb_ = (IsDlgButtonChecked(page_wing_, kIdRecorderTargetUsb) == BST_CHECKED);
        pending_recorder_follow_ = (IsDlgButtonChecked(page_wing_, kIdRecorderFollowOn) == BST_CHECKED);
        pending_recorder_pair_left_ = (IsDlgButtonChecked(page_wing_, kIdRecorderPair7) == BST_CHECKED) ? 7 :
                                      (IsDlgButtonChecked(page_wing_, kIdRecorderPair5) == BST_CHECKED) ? 5 :
                                      (IsDlgButtonChecked(page_wing_, kIdRecorderPair3) == BST_CHECKED) ? 3 : 1;
        const auto& config = ReaperExtension::Instance().GetConfig();
        recorder_settings_dirty_ =
            pending_recorder_enabled_ != config.recorder_coordination_enabled ||
            pending_recorder_target_usb_ != (config.recorder_target == "USBREC") ||
            pending_recorder_follow_ != config.sd_auto_record_with_reaper ||
            pending_recorder_pair_left_ != std::max(1, config.sd_lr_left_input);
        footer_message_ = recorder_settings_dirty_
            ? L"Recorder coordination changes staged."
            : L"Recorder coordination matches the applied settings.";
        RefreshAll();
    }

    void OnApplyRecorderSettings() {
        auto& extension = ReaperExtension::Instance();
        auto& config = extension.GetConfig();
        config.recorder_coordination_enabled = pending_recorder_enabled_;
        config.sd_lr_route_enabled = pending_recorder_enabled_;
        config.recorder_target = pending_recorder_target_usb_ ? "USBREC" : "WLIVE";
        config.sd_auto_record_with_reaper = pending_recorder_follow_;
        config.sd_lr_group = "MAIN";
        config.sd_lr_left_input = pending_recorder_pair_left_;
        config.sd_lr_right_input = pending_recorder_pair_left_ + 1;
        SaveConfigIfPossible(extension);
        extension.ApplyAutoRecordSettings();
        if (extension.IsConnected() && config.recorder_coordination_enabled) {
            extension.ApplyRecorderRoutingNoDialog();
        }
        recorder_settings_dirty_ = false;
        footer_message_ = L"Recorder coordination settings applied.";
        RefreshAll();
    }

    void OnDiscardRecorderSettings() {
        SyncPendingSettingsFromConfig();
        SyncWingControlsFromPending();
        footer_message_ = L"Recorder coordination changes discarded.";
        RefreshAll();
    }

    void OnMidiActionsChanged() {
        pending_midi_actions_enabled_ = (IsDlgButtonChecked(page_control_, kIdMidiActionsOn) == BST_CHECKED);
        const LRESULT selection = SendMessageW(warning_layer_combo_, CB_GETCURSEL, 0, 0);
        pending_warning_layer_ = (selection >= 0) ? static_cast<int>(selection) + 1 : 1;
        auto& extension = ReaperExtension::Instance();
        const auto& config = extension.GetConfig();
        midi_actions_dirty_ =
            pending_midi_actions_enabled_ != extension.IsMidiActionsEnabled() ||
            pending_warning_layer_ != config.warning_flash_cc_layer;
        footer_message_ = midi_actions_dirty_
            ? L"Control integration changes staged."
            : L"Control integration matches the applied settings.";
        RefreshAll();
    }

    void OnApplyMidiActions() {
        auto& extension = ReaperExtension::Instance();
        extension.GetConfig().warning_flash_cc_layer = pending_warning_layer_;
        SaveConfigIfPossible(extension);
        extension.EnableMidiActions(pending_midi_actions_enabled_);
        if (pending_midi_actions_enabled_ && extension.IsConnected()) {
            extension.SyncMidiActionsToWing();
        }
        midi_actions_dirty_ = false;
        footer_message_ = L"Control integration settings applied.";
        RefreshAll();
    }

    void OnDiscardMidiActions() {
        SyncPendingSettingsFromConfig();
        SyncControlTabFromPending();
        footer_message_ = L"Control integration changes discarded.";
        RefreshAll();
    }

    void OnOpenDebugLog() {
        if (!debug_log_view_) {
            return;
        }
        debug_log_popup_.Show(hwnd_, CurrentLogText());
        footer_message_ = L"Diagnostics log popped out.";
        RefreshAll();
    }

    void OnClearDebugLog() {
        {
            std::lock_guard<std::mutex> lock(log_buffer_mutex_);
            pending_log_buffer_.clear();
        }
        if (debug_log_view_) {
            SetWindowTextW(debug_log_view_, L"");
        }
        footer_message_ = L"Diagnostics log cleared.";
        RefreshAll();
    }

    HWND hwnd_ = nullptr;
    HWND banner_group_ = nullptr;
    HWND status_group_ = nullptr;
    HWND logo_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND tab_ = nullptr;
    HWND page_frame_ = nullptr;
    HWND header_console_icon_ = nullptr;
    HWND header_console_status_ = nullptr;
    HWND header_validation_icon_ = nullptr;
    HWND header_validation_status_ = nullptr;
    HWND header_recorder_icon_ = nullptr;
    HWND header_recorder_status_ = nullptr;
    HWND header_midi_icon_ = nullptr;
    HWND header_midi_status_ = nullptr;
    HWND page_console_ = nullptr;
    HWND page_reaper_ = nullptr;
    HWND page_wing_ = nullptr;
    HWND page_control_ = nullptr;
    HWND tab_status_console_ = nullptr;
    HWND tab_status_reaper_ = nullptr;
    HWND tab_status_wing_ = nullptr;
    HWND tab_status_control_ = nullptr;
    HWND console_intro_ = nullptr;
    HWND console_section_icon_ = nullptr;
    HWND console_section_header_ = nullptr;
    HWND console_label_ = nullptr;
    HWND console_help_discovery_ = nullptr;
    HWND console_manual_ip_label_ = nullptr;
    HWND console_help_manual_ = nullptr;
    HWND console_footer_ = nullptr;
    HWND reaper_intro_ = nullptr;
    HWND reaper_section_icon_ = nullptr;
    HWND reaper_section_header_ = nullptr;
    HWND reaper_output_label_ = nullptr;
    HWND reaper_output_help_ = nullptr;
    HWND reaper_toggle_help_ = nullptr;
    HWND auto_trigger_header_ = nullptr;
    HWND auto_trigger_section_icon_ = nullptr;
    HWND auto_trigger_detail_ = nullptr;
    HWND auto_trigger_hint_ = nullptr;
    HWND auto_trigger_enable_label_ = nullptr;
    HWND auto_trigger_enable_off_ = nullptr;
    HWND auto_trigger_enable_on_ = nullptr;
    HWND auto_trigger_monitor_label_ = nullptr;
    HWND auto_trigger_monitor_combo_ = nullptr;
    HWND auto_trigger_mode_label_ = nullptr;
    HWND auto_trigger_mode_warning_ = nullptr;
    HWND auto_trigger_mode_record_ = nullptr;
    HWND auto_trigger_threshold_label_ = nullptr;
    HWND auto_trigger_threshold_edit_ = nullptr;
    HWND auto_trigger_hold_label_ = nullptr;
    HWND auto_trigger_hold_edit_ = nullptr;
    HWND auto_trigger_meter_label_ = nullptr;
    HWND apply_auto_trigger_button_ = nullptr;
    HWND discard_auto_trigger_button_ = nullptr;
    HWND wing_intro_ = nullptr;
    HWND wing_section_icon_ = nullptr;
    HWND wing_section_header_ = nullptr;
    HWND wing_enable_label_ = nullptr;
    HWND wing_enable_off_ = nullptr;
    HWND wing_enable_on_ = nullptr;
    HWND wing_target_label_ = nullptr;
    HWND wing_target_wlive_ = nullptr;
    HWND wing_target_usb_ = nullptr;
    HWND wing_pair_label_ = nullptr;
    HWND wing_pair_1_ = nullptr;
    HWND wing_pair_3_ = nullptr;
    HWND wing_pair_5_ = nullptr;
    HWND wing_pair_7_ = nullptr;
    HWND wing_follow_label_ = nullptr;
    HWND wing_follow_off_ = nullptr;
    HWND wing_follow_on_ = nullptr;
    HWND apply_recorder_button_ = nullptr;
    HWND discard_recorder_button_ = nullptr;
    HWND control_intro_ = nullptr;
    HWND control_section_icon_ = nullptr;
    HWND control_section_header_ = nullptr;
    HWND control_enable_label_ = nullptr;
    HWND midi_actions_off_ = nullptr;
    HWND midi_actions_on_ = nullptr;
    HWND midi_summary_ = nullptr;
    HWND midi_detail_ = nullptr;
    HWND warning_layer_label_ = nullptr;
    HWND warning_layer_combo_ = nullptr;
    HWND apply_midi_button_ = nullptr;
    HWND discard_midi_button_ = nullptr;
    HWND support_section_header_ = nullptr;
    HWND support_detail_ = nullptr;
    HWND open_debug_log_button_ = nullptr;
    HWND clear_debug_log_button_ = nullptr;
    HWND debug_log_view_ = nullptr;
    HWND wing_placeholder_body_ = nullptr;
    HWND control_placeholder_body_ = nullptr;
    HWND wing_combo_ = nullptr;
    HWND scan_button_ = nullptr;
    HWND manual_ip_edit_ = nullptr;
    HWND connect_button_ = nullptr;
    HWND output_usb_radio_ = nullptr;
    HWND output_card_radio_ = nullptr;
    HWND pending_summary_ = nullptr;
    HWND readiness_detail_ = nullptr;
    HWND choose_sources_button_ = nullptr;
    HWND apply_setup_button_ = nullptr;
    HWND discard_setup_button_ = nullptr;
    HWND toggle_soundcheck_button_ = nullptr;
    HWND footer_status_ = nullptr;
    UINT dpi_ = WindowsUi::kBaseDpi;
    HFONT font_ = nullptr;
    HFONT bold_font_ = nullptr;
    HFONT small_bold_font_ = nullptr;
    HFONT section_font_ = nullptr;
    HFONT tab_font_ = nullptr;
    HFONT subtle_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    HFONT icon_font_ = nullptr;
    HBRUSH banner_brush_ = nullptr;
    HBRUSH status_panel_brush_ = nullptr;
    HBRUSH card_brush_ = nullptr;
    HBRUSH body_brush_ = nullptr;
    HBRUSH border_brush_ = nullptr;
    RECT banner_rect_{};
    RECT status_panel_rect_{};
    PageLayoutState console_page_state_;
    PageLayoutState reaper_page_state_;
    PageLayoutState wing_page_state_;
    PageLayoutState control_page_state_;
    std::array<PageContext, 4> page_contexts_{};
    std::vector<WingInfo> discovered_wings_;
    std::vector<SourceSelectionInfo> pending_setup_channels_;
    std::wstring pending_output_mode_;
    bool has_pending_setup_draft_ = false;
    bool pending_setup_soundcheck_ = true;
    bool pending_replace_existing_ = true;
    bool pending_auto_record_enabled_ = false;
    bool pending_auto_record_warning_only_ = false;
    double pending_auto_record_threshold_db_ = -40.0;
    int pending_auto_record_hold_ms_ = 3000;
    int pending_auto_record_monitor_track_ = 0;
    bool auto_trigger_dirty_ = false;
    ValidationState latest_validation_state_ = ValidationState::NotReady;
    std::string latest_validation_details_;
    DWORD last_validation_tick_ = 0;
    bool validation_snapshot_ready_ = false;
    bool validation_in_progress_ = false;
    unsigned long long validation_generation_ = 1;
    std::wstring footer_message_;
    StatusSnapshot current_snapshot_;
    int current_tab_index_ = 0;
    bool pending_recorder_enabled_ = false;
    bool pending_recorder_target_usb_ = false;
    bool pending_recorder_follow_ = false;
    int pending_recorder_pair_left_ = 1;
    bool recorder_settings_dirty_ = false;
    bool pending_midi_actions_enabled_ = false;
    int pending_warning_layer_ = 1;
    bool midi_actions_dirty_ = false;
    int last_monitor_track_count_ = -1;
    bool scan_in_progress_ = false;
    bool connect_in_progress_ = false;
    bool source_load_in_progress_ = false;
    bool apply_plan_in_progress_ = false;
    bool toggle_in_progress_ = false;
    bool pending_choose_sources_after_connect_ = false;
    bool pending_apply_after_connect_ = false;
    bool pending_toggle_after_connect_ = false;
    std::mutex log_buffer_mutex_;
    std::wstring pending_log_buffer_;
    DebugLogPopup debug_log_popup_;
};

WingConnectorWindowsDialog* WingConnectorWindowsDialog::instance_ = nullptr;

}  // namespace

extern "C" void ShowWingConnectorDialogWindows() {
    WingConnectorWindowsDialog::Show();
}

extern "C" bool ShowExistingProjectAdoptionEditor(const std::vector<AdoptionEditorRow>& rows,
                                                   const std::vector<int>& available_channels,
                                                   const char* initial_output_mode,
                                                   std::string& output_mode_out,
                                                   std::string& channel_overrides_spec_out,
                                                   std::string& slot_overrides_spec_out,
                                                   bool& apply_now_out) {
    AdoptionEditorDialog dialog(rows, available_channels, initial_output_mode);
    return dialog.Run(output_mode_out, channel_overrides_spec_out, slot_overrides_spec_out, apply_now_out);
}

#endif
