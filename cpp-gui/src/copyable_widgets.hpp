#pragma once

#include "KswordGUI/KswordStyle.h"

#include <string>

namespace jiyu::gui {

class CopyableTextBox : public KTextBox {
public:
    CopyableTextBox(int x, int y, int w, int h, const char* label = nullptr);
    int handle(int event) override;
};

class CopyableTextDisplay : public KTextDisplay {
public:
    CopyableTextDisplay(int x, int y, int w, int h, const char* label = nullptr);
    int handle(int event) override;
    std::string text() const;
};

} // namespace jiyu::gui
