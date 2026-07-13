#ifndef KSWORD_GUI_KTITLEBAR_H
#define KSWORD_GUI_KTITLEBAR_H

#include "Fl_Group.H"
#include "Fl_Image.H"
#include "Fl_Window.H"

#include <memory>
#include <string>

// KTitleBarStyle selects one of the four branded title-bar fills requested by
// the application shell. Each style changes only the title-bar background; the
// icon, logo, drag behavior, and caption buttons keep the same contract.
enum class KTitleBarStyle {
    Trapezoid,
    Fade,
    Solid,
    Plain
};

// KTitleBar draws the branded caption strip (icon, logo, min/max/close buttons)
// that replaces the native title bar. It deliberately does NOT move, snap, or
// maximize the window itself: the chrome window procedure marks the drag area as
// HTCAPTION so Windows performs move / Aero Snap / double-click-maximize with its
// own native behavior, and the caption buttons drive ShowWindow(). This keeps a
// single source of truth for the window state (the OS) instead of a second,
// hand-maintained one.
class KTitleBar : public Fl_Group {
public:
    // Creates a self-drawing title bar. It does not install itself into a window;
    // callers use KInstallTitleBar() so existing content can be shifted safely.
    KTitleBar(int x, int y, int w, int h, const char* title, KTitleBarStyle style, bool showMaximize);

    // Releases loaded FLTK image objects. No external pointers are owned.
    ~KTitleBar() override;

    // Draws the selected background, app icon/logo, bottom divider, and caption buttons.
    void draw() override;

    // Handles only the caption buttons (hover, press, minimize/maximize/close).
    // Window move/snap/restore are handled natively through the chrome WndProc.
    int handle(int event) override;

    // Updates the visual fill style and repaints; input is the new style, return is none.
    void setStyle(KTitleBarStyle style);

    // Returns the current visual fill style; input is none.
    KTitleBarStyle style() const;

    // Enables or disables the maximize/restore button and repaints; return is none.
    void setShowMaximize(bool showMaximize);

    // Returns true when the maximize/restore button is displayed.
    bool showMaximize() const;

    // Mirrors native WM_SIZE notifications into the bar: resizes the strip to the
    // new owner width and records whether the OS window is maximized so the
    // maximize/restore glyph matches reality. Return is none.
    void syncFromNativeWindow(int ownerWidth, bool maximized);

    // Small rectangle helper so button hit-tests do not depend on Win32 RECT.
    struct Rect {
        int x;
        int y;
        int w;
        int h;
    };

    // Fixed title-bar height in FLTK logical units; shared with the chrome WndProc.
    static int barHeight();

private:
    // Internal caption-button identifier used by hit-testing and drawing.
    enum class Button {
        None,
        Minimize,
        Maximize,
        Close
    };

    // Loads embedded or project-local icon/logo images lazily; returns no value.
    void ensureImages();

    // Draws the requested background fill over the full title-bar rectangle.
    void drawBackground();

    // Draws app icon and logo, with a text fallback if images cannot be loaded.
    void drawBrand();

    // Draws one caption button in its hover/pressed state.
    void drawButton(Button button);

    // Computes the absolute title-bar rectangle for one caption button.
    Rect buttonRect(Button button) const;

    // Returns which caption button contains the absolute FLTK event coordinate.
    Button hitButton(int px, int py) const;

    // Runs the action attached to a clicked caption button; returns no value.
    void triggerButton(Button button);

    // Minimizes the owning window natively; returns no value.
    void minimizeWindow();

    // Toggles native maximize/restore for the owning window; returns no value.
    void toggleMaximize();

    // Closes the owning window; returns no value.
    void closeWindow();

    // Draws a compact fallback K mark when the external icon cannot be loaded.
    void drawFallbackIcon(int px, int py, int size);

private:
    KTitleBarStyle style_;
    bool show_maximize_;
    bool maximized_;
    Button hover_button_;
    Button pressed_button_;
    std::string title_;
    std::unique_ptr<Fl_Image> icon_image_;
    std::unique_ptr<Fl_Image> icon_scaled_;
    std::unique_ptr<Fl_Image> logo_image_;
    std::unique_ptr<Fl_Image> logo_scaled_;
};

// Returns the fixed custom title-bar height used when shifting window content.
int KTitleBarHeight();

// Installs a custom title bar into a top-level window. Input is the target
// window, style, and maximize-button policy; output is true when installed or
// updated. Existing children are shifted down exactly once.
bool KInstallTitleBar(Fl_Window* window, KTitleBarStyle style = KTitleBarStyle::Trapezoid, bool showMaximize = true);

// Applies the executable icon resource to a shown Win32 window and installs the
// custom-chrome subclass. Input may be null or not-yet-shown; in those cases it
// is ignored and no value is returned.
void KApplyWindowIcon(Fl_Window* window);

#endif // KSWORD_GUI_KTITLEBAR_H
