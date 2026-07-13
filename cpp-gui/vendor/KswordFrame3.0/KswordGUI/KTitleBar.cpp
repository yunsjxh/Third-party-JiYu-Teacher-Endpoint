#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>

#include "KTitleBar.h"

#include "KTheme.h"
#include "../resource.h"
#include "../KswordWinAPICore/ksword.h"

#include "Fl.H"
#include "Fl_ICO_Image.H"
#include "Fl_PNG_Image.H"
#include "fl_draw.H"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// The bundled FLTK headers in this repository are included without an FL/
// prefix, while platform.H expects that layout. Declare only the Win32 handle
// bridge we need and keep the implementation linked from the existing FLTK lib.
extern HWND fl_win32_xid(const Fl_Window* window);

namespace {
constexpr int kTitleBarHeight = 42;
constexpr int kCaptionButtonWidth = 46;
constexpr int kCaptionIconSize = 24;
constexpr int kLogoTargetHeight = 26;
constexpr int kMinimumResizeBorder = 6;
const wchar_t kChromeWndProcProp[] = L"KswordFrame3.CustomChrome.PreviousWndProc";
const wchar_t kChromeWindowProp[] = L"KswordFrame3.CustomChrome.FlWindow";

// ClampByte keeps manual color blending inside the displayable 8-bit range.
unsigned char ClampByte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<unsigned char>(value);
}

// BlendColor approximates alpha composition for FLTK colors, which are drawn as
// opaque values. Inputs are foreground/background colors and foreground opacity;
// output is a new RGB FLTK color.
Fl_Color BlendColor(Fl_Color foreground, Fl_Color background, double opacity) {
    opacity = std::max(0.0, std::min(1.0, opacity));
    uchar fr = 0;
    uchar fg = 0;
    uchar fb = 0;
    uchar br = 0;
    uchar bg = 0;
    uchar bb = 0;
    Fl::get_color(foreground, fr, fg, fb);
    Fl::get_color(background, br, bg, bb);
    const double inv = 1.0 - opacity;
    return fl_rgb_color(
        ClampByte(static_cast<int>(fr * opacity + br * inv + 0.5)),
        ClampByte(static_cast<int>(fg * opacity + bg * inv + 0.5)),
        ClampByte(static_cast<int>(fb * opacity + bb * inv + 0.5)));
}

// FileExists checks whether a path points to a readable filesystem object.
bool FileExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attrs = ::GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// DirectoryOf extracts the parent directory from a Windows path.
std::string DirectoryOf(const std::string& path) {
    const std::size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return "";
    }
    return path.substr(0, pos);
}

// JoinPath appends a relative path to a base directory using a backslash.
std::string JoinPath(const std::string& base, const std::string& child) {
    if (base.empty()) {
        return child;
    }
    const char last = base[base.size() - 1];
    if (last == '\\' || last == '/') {
        return base + child;
    }
    return base + "\\" + child;
}

// ExecutableDirectory returns the directory containing the running module.
std::string ExecutableDirectory() {
    char modulePath[MAX_PATH] = {};
    const DWORD written = ::GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (written == 0 || written >= MAX_PATH) {
        return "";
    }
    return DirectoryOf(modulePath);
}

// ReadResourceBytes reads one RT_RCDATA resource from the current executable.
std::vector<unsigned char> ReadResourceBytes(int resourceId) {
    HMODULE module = ::GetModuleHandleW(nullptr);
    if (!module) {
        return {};
    }
    HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return {};
    }
    const DWORD size = ::SizeofResource(module, resource);
    HGLOBAL loaded = ::LoadResource(module, resource);
    if (!loaded || size == 0) {
        return {};
    }
    const void* data = ::LockResource(loaded);
    if (!data) {
        return {};
    }
    const unsigned char* begin = static_cast<const unsigned char*>(data);
    return std::vector<unsigned char>(begin, begin + size);
}

// ExtractResourceToTemp writes an embedded resource to a stable temp file for
// FLTK image loaders that accept paths rather than memory buffers.
std::string ExtractResourceToTemp(int resourceId, const char* fileName) {
    const std::vector<unsigned char> bytes = ReadResourceBytes(resourceId);
    if (bytes.empty() || !fileName || !*fileName) {
        return "";
    }

    char tempDir[MAX_PATH] = {};
    const DWORD count = ::GetTempPathA(MAX_PATH, tempDir);
    if (count == 0 || count >= MAX_PATH) {
        return "";
    }

    const std::string path = JoinPath(tempDir, std::string("KswordFrame3_0_") + fileName);
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) {
        return "";
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out ? path : "";
}

// ResolveProjectAsset finds a resource file when running from the source tree or
// from x64/Debug|Release. Input is the resource.h relative path; output may be empty.
std::string ResolveProjectAsset(const char* relativePath) {
    if (!relativePath || !*relativePath) {
        return "";
    }

    const std::string relative(relativePath);
    const std::string exeDir = ExecutableDirectory();
    std::vector<std::string> candidates;
    candidates.push_back(relative);
    candidates.push_back(JoinPath(exeDir, relative));
    candidates.push_back(JoinPath(JoinPath(exeDir, "..\\..\\KswordFrame3.0"), relative));
    candidates.push_back(JoinPath(JoinPath(exeDir, "..\\.."), relative));

    for (const std::string& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return "";
}

// WindowHandle maps an FLTK window to HWND only after the window has been shown.
HWND WindowHandle(Fl_Window* window) {
    if (!window) {
        return nullptr;
    }
    return fl_win32_xid(window);
}

// PointInRect tests absolute FLTK event coordinates against a simple rectangle.
bool PointInRect(int px, int py, const KTitleBar::Rect& rect) {
    return rect.w > 0 && rect.h > 0 && px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h;
}

// SetCaptionLineStyle sets the color and width used to stroke caption glyphs.
void SetCaptionLineStyle(Fl_Color color, int width) {
    fl_color(color);
    fl_line_style(FL_SOLID, width);
}

// FindInstalledTitleBar detects a previously installed KTitleBar child.
KTitleBar* FindInstalledTitleBar(Fl_Window* window) {
    if (!window) {
        return nullptr;
    }
    for (int i = 0; i < window->children(); ++i) {
        KTitleBar* bar = dynamic_cast<KTitleBar*>(window->child(i));
        if (bar) {
            return bar;
        }
    }
    return nullptr;
}

// PreviousChromeWndProc returns the FLTK window procedure saved before the
// custom chrome subclass was installed.
WNDPROC PreviousChromeWndProc(HWND hwnd) {
    return reinterpret_cast<WNDPROC>(::GetPropW(hwnd, kChromeWndProcProp));
}

// CallPreviousChromeWndProc forwards messages to FLTK so normal double-buffered
// client painting and control repaint behavior remain owned by FLTK.
LRESULT CallPreviousChromeWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    WNDPROC previous = PreviousChromeWndProc(hwnd);
    if (previous) {
        return ::CallWindowProcW(previous, hwnd, message, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// WindowFromChromeProp maps a subclassed HWND back to its FLTK owner.
Fl_Window* WindowFromChromeProp(HWND hwnd) {
    return reinterpret_cast<Fl_Window*>(::GetPropW(hwnd, kChromeWindowProp));
}

// NativeResizeBorder returns the Win32 hit-test band in physical pixels.
int NativeResizeBorder() {
    const int frame = ::GetSystemMetrics(SM_CXSIZEFRAME);
    const int padded = ::GetSystemMetrics(SM_CXPADDEDBORDER);
    return std::max(kMinimumResizeBorder, frame + padded);
}

// TitleBarPhysicalHeight converts the logical title-bar height into client
// pixels, accounting for any FLTK logical-to-physical display scale. Returns 0
// when there is no installed title bar.
int TitleBarPhysicalHeight(Fl_Window* window, HWND hwnd) {
    KTitleBar* bar = FindInstalledTitleBar(window);
    if (!bar || !window) {
        return 0;
    }
    RECT clientRect = {};
    if (!::GetClientRect(hwnd, &clientRect)) {
        return 0;
    }
    const int clientHeight = std::max<LONG>(1, clientRect.bottom - clientRect.top);
    const double scaleY = static_cast<double>(clientHeight) / std::max(1, window->h());
    return static_cast<int>(bar->h() * scaleY + 0.5);
}

// ClientPointInCaptionButtonBand protects the minimize/maximize/close buttons
// from native resize/caption hit-tests so FLTK still receives their clicks.
bool ClientPointInCaptionButtonBand(Fl_Window* window, HWND hwnd, POINT screenPoint) {
    KTitleBar* bar = FindInstalledTitleBar(window);
    if (!bar || !window) {
        return false;
    }

    RECT clientRect = {};
    if (!::GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    POINT clientPoint = screenPoint;
    if (!::ScreenToClient(hwnd, &clientPoint)) {
        return false;
    }

    const int clientWidth = std::max<LONG>(1, clientRect.right - clientRect.left);
    const double scaleX = static_cast<double>(clientWidth) / std::max(1, window->w());
    const int buttonSlots = bar->showMaximize() ? 3 : 2;
    const int buttonBand = static_cast<int>(kCaptionButtonWidth * buttonSlots * scaleX + 0.5);
    const int titleHeight = TitleBarPhysicalHeight(window, hwnd);

    return clientPoint.y >= 0 &&
           clientPoint.y < titleHeight &&
           clientPoint.x >= clientWidth - buttonBand &&
           clientPoint.x < clientWidth;
}

// ClientPointInTitleBar reports whether a screen point falls over the draggable
// title strip (used to classify the area as HTCAPTION for native window move).
bool ClientPointInTitleBar(Fl_Window* window, HWND hwnd, POINT screenPoint) {
    const int titleHeight = TitleBarPhysicalHeight(window, hwnd);
    if (titleHeight <= 0) {
        return false;
    }
    RECT clientRect = {};
    if (!::GetClientRect(hwnd, &clientRect)) {
        return false;
    }
    POINT clientPoint = screenPoint;
    if (!::ScreenToClient(hwnd, &clientPoint)) {
        return false;
    }
    return clientPoint.x >= clientRect.left && clientPoint.x < clientRect.right &&
           clientPoint.y >= 0 && clientPoint.y < titleHeight;
}

// NativeResizeHitTest computes border and corner HT* codes for a borderless
// resizable window. It returns HTNOWHERE when the point should not start a
// resize (window not sizable, maximized, over the caption buttons, or interior).
LRESULT NativeResizeHitTest(HWND hwnd, Fl_Window* window, LPARAM lParam) {
    // No resize when the window is maximized or lacks a sizing frame (dialogs).
    if (::IsZoomed(hwnd)) {
        return HTNOWHERE;
    }
    if ((::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) == 0) {
        return HTNOWHERE;
    }

    RECT windowRect = {};
    if (!::GetWindowRect(hwnd, &windowRect)) {
        return HTNOWHERE;
    }

    POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    const int border = NativeResizeBorder();
    bool onLeft = screenPoint.x >= windowRect.left && screenPoint.x < windowRect.left + border;
    bool onRight = screenPoint.x < windowRect.right && screenPoint.x >= windowRect.right - border;
    bool onTop = screenPoint.y >= windowRect.top && screenPoint.y < windowRect.top + border;
    bool onBottom = screenPoint.y < windowRect.bottom && screenPoint.y >= windowRect.bottom - border;

    // The top-right caption buttons must keep receiving FLTK mouse events, so
    // suppress native top/right resize hit-tests inside that button band.
    if ((onTop || onRight) && ClientPointInCaptionButtonBand(window, hwnd, screenPoint)) {
        onTop = false;
        onRight = false;
    }

    if (onTop && onLeft) {
        return HTTOPLEFT;
    }
    if (onTop && onRight) {
        return HTTOPRIGHT;
    }
    if (onBottom && onLeft) {
        return HTBOTTOMLEFT;
    }
    if (onBottom && onRight) {
        return HTBOTTOMRIGHT;
    }
    if (onLeft) {
        return HTLEFT;
    }
    if (onRight) {
        return HTRIGHT;
    }
    if (onTop) {
        return HTTOP;
    }
    if (onBottom) {
        return HTBOTTOM;
    }
    return HTNOWHERE;
}

// ApplyMonitorMaxInfo constrains native maximize operations to the taskbar-safe
// monitor work area so a frameless maximize never overhangs the monitor or hides
// the taskbar.
void ApplyMonitorMaxInfo(HWND hwnd, LPARAM lParam) {
    MINMAXINFO* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
    if (!minMax) {
        return;
    }

    HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !::GetMonitorInfoW(monitor, &info)) {
        return;
    }

    const RECT& work = info.rcWork;
    const RECT& monitorRect = info.rcMonitor;
    minMax->ptMaxPosition.x = work.left - monitorRect.left;
    minMax->ptMaxPosition.y = work.top - monitorRect.top;
    minMax->ptMaxSize.x = work.right - work.left;
    minMax->ptMaxSize.y = work.bottom - work.top;
    // FLTK and Win32 both have a say in maximum tracking size. Keep the maximum
    // track ceiling at the monitor work area so native maximize, Aero Snap, and
    // manual resize never get clamped back to the restore rectangle by stale
    // FLTK sizing data. The actual child layout remains owned by the FLTK
    // resizable widget selected by the caller.
    minMax->ptMaxTrackSize.x = work.right - work.left;
    minMax->ptMaxTrackSize.y = work.bottom - work.top;
}

// SyncTitleBarFromNativeSize mirrors native WM_SIZE changes into the FLTK title
// bar so its width and maximize/restore glyph always match the OS window state.
void SyncTitleBarFromNativeSize(HWND hwnd, Fl_Window* window, WPARAM sizeCode) {
    if (!window || sizeCode == SIZE_MINIMIZED) {
        return;
    }

    KTitleBar* bar = FindInstalledTitleBar(window);
    if (!bar) {
        return;
    }

    const bool maximized = sizeCode == SIZE_MAXIMIZED || ::IsZoomed(hwnd);
    bar->syncFromNativeWindow(window->w(), maximized);
}

// ChromeWindowProc adds native-window polish around FLTK: it suppresses the
// gray frame, marks the caption/resize regions, keeps maximize inside the work
// area, and lets Windows own move/snap/double-click behavior for the title bar.
LRESULT CALLBACK ChromeWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) {
        // FLTK's buffered client paint already fills the surface; skipping the
        // system erase avoids the erase-then-redraw flash while moving/resizing.
        return 1;
    }

    if (message == WM_NCPAINT) {
        // The whole HWND is client area (see WM_NCCALCSIZE), so there is no real
        // non-client region to paint. Swallowing WM_NCPAINT stops DWM from
        // stroking the thin gray sizing-border line that otherwise flickers at
        // the window edges.
        return 0;
    }

    if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU) {
        // Alt / F10 would open the invisible system menu and trap keyboard focus
        // in menu mode (looks like a freeze). Real commands (SC_CLOSE from
        // Alt+F4, SC_MINIMIZE/SC_MAXIMIZE/SC_RESTORE, native move/size) are not
        // SC_KEYMENU and fall through to the previous procedure below.
        return 0;
    }

    if (message == WM_NCCALCSIZE && wParam == TRUE) {
        // Keep the entire window as client area (frameless). WM_GETMINMAXINFO
        // constrains the maximized rect to the monitor work area, so returning 0
        // never overhangs the monitor or covers the taskbar.
        return 0;
    }

    if (message == WM_NCHITTEST) {
        Fl_Window* window = WindowFromChromeProp(hwnd);
        const LRESULT resizeHit = NativeResizeHitTest(hwnd, window, lParam);
        if (resizeHit != HTNOWHERE) {
            return resizeHit;
        }
        POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (ClientPointInCaptionButtonBand(window, hwnd, screenPoint)) {
            // Caption buttons stay client so FLTK receives the click.
            return HTCLIENT;
        }
        if (ClientPointInTitleBar(window, hwnd, screenPoint)) {
            // The rest of the title strip is the caption: Windows performs native
            // move, Aero Snap, and double-click maximize/restore from here.
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    if (message == WM_GETMINMAXINFO) {
        const LRESULT result = CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
        ApplyMonitorMaxInfo(hwnd, lParam);
        return result;
    }

    if (message == WM_SIZE) {
        const LRESULT result = CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
        SyncTitleBarFromNativeSize(hwnd, WindowFromChromeProp(hwnd), wParam);
        return result;
    }

    if (message == WM_NCDESTROY) {
        WNDPROC previous = PreviousChromeWndProc(hwnd);
        const LRESULT result = CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
        if (previous) {
            ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        }
        ::RemovePropW(hwnd, kChromeWndProcProp);
        ::RemovePropW(hwnd, kChromeWindowProp);
        return result;
    }

    return CallPreviousChromeWndProc(hwnd, message, wParam, lParam);
}

// InstallChromeSubclass attaches ChromeWindowProc once per HWND.
bool InstallChromeSubclass(Fl_Window* window, HWND hwnd) {
    if (!window || !hwnd) {
        return false;
    }

    ::SetPropW(hwnd, kChromeWindowProp, reinterpret_cast<HANDLE>(window));
    if (::GetPropW(hwnd, kChromeWndProcProp)) {
        return true;
    }

    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ChromeWindowProc));
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        ::RemovePropW(hwnd, kChromeWindowProp);
        return false;
    }

    if (!::SetPropW(hwnd, kChromeWndProcProp, reinterpret_cast<HANDLE>(previous))) {
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, previous);
        ::RemovePropW(hwnd, kChromeWindowProp);
        return false;
    }
    return true;
}

// ShouldExposeOnTaskbar identifies the primary maximizable application window.
bool ShouldExposeOnTaskbar(Fl_Window* window) {
    KTitleBar* bar = FindInstalledTitleBar(window);
    return bar && bar->showMaximize() && window && window->parent() == nullptr;
}

// ConfigureNativeAppWindow gives the primary borderless window the native styles
// that enable minimize, maximize, resize, Aero Snap, drop shadow, and a taskbar
// button. Inputs are the FLTK owner and HWND; output is none.
void ConfigureNativeAppWindow(Fl_Window* window, HWND hwnd) {
    if (!ShouldExposeOnTaskbar(window) || !hwnd) {
        return;
    }

    KEnsureAppUserModelID();

    // WS_MINIMIZEBOX/WS_MAXIMIZEBOX enable ShowWindow() minimize/maximize plus the
    // taskbar animations; WS_SYSMENU keeps the taskbar context menu. WS_THICKFRAME
    // must stay enabled even for a frameless window: Windows uses it to decide
    // whether the HWND participates in edge resize, drag-to-top maximize, snap
    // restore, and Win+arrow tiling. WM_NCCALCSIZE/WM_NCPAINT below still remove
    // the native caption/border pixels, while WM_NCHITTEST supplies our custom
    // resize and caption zones.
    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR desiredStyle = (style | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME) & ~(WS_CAPTION | WS_BORDER | WS_DLGFRAME);
    if (desiredStyle != style) {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, desiredStyle);
    }

    LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR desiredExStyle = (exStyle & ~(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) | WS_EX_APPWINDOW;
    if (desiredExStyle != exStyle) {
        ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, desiredExStyle);
    }

    if (::GetWindow(hwnd, GW_OWNER)) {
        ::SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
    }

    ::SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
}

KTitleBar::KTitleBar(int x, int y, int w, int h, const char* title, KTitleBarStyle style, bool showMaximize)
    : Fl_Group(x, y, w, h, title),
      style_(style),
      show_maximize_(showMaximize),
      maximized_(false),
      hover_button_(Button::None),
      pressed_button_(Button::None),
      title_(title ? title : "KswordFrame3.0"),
      icon_image_(),
      icon_scaled_(),
      logo_image_(),
      logo_scaled_() {
    box(FL_NO_BOX);
    color(KThemeManager::instance().theme().windowBg);
}

KTitleBar::~KTitleBar() = default;

void KTitleBar::draw() {
    drawBackground();
    drawBrand();

    drawButton(Button::Minimize);
    if (show_maximize_) {
        drawButton(Button::Maximize);
    }
    drawButton(Button::Close);

    const KTheme& theme = KThemeManager::instance().theme();
    fl_color(theme.border);
    fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1);
}

int KTitleBar::handle(int event) {
    const int event_x = Fl::event_x();
    const int event_y = Fl::event_y();

    switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
        const Button next = hitButton(event_x, event_y);
        if (next != hover_button_) {
            hover_button_ = next;
            redraw();
        }
        return 1;
    }
    case FL_LEAVE: {
        if (hover_button_ != Button::None || pressed_button_ != Button::None) {
            hover_button_ = Button::None;
            pressed_button_ = Button::None;
            redraw();
        }
        return 1;
    }
    case FL_PUSH: {
        // Only the caption buttons are handled here. The draggable title area is
        // classified as HTCAPTION by the chrome WndProc, so Windows already owns
        // move / Aero Snap / double-click-maximize and those events never arrive.
        if (Fl::event_button() == FL_LEFT_MOUSE) {
            const Button hit = hitButton(event_x, event_y);
            if (hit != Button::None) {
                pressed_button_ = hit;
                redraw();
                return 1;
            }
        }
        break;
    }
    case FL_DRAG: {
        if (pressed_button_ != Button::None) {
            const Button next = hitButton(event_x, event_y);
            if (next != hover_button_) {
                hover_button_ = next;
                redraw();
            }
            return 1;
        }
        break;
    }
    case FL_RELEASE: {
        const Button released = pressed_button_;
        pressed_button_ = Button::None;
        if (released != Button::None) {
            if (released == hitButton(event_x, event_y)) {
                triggerButton(released);
            }
            redraw();
            return 1;
        }
        break;
    }
    default:
        break;
    }

    return Fl_Group::handle(event);
}

void KTitleBar::setStyle(KTitleBarStyle style) {
    if (style_ == style) {
        return;
    }
    style_ = style;
    redraw();
}

KTitleBarStyle KTitleBar::style() const {
    return style_;
}

void KTitleBar::setShowMaximize(bool showMaximize) {
    if (show_maximize_ == showMaximize) {
        return;
    }
    show_maximize_ = showMaximize;
    redraw();
}

bool KTitleBar::showMaximize() const {
    return show_maximize_;
}

void KTitleBar::syncFromNativeWindow(int ownerWidth, bool maximized) {
    bool changed = false;
    const int safeWidth = std::max(1, ownerWidth);
    if (safeWidth != w() || x() != 0 || y() != 0 || h() != kTitleBarHeight) {
        // WM_SIZE is the only path that changes title-bar geometry; keep the
        // repaint local to this widget so child controls retain FLTK repainting.
        Fl_Group::resize(0, 0, safeWidth, kTitleBarHeight);
        changed = true;
    }
    if (maximized_ != maximized) {
        maximized_ = maximized;
        changed = true;
    }
    if (changed) {
        redraw();
    }
}

void KTitleBar::ensureImages() {
    if (!icon_image_) {
        std::string iconPath = ExtractResourceToTemp(IDR_KSWORD_APP_ICON_ICO, "app.ico");
        if (iconPath.empty()) {
            iconPath = ResolveProjectAsset(KSWORD_APP_ICON_FILE);
        }
        if (!iconPath.empty()) {
            Fl_ICO_Image* rawIcon = new Fl_ICO_Image(iconPath.c_str());
            if (rawIcon->fail()) {
                delete rawIcon;
            }
            else {
                icon_image_.reset(rawIcon);
                icon_scaled_.reset(icon_image_->copy(kCaptionIconSize, kCaptionIconSize));
            }
        }
    }

    if (!logo_image_) {
        std::string logoPath = ExtractResourceToTemp(IDR_KSWORD_APP_LOGO_PNG, "app_logo.png");
        if (logoPath.empty()) {
            logoPath = ResolveProjectAsset(KSWORD_APP_LOGO_FILE);
        }
        if (!logoPath.empty()) {
            Fl_PNG_Image* rawLogo = new Fl_PNG_Image(logoPath.c_str());
            if (rawLogo->fail()) {
                delete rawLogo;
            }
            else {
                logo_image_.reset(rawLogo);
                const int targetWidth = std::max(120, std::min(260, rawLogo->data_w() * kLogoTargetHeight / std::max(1, rawLogo->data_h())));
                logo_scaled_.reset(logo_image_->copy(targetWidth, kLogoTargetHeight));
            }
        }
    }
}

void KTitleBar::drawBackground() {
    const KTheme& theme = KThemeManager::instance().theme();
    const Fl_Color base = theme.windowBg;
    const Fl_Color fill = theme.primary;

    fl_color(base);
    fl_rectf(x(), y(), w(), h());

    if (style_ == KTitleBarStyle::Solid) {
        fl_color(fill);
        fl_rectf(x(), y(), w(), h());
        return;
    }

    if (style_ == KTitleBarStyle::Fade) {
        const int step = 4;
        for (int offset = 0; offset < w(); offset += step) {
            const double progress = static_cast<double>(offset) / std::max(1, w() - 1);
            const double opacity = std::max(0.0, 0.92 - progress * 0.86);
            fl_color(BlendColor(fill, base, opacity));
            fl_rectf(x() + offset, y(), std::min(step, w() - offset), h());
        }
        return;
    }

    if (style_ == KTitleBarStyle::Trapezoid) {
        const int brandWidth = std::min(360, std::max(240, w() / 3));
        const int slant = 34;
        fl_color(BlendColor(fill, base, 0.92));
        fl_begin_polygon();
        fl_vertex(x(), y());
        fl_vertex(x() + brandWidth, y());
        fl_vertex(x() + brandWidth - slant, y() + h());
        fl_vertex(x(), y() + h());
        fl_end_polygon();
    }
}

void KTitleBar::drawBrand() {
    ensureImages();

    const int iconX = x() + 14;
    const int iconY = y() + (h() - kCaptionIconSize) / 2;
    if (icon_scaled_) {
        icon_scaled_->draw(iconX, iconY);
    }
    else {
        drawFallbackIcon(iconX, iconY, kCaptionIconSize);
    }

    const int logoX = iconX + kCaptionIconSize + 12;
    if (logo_scaled_) {
        const int logoY = y() + (h() - logo_scaled_->h()) / 2;
        logo_scaled_->draw(logoX, logoY);
        return;
    }

    const KTheme& theme = KThemeManager::instance().theme();
    const bool onAccent = style_ == KTitleBarStyle::Solid || style_ == KTitleBarStyle::Trapezoid;
    fl_color(onAccent ? FL_WHITE : theme.text);
    fl_font(FL_HELVETICA_BOLD, 14);
    fl_draw(title_.c_str(), logoX, y(), std::max(120, w() / 3), h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

void KTitleBar::drawButton(Button button) {
    if (button == Button::None || (button == Button::Maximize && !show_maximize_)) {
        return;
    }

    const Rect rect = buttonRect(button);
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    const KTheme& theme = KThemeManager::instance().theme();
    const bool hovered = hover_button_ == button;
    const bool pressed = pressed_button_ == button;
    const bool accentText = style_ == KTitleBarStyle::Solid;

    if (hovered || pressed) {
        Fl_Color buttonFill = BlendColor(theme.primary, theme.windowBg, pressed ? 0.24 : 0.14);
        if (button == Button::Close) {
            buttonFill = pressed ? BlendColor(theme.danger, FL_BLACK, 0.92) : theme.danger;
        }
        fl_color(buttonFill);
        fl_rectf(rect.x, rect.y, rect.w, rect.h);
        fl_color(BlendColor(buttonFill, FL_BLACK, pressed ? 0.18 : 0.08));
        fl_line(rect.x, rect.y + rect.h - 1, rect.x + rect.w - 1, rect.y + rect.h - 1);
    }

    Fl_Color iconColor = accentText ? FL_WHITE : theme.text;
    if (button == Button::Close && hovered) {
        iconColor = FL_WHITE;
    }

    const int pressedOffset = pressed ? 1 : 0;
    const int cx = rect.x + rect.w / 2 + pressedOffset;
    const int cy = rect.y + rect.h / 2 + pressedOffset;
    SetCaptionLineStyle(iconColor, 2);

    if (button == Button::Minimize) {
        fl_line(cx - 6, cy + 5, cx + 6, cy + 5);
    }
    else if (button == Button::Maximize) {
        if (maximized_) {
            fl_rect(cx - 5, cy - 3, 10, 8);
            fl_line(cx - 2, cy - 6, cx + 7, cy - 6);
            fl_line(cx + 7, cy - 6, cx + 7, cy + 1);
        }
        else {
            fl_rect(cx - 6, cy - 6, 12, 12);
        }
    }
    else if (button == Button::Close) {
        fl_line(cx - 6, cy - 6, cx + 6, cy + 6);
        fl_line(cx + 6, cy - 6, cx - 6, cy + 6);
    }

    fl_line_style(0);
}

KTitleBar::Rect KTitleBar::buttonRect(Button button) const {
    const int right = x() + w();
    const int top = y();
    const int height = h();

    if (button == Button::Close) {
        return { right - kCaptionButtonWidth, top, kCaptionButtonWidth, height };
    }
    if (button == Button::Maximize && show_maximize_) {
        return { right - kCaptionButtonWidth * 2, top, kCaptionButtonWidth, height };
    }
    if (button == Button::Minimize) {
        const int slot = show_maximize_ ? 3 : 2;
        return { right - kCaptionButtonWidth * slot, top, kCaptionButtonWidth, height };
    }
    return { 0, 0, 0, 0 };
}

KTitleBar::Button KTitleBar::hitButton(int px, int py) const {
    if (PointInRect(px, py, buttonRect(Button::Close))) {
        return Button::Close;
    }
    if (show_maximize_ && PointInRect(px, py, buttonRect(Button::Maximize))) {
        return Button::Maximize;
    }
    if (PointInRect(px, py, buttonRect(Button::Minimize))) {
        return Button::Minimize;
    }
    return Button::None;
}

void KTitleBar::triggerButton(Button button) {
    if (button == Button::Minimize) {
        minimizeWindow();
    }
    else if (button == Button::Maximize) {
        toggleMaximize();
    }
    else if (button == Button::Close) {
        closeWindow();
    }
}

void KTitleBar::minimizeWindow() {
    HWND hwnd = WindowHandle(window());
    if (hwnd) {
        ::ShowWindow(hwnd, SW_MINIMIZE);
        return;
    }
    if (window()) {
        window()->iconize();
    }
}

void KTitleBar::toggleMaximize() {
    if (!show_maximize_) {
        return;
    }
    HWND hwnd = WindowHandle(window());
    if (!hwnd) {
        return;
    }
    ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

void KTitleBar::closeWindow() {
    Fl_Window* owner = window();
    if (owner) {
        owner->hide();
    }
}

void KTitleBar::drawFallbackIcon(int px, int py, int size) {
    const KTheme& theme = KThemeManager::instance().theme();
    fl_color(theme.primary);
    fl_rectf(px, py, size, size);
    fl_color(FL_WHITE);
    fl_font(FL_HELVETICA_BOLD, std::max(12, size - 8));
    fl_draw("K", px, py, size, size, FL_ALIGN_CENTER);
}

int KTitleBar::barHeight() {
    return kTitleBarHeight;
}

int KTitleBarHeight() {
    return kTitleBarHeight;
}

bool KInstallTitleBar(Fl_Window* window, KTitleBarStyle style, bool showMaximize) {
    if (!window) {
        return false;
    }

    if (showMaximize) {
        // Set the shell identity before the main HWND is shown so taskbar
        // grouping is stable even though the icon/style is applied after show().
        KEnsureAppUserModelID();
    }

    KTitleBar* existing = FindInstalledTitleBar(window);
    if (existing) {
        existing->setStyle(style);
        existing->setShowMaximize(showMaximize);
        return true;
    }

    std::vector<Fl_Widget*> children;
    children.reserve(window->children());
    for (int i = 0; i < window->children(); ++i) {
        children.push_back(window->child(i));
    }

    // Temporarily disable FLTK's group resize policy while adding the title bar.
    // If the caller already selected a content root as window->resizable(), the
    // intermediate height increase would otherwise stretch that root before we
    // shift it down below the custom chrome. Restoring the pointer after the
    // children move preserves responsive layouts without double-applying the
    // title-bar offset.
    Fl_Widget* previousResizable = window->resizable();
    window->resizable(nullptr);

    window->border(0);
    const int oldWidth = window->w();
    const int oldHeight = window->h();
    window->size(oldWidth, oldHeight + kTitleBarHeight);

    for (Fl_Widget* child : children) {
        if (!child) {
            continue;
        }
        child->resize(child->x(), child->y() + kTitleBarHeight, child->w(), child->h());
    }

    Fl_Group* previous = Fl_Group::current();
    Fl_Group::current(nullptr);
    KTitleBar* bar = new KTitleBar(0, 0, oldWidth, kTitleBarHeight, window->label(), style, showMaximize);
    Fl_Group::current(previous);
    window->add(bar);
    window->resizable(previousResizable);
    window->init_sizes();
    KThemeManager::instance().ApplyTo(bar);
    window->redraw();
    return true;
}

void KApplyWindowIcon(Fl_Window* window) {
    HWND hwnd = WindowHandle(window);
    if (!hwnd) {
        return;
    }

    InstallChromeSubclass(window, hwnd);
    ConfigureNativeAppWindow(window, hwnd);

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    HICON bigIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_KSWORD_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));
    HICON smallIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_KSWORD_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    if (bigIcon) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    }
    if (smallIcon) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }
}
