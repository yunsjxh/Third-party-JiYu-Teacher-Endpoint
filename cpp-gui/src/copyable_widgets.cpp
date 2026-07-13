#include "copyable_widgets.hpp"

#include "Fl.H"
#include "Fl_Input_.H"
#include "Fl_Text_Buffer.H"

#include <cstring>
#include <cstdlib>
#include <string>

namespace jiyu::gui {
namespace {

void copyToClipboard(const char* text) {
    if (!text) {
        text = "";
    }
    Fl::copy(text, static_cast<int>(std::strlen(text)), 1);
}

} // namespace

CopyableTextBox::CopyableTextBox(int x, int y, int w, int h, const char* label)
    : KTextBox(x, y, w, h, label) {
}

int CopyableTextBox::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
        const int picked = KShowPopupMenu(Fl::event_x_root(), Fl::event_y_root(), { "复制", "复制全部" });
        if (picked == 0) {
            if (insert_position() != mark()) {
                copy(1);
            } else {
                copyToClipboard(value());
            }
        } else if (picked == 1) {
            copyToClipboard(value());
        }
        return 1;
    }
    return KTextBox::handle(event);
}

CopyableTextDisplay::CopyableTextDisplay(int x, int y, int w, int h, const char* label)
    : KTextDisplay(x, y, w, h, label) {
}

int CopyableTextDisplay::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
        const int picked = KShowPopupMenu(Fl::event_x_root(), Fl::event_y_root(), { "复制", "复制全部" });
        Fl_Text_Buffer* b = buffer();
        if (b && picked == 0) {
            char* selected = b->selection_text();
            if (selected && selected[0]) {
                copyToClipboard(selected);
                std::free(selected);
            } else {
                if (selected) {
                    std::free(selected);
                }
                char* all = b->text();
                copyToClipboard(all);
                if (all) {
                    std::free(all);
                }
            }
        } else if (b && picked == 1) {
            char* all = b->text();
            copyToClipboard(all);
            if (all) {
                std::free(all);
            }
        }
        return 1;
    }
    return KTextDisplay::handle(event);
}

std::string CopyableTextDisplay::text() const {
    Fl_Text_Buffer* b = buffer();
    if (!b) {
        return {};
    }
    char* raw = b->text();
    std::string out = raw ? raw : "";
    if (raw) {
        std::free(raw);
    }
    return out;
}

} // namespace jiyu::gui
