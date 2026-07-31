#pragma once

#include "remote_view.hpp"

#include "Fl_Double_Window.H"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <string>

namespace jiyu {
class TeacherService;
struct RemoteViewState;
}

class KButton;

namespace jiyu::gui {

class CopyableTextDisplay;

class RemoteViewWindow : public Fl_Double_Window {
public:
    RemoteViewWindow(std::string student_ip, TeacherService& service);
    ~RemoteViewWindow() override;

    const std::string& studentIp() const;
    void updateFrame(RemoteFrame frame);
    void refreshState(const RemoteViewState& state);
    void showAndFocus();

private:
    class Canvas;

    static void StartControlThunk(Fl_Widget*, void* data);
    static void StopControlThunk(Fl_Widget*, void* data);
    static void StopViewThunk(Fl_Widget*, void* data);
    static void CloseThunk(Fl_Widget*, void* data);
    static LRESULT CALLBACK KeyboardHook(int code, WPARAM message, LPARAM data);

    void startControl();
    void stopControl();
    void stopView();
    void closeWindow();
    void installKeyboardHook();
    void uninstallKeyboardHook();

    std::string student_ip_;
    TeacherService& service_;
    Canvas* canvas_ = nullptr;
    CopyableTextDisplay* status_ = nullptr;
    KButton* control_button_ = nullptr;
    HHOOK keyboard_hook_ = nullptr;
    bool controlling_ = false;

    static RemoteViewWindow* hook_owner_;
};

} // namespace jiyu::gui
