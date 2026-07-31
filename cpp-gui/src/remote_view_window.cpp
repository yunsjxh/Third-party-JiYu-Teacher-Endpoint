#include "remote_view_window.hpp"

#include "copyable_widgets.hpp"
#include "teacher_service.hpp"

#include "KswordGUI/KswordStyle.h"
#include "Fl.H"
#include "Fl_RGB_Image.H"
#include "fl_draw.H"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>

extern HWND fl_win32_xid(const Fl_Window* window);

namespace jiyu::gui {

RemoteViewWindow* RemoteViewWindow::hook_owner_ = nullptr;

class RemoteViewWindow::Canvas : public Fl_Widget {
public:
    Canvas(int x, int y, int w, int h, std::string student_ip, TeacherService& service)
        : Fl_Widget(x, y, w, h), student_ip_(std::move(student_ip)), service_(service) {
        box(FL_FLAT_BOX);
        color(FL_BLACK);
    }

    void setFrame(RemoteFrame frame) {
        frame_ = std::move(frame);
        redraw();
    }

    void setState(const RemoteViewState& state) {
        controlling_ = state.controlling;
        has_remote_cursor_ = state.has_cursor;
        remote_cursor_x_ = state.cursor_x;
        remote_cursor_y_ = state.cursor_y;
        redraw();
    }

    int handle(int event) override {
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE &&
            (!controlling_ || (Fl::event_state() & FL_SHIFT) != 0)) {
            const int picked = KShowPopupMenu(Fl::event_x_root(), Fl::event_y_root(),
                { "复制当前画面", "复制画面信息" });
            if (picked == 0) copyFrame();
            else if (picked == 1) copyInfo();
            return 1;
        }
        if (!controlling_) {
            if (event == FL_FOCUS || event == FL_UNFOCUS) return 1;
            return Fl_Widget::handle(event);
        }
        switch (event) {
        case FL_ENTER:
        case FL_FOCUS:
            return 1;
        case FL_MOVE:
        case FL_DRAG:
            sendMouse(0x0200, 0);
            return 1;
        case FL_PUSH:
            take_focus();
            if (Fl::event_button() == FL_LEFT_MOUSE) sendMouse(0x0201, 0);
            else if (Fl::event_button() == FL_MIDDLE_MOUSE) sendMouse(0x0207, 0);
            else if (Fl::event_button() == FL_RIGHT_MOUSE) sendMouse(0x0204, 0);
            return 1;
        case FL_RELEASE:
            if (Fl::event_button() == FL_LEFT_MOUSE) sendMouse(0x0202, 0);
            else if (Fl::event_button() == FL_MIDDLE_MOUSE) sendMouse(0x0208, 0);
            else if (Fl::event_button() == FL_RIGHT_MOUSE) sendMouse(0x0205, 0);
            return 1;
        case FL_MOUSEWHEEL:
            sendMouse(0x020a, -Fl::event_dy() * 120);
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        fl_color(FL_BLACK);
        fl_rectf(x(), y(), w(), h());
        int draw_x = x();
        int draw_y = y();
        int draw_w = w();
        int draw_h = h();
        if (frame_.width > 0 && frame_.height > 0 && !frame_.rgb.empty()) {
            imageRect(draw_x, draw_y, draw_w, draw_h);
            Fl_RGB_Image source(frame_.rgb.data(), frame_.width, frame_.height, 3);
            std::unique_ptr<Fl_Image> scaled(source.copy(draw_w, draw_h));
            if (scaled) scaled->draw(draw_x, draw_y);

            const auto now = std::chrono::steady_clock::now();
            const bool predicted = has_local_cursor_ && now - last_local_cursor_ < std::chrono::milliseconds(180);
            if (predicted || has_remote_cursor_) {
                const int source_x = predicted ? local_cursor_x_ : remote_cursor_x_;
                const int source_y = predicted ? local_cursor_y_ : remote_cursor_y_;
                const int cursor_x = draw_x + std::clamp(source_x, 0, frame_.width - 1) * std::max(1, draw_w - 1) / std::max(1, frame_.width - 1);
                const int cursor_y = draw_y + std::clamp(source_y, 0, frame_.height - 1) * std::max(1, draw_h - 1) / std::max(1, frame_.height - 1);
                const int arm = std::max(8, std::min(draw_w, draw_h) / 60);
                fl_color(FL_BLACK); fl_line_style(FL_SOLID, 5);
                fl_line(cursor_x - arm, cursor_y, cursor_x + arm, cursor_y);
                fl_line(cursor_x, cursor_y - arm, cursor_x, cursor_y + arm);
                fl_color(FL_WHITE); fl_line_style(FL_SOLID, 2);
                fl_line(cursor_x - arm, cursor_y, cursor_x + arm, cursor_y);
                fl_line(cursor_x, cursor_y - arm, cursor_x, cursor_y + arm);
                fl_line_style(0);
            }
        } else {
            fl_color(FL_WHITE);
            fl_font(FL_HELVETICA, 16);
            fl_draw("正在等待视频画面...", x(), y(), w(), h(), FL_ALIGN_CENTER);
        }
        fl_pop_clip();
    }

private:
    void imageRect(int& draw_x, int& draw_y, int& draw_w, int& draw_h) const {
        if (frame_.width <= 0 || frame_.height <= 0) return;
        const double scale = std::min(static_cast<double>(w()) / frame_.width, static_cast<double>(h()) / frame_.height);
        draw_w = std::max(1, static_cast<int>(std::lround(frame_.width * scale)));
        draw_h = std::max(1, static_cast<int>(std::lround(frame_.height * scale)));
        draw_x = x() + (w() - draw_w) / 2;
        draw_y = y() + (h() - draw_h) / 2;
    }

    void sendMouse(std::uint32_t message, int data) {
        if (frame_.width <= 0 || frame_.height <= 0) return;
        const auto now = std::chrono::steady_clock::now();
        if (message == 0x0200 && now - last_move_ < std::chrono::milliseconds(8)) return;
        if (message == 0x0200) last_move_ = now;
        int draw_x = x(), draw_y = y(), draw_w = w(), draw_h = h();
        imageRect(draw_x, draw_y, draw_w, draw_h);
        const int relative_x = std::clamp(Fl::event_x() - draw_x, 0, std::max(0, draw_w - 1));
        const int relative_y = std::clamp(Fl::event_y() - draw_y, 0, std::max(0, draw_h - 1));
        const std::uint32_t normalized_x = static_cast<std::uint32_t>(relative_x) * 65535U / std::max(1, draw_w - 1);
        const std::uint32_t normalized_y = static_cast<std::uint32_t>(relative_y) * 65535U / std::max(1, draw_h - 1);
        local_cursor_x_ = static_cast<int>(normalized_x * static_cast<std::uint32_t>(std::max(0, frame_.width - 1)) / 65535U);
        local_cursor_y_ = static_cast<int>(normalized_y * static_cast<std::uint32_t>(std::max(0, frame_.height - 1)) / 65535U);
        has_local_cursor_ = true;
        last_local_cursor_ = now;
        service_.sendRemoteMouse(student_ip_, message, normalized_x, normalized_y, data);
        redraw();
    }

    void copyInfo() const {
        std::ostringstream text;
        text << student_ip_ << "  " << frame_.width << 'x' << frame_.height << "  frame=" << frame_.sequence;
        const auto value = text.str();
        Fl::copy(value.c_str(), static_cast<int>(value.size()), 1);
    }

    void copyFrame() const {
        if (frame_.width <= 0 || frame_.height <= 0 || frame_.rgb.empty()) return;
        const std::size_t stride = (static_cast<std::size_t>(frame_.width) * 3 + 3) & ~std::size_t(3);
        const std::size_t pixels = stride * frame_.height;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels);
        if (!memory) return;
        auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
        std::memset(header, 0, sizeof(*header));
        header->biSize = sizeof(*header);
        header->biWidth = frame_.width;
        header->biHeight = frame_.height;
        header->biPlanes = 1;
        header->biBitCount = 24;
        header->biCompression = BI_RGB;
        header->biSizeImage = static_cast<DWORD>(pixels);
        auto* output = reinterpret_cast<std::uint8_t*>(header + 1);
        for (int y = 0; y < frame_.height; ++y) {
            auto* row = output + static_cast<std::size_t>(frame_.height - 1 - y) * stride;
            for (int x = 0; x < frame_.width; ++x) {
                const auto source = (static_cast<std::size_t>(y) * frame_.width + x) * 3;
                const auto target = static_cast<std::size_t>(x) * 3;
                row[target + 0] = frame_.rgb[source + 2];
                row[target + 1] = frame_.rgb[source + 1];
                row[target + 2] = frame_.rgb[source + 0];
            }
        }
        GlobalUnlock(memory);
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_DIB, memory)) GlobalFree(memory);
            CloseClipboard();
        } else {
            GlobalFree(memory);
        }
    }

    std::string student_ip_;
    TeacherService& service_;
    RemoteFrame frame_;
    bool controlling_ = false;
    bool has_remote_cursor_ = false;
    int remote_cursor_x_ = 0;
    int remote_cursor_y_ = 0;
    bool has_local_cursor_ = false;
    int local_cursor_x_ = 0;
    int local_cursor_y_ = 0;
    std::chrono::steady_clock::time_point last_local_cursor_{};
    std::chrono::steady_clock::time_point last_move_{};
};

RemoteViewWindow::RemoteViewWindow(std::string student_ip, TeacherService& service)
    : Fl_Double_Window(1000, 760), student_ip_(std::move(student_ip)), service_(service) {
    const std::string title = "学生屏幕 " + student_ip_;
    copy_label(title.c_str());
    SetWindowStyle(this);
    callback(CloseThunk, this);
    begin();
    canvas_ = new Canvas(16, 16, 968, 646, student_ip_, service_);
    control_button_ = KCreateButton(16, 680, 112, 34, "开始远控", KBUTTON_HEAVY);
    control_button_->callback(StartControlThunk, this);
    auto* stop_control = KCreateButton(138, 680, 112, 34, "停止远控", KBUTTON_LIGHT);
    stop_control->callback(StopControlThunk, this);
    auto* stop_view = KCreateButton(260, 680, 112, 34, "停止观看", KBUTTON_LIGHT);
    stop_view->callback(StopViewThunk, this);
    status_ = new CopyableTextDisplay(390, 676, 594, 60, nullptr);
    status_->set_text("正在连接；状态文本和画面均可右键复制。\n");
    end();
    resizable(canvas_);
}

RemoteViewWindow::~RemoteViewWindow() {
    uninstallKeyboardHook();
    service_.stopRemoteView(student_ip_, true);
    KThemeManager::instance().UnregisterWindow(this);
}

const std::string& RemoteViewWindow::studentIp() const { return student_ip_; }

void RemoteViewWindow::updateFrame(RemoteFrame frame) {
    if (canvas_) canvas_->setFrame(std::move(frame));
}

void RemoteViewWindow::refreshState(const RemoteViewState& state) {
    if (canvas_) canvas_->setState(state);
    if (state.controlling != controlling_) {
        controlling_ = state.controlling;
        if (controlling_) installKeyboardHook(); else uninstallKeyboardHook();
        const std::string title = (controlling_ ? "远程控制 " : "学生屏幕 ") + student_ip_;
        copy_label(title.c_str());
    }
    std::ostringstream text;
    text << (state.connected ? "TCP 已连接" : (state.active ? "正在连接" : "已停止"))
         << " · 画面 " << state.frame_count
         << " · 远控 " << (state.controlling ? "已启用" : "未启用");
    if (state.controlling) text << " · UDP " << state.control_port;
    text << "\n右键画面复制；远控时 Shift+右键打开复制菜单。";
    status_->set_text(text.str().c_str());
}

void RemoteViewWindow::showAndFocus() {
    show();
    take_focus();
}

void RemoteViewWindow::StartControlThunk(Fl_Widget*, void* data) { static_cast<RemoteViewWindow*>(data)->startControl(); }
void RemoteViewWindow::StopControlThunk(Fl_Widget*, void* data) { static_cast<RemoteViewWindow*>(data)->stopControl(); }
void RemoteViewWindow::StopViewThunk(Fl_Widget*, void* data) { static_cast<RemoteViewWindow*>(data)->stopView(); }
void RemoteViewWindow::CloseThunk(Fl_Widget*, void* data) { static_cast<RemoteViewWindow*>(data)->closeWindow(); }

void RemoteViewWindow::startControl() {
    std::string error;
    if (!service_.startRemoteControl(student_ip_, 0, &error)) {
        KShowMessage("无法开始远控", error.c_str());
    }
}

void RemoteViewWindow::stopControl() { service_.stopRemoteControl(student_ip_, true); }

void RemoteViewWindow::stopView() {
    service_.stopRemoteView(student_ip_, true);
    hide();
}

void RemoteViewWindow::closeWindow() { stopView(); }

void RemoteViewWindow::installKeyboardHook() {
    if (keyboard_hook_) return;
    if (hook_owner_ && hook_owner_ != this) hook_owner_->uninstallKeyboardHook();
    hook_owner_ = this;
    keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
}

void RemoteViewWindow::uninstallKeyboardHook() {
    if (keyboard_hook_) {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }
    if (hook_owner_ == this) hook_owner_ = nullptr;
}

LRESULT CALLBACK RemoteViewWindow::KeyboardHook(int code, WPARAM message, LPARAM data) {
    auto* owner = hook_owner_;
    if (code >= 0 && owner && owner->controlling_ && owner->shown()) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if ((key->flags & LLKHF_INJECTED) == 0) {
            const bool foreground = GetForegroundWindow() == fl_win32_xid(owner);
            if (foreground && (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)) {
                const auto vk = static_cast<std::uint16_t>(key->vkCode & 0xffff);
                if (vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return 1;
                const bool key_up = message == WM_KEYUP || message == WM_SYSKEYUP;
                owner->service_.sendRemoteKey(owner->student_ip_, vk, key_up,
                    static_cast<std::uint16_t>(key->scanCode & 0xffff), (key->flags & LLKHF_EXTENDED) != 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(owner ? owner->keyboard_hook_ : nullptr, code, message, data);
}

} // namespace jiyu::gui
