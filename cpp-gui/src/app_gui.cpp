#include "app_gui.hpp"

#include "copyable_widgets.hpp"
#include "teacher_service.hpp"

#include "KswordGUI/KswordStyle.h"
#include "KswordWinAPICore/ksword.h"
#include "Fl.H"
#include "Fl_Double_Window.H"
#include "Fl_Text_Buffer.H"
#include "fl_draw.H"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace jiyu::gui {
namespace {

constexpr int kWinW = 1220;
constexpr int kWinH = 760;

std::string formatTime(std::chrono::system_clock::time_point tp) {
    if (tp.time_since_epoch().count() == 0) {
        return "-";
    }
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

std::string formatDateTime(std::chrono::system_clock::time_point tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::filesystem::path desktopPath() {
    PWSTR known_desktop = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &known_desktop)) && known_desktop) {
        std::filesystem::path result(known_desktop);
        CoTaskMemFree(known_desktop);
        return result;
    }
    if (known_desktop) {
        CoTaskMemFree(known_desktop);
    }

    wchar_t profile[MAX_PATH]{};
    constexpr DWORD profile_capacity = static_cast<DWORD>(sizeof(profile) / sizeof(profile[0]));
    const DWORD profile_len = GetEnvironmentVariableW(L"USERPROFILE", profile, profile_capacity);
    if (profile_len > 0 && profile_len < profile_capacity) {
        return std::filesystem::path(profile) / L"Desktop";
    }
    return std::filesystem::current_path();
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string pathToUtf8(const std::filesystem::path& path) {
    return wideToUtf8(path.wstring());
}

std::string trim(std::string text) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [&](char ch) { return !is_space(static_cast<unsigned char>(ch)); }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [&](char ch) { return !is_space(static_cast<unsigned char>(ch)); }).base(), text.end());
    return text;
}

std::optional<std::uint32_t> parseBlackTextColor(const std::string& raw) {
    std::string text = trim(raw);
    if (text.empty()) {
        return 0x0000FFFF;
    }

    std::string lower;
    lower.reserve(text.size());
    for (unsigned char ch : text) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }

    if (text == "黄色" || lower == "yellow") return 0x0000FFFF;
    if (text == "白色" || lower == "white") return 0x00FFFFFF;
    if (text == "红色" || lower == "red") return 0x000000FF;
    if (text == "绿色" || lower == "green") return 0x0000FF00;
    if (text == "蓝色" || lower == "blue") return 0x00FF0000;

    try {
        if (lower.rfind("0x", 0) == 0) {
            const unsigned long value = std::stoul(lower.substr(2), nullptr, 16);
            if (value <= 0x00FFFFFFUL) {
                return static_cast<std::uint32_t>(value);
            }
            return std::nullopt;
        }

        if (!text.empty() && text[0] == '#') {
            text.erase(text.begin());
        }
        if (text.size() == 6 && std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; })) {
            const unsigned long rgb = std::stoul(text, nullptr, 16);
            const std::uint32_t r = static_cast<std::uint32_t>((rgb >> 16) & 0xff);
            const std::uint32_t g = static_cast<std::uint32_t>((rgb >> 8) & 0xff);
            const std::uint32_t b = static_cast<std::uint32_t>(rgb & 0xff);
            return (b << 16) | (g << 8) | r;
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

class CleanLabel : public Fl_Box {
public:
    CleanLabel(int x, int y, int w, int h, const char* label = nullptr, bool muted = false)
        : Fl_Box(x, y, w, h, label), muted_(muted) {
        box(FL_FLAT_BOX);
        align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CENTER);
        labelsize(14);
        refreshPalette();
    }

    void setMuted(bool muted) {
        muted_ = muted;
        refreshPalette();
        redraw();
    }

    void draw() override {
        refreshPalette();
        Fl_Box::draw();
    }

private:
    void refreshPalette() {
        const auto& theme = KThemeManager::instance().theme();
        color(theme.panelBg);
        labelcolor(muted_ ? theme.mutedText : theme.text);
    }

    bool muted_ = false;
};

class CleanTable : public KTable {
public:
    using KTable::KTable;

    void draw_cell(TableContext context, int row, int col, int x, int y, int w, int h) override {
        if (context == CONTEXT_TABLE) {
            const auto& theme = KThemeManager::instance().theme();
            fl_push_clip(x, y, w, h);
            fl_color(theme.panelBg);
            fl_rectf(x, y, w, h);
            fl_pop_clip();
            return;
        }
        KTable::draw_cell(context, row, col, x, y, w, h);
    }
};

CleanLabel* createLabel(int x, int y, int w, int h, const char* text, bool muted = false) {
    return new CleanLabel(x, y, w, h, text, muted);
}

class TeacherWindow : public Fl_Double_Window {
public:
    TeacherWindow()
        : Fl_Double_Window(kWinW, kWinH, "Third-party JiYu Teacher Endpoint - C++ GUI") {
        buildUi();
        refreshStudents();
        appendLog("INFO", "界面已就绪；仅限授权研究与教学演示环境使用。", {});
        Fl::add_timeout(0.25, &TeacherWindow::TimerThunk, this);
    }

    ~TeacherWindow() override {
        Fl::remove_timeout(&TeacherWindow::TimerThunk, this);
        service_.stop();
    }

private:
    static void TimerThunk(void* data) {
        auto* self = static_cast<TeacherWindow*>(data);
        if (!self) {
            return;
        }
        self->pollService();
        Fl::repeat_timeout(0.5, &TeacherWindow::TimerThunk, data);
    }

    static void StartThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onStartStop(); }
    static void ThemeThunk(Fl_Widget*, void*) { KThemeManager::instance().ToggleMode(); }
    static void PreviewThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onPreviewSelected(); }
    static void PreviewAllThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onPreviewAll(); }
    static void ChatThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onSendChat(); }
    static void BlackThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onBlack(false); }
    static void BlackPermThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onBlack(true); }
    static void UnlockThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onUnlockSelected(); }
    static void UnlockAllThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onUnlockAll(); }
    static void DebugThunk(Fl_Widget*, void* data) { static_cast<TeacherWindow*>(data)->onDebugToggle(); }
    static void TableThunk(Fl_Widget* widget, void* data) { static_cast<TeacherWindow*>(data)->onTableEvent(static_cast<KTable*>(widget)); }

    void buildUi() {
        InitFlatThemeGlobal();
        SetWindowStyle(this);
        resizable(this);
        begin();

        auto* header = KCreatePanel(16, 14, 1188, 84, nullptr);
        header->begin();

        auto* title = createLabel(40, 28, 364, 28, "Third-party JiYu Teacher Endpoint");
        title->labelfont(FL_HELVETICA_BOLD);
        title->labelsize(18);
        auto* subtitle = createLabel(40, 58, 520, 20, "Native C++ / Boost.Asio / KswordFrame3.0 · 授权网络测试", true);

        status_badge_ = KCreateBadge(582, 38, 84, 28, "未启动");
        status_badge_->setAccentColor(KThemeManager::instance().theme().danger);

        student_count_badge_ = KCreateBadge(680, 38, 96, 28, "学生 0");
        student_count_badge_->setAccentColor(KThemeManager::instance().theme().primary);

        status_text_ = createLabel(792, 37, 130, 30, "本机 IP：-", true);
        status_text_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        start_button_ = KCreateButton(934, 36, 78, 34, "启动", KBUTTON_HEAVY);
        start_button_->callback(StartThunk, this);
        auto* theme = KCreateButton(1022, 36, 66, 34, "深/浅", KBUTTON_LIGHT);
        theme->callback(ThemeThunk, this);
        debug_button_ = KCreateButton(1098, 36, 74, 34, "DEBUG", KBUTTON_LIGHT);
        debug_button_->callback(DebugThunk, this);
        header->end();

        auto* students_card = KCreateCard(16, 112, 640, 310, nullptr);
        students_card->setTitle("学生列表");
        students_card->setSubtitle("自动发现 LOGI / MESS / LANT；右键表格复制单元格或整行");
        students_card->begin();

        students_table_ = new CleanTable(36, 164, 600, 236, nullptr);
        students_table_->set_size(0, 5);
        students_table_->set_col_header_label(0, "IP");
        students_table_->set_col_header_label(1, "登录");
        students_table_->set_col_header_label(2, "最后包");
        students_table_->set_col_header_label(3, "活跃");
        students_table_->set_col_header_label(4, "预览");
        students_table_->col_width(0, 145);
        students_table_->col_width(1, 65);
        students_table_->col_width(2, 105);
        students_table_->col_width(3, 100);
        students_table_->col_width(4, 160);
        students_table_->set_builtin_copy_context_menu_enabled(true);
        students_table_->callback(TableThunk, this);
        students_card->end();

        auto* action_card = KCreateCard(672, 112, 532, 310, nullptr);
        action_card->setTitle("学生操作");
        action_card->setSubtitle("先从左侧表格选择学生；批量命令只作用于已登录学生");
        action_card->begin();
        selected_text_ = createLabel(692, 164, 480, 26, "选中：-", true);

        auto* preview = KCreateButton(692, 202, 110, 34, "请求预览", KBUTTON_HEAVY);
        preview->callback(PreviewThunk, this);
        auto* preview_all = KCreateButton(812, 202, 110, 34, "请求全部", KBUTTON_LIGHT);
        preview_all->callback(PreviewAllThunk, this);
        auto* unlock = KCreateButton(932, 202, 86, 34, "解锁", KBUTTON_LIGHT);
        unlock->callback(UnlockThunk, this);
        auto* unlock_all = KCreateButton(1028, 202, 110, 34, "全员解锁", KBUTTON_LIGHT);
        unlock_all->callback(UnlockAllThunk, this);

        auto* chat_label = createLabel(692, 254, 72, 22, "聊天内容", true);
        (void)chat_label;
        chat_input_ = new CopyableTextBox(772, 246, 286, 34, nullptr);
        chat_input_->value("请认真听课");
        auto* send_chat = KCreateButton(1070, 246, 72, 34, "发送", KBUTTON_HEAVY);
        send_chat->callback(ChatThunk, this);

        auto* black_text_label = createLabel(692, 296, 72, 22, "黑屏内容", true);
        (void)black_text_label;
        black_text_input_ = new CopyableTextBox(772, 288, 286, 34, nullptr);
        black_text_input_->value("请认真听课");
        auto* color_label = createLabel(1070, 296, 38, 22, "颜色", true);
        (void)color_label;
        black_color_input_ = new CopyableTextBox(1110, 288, 64, 34, nullptr);
        black_color_input_->value("#FFFF00");

        lock_check_ = KCreateCheckBox(692, 340, 104, 28, "锁键鼠");
        lock_check_->value(1);
        lock_check_->color(KThemeManager::instance().theme().panelBg);
        lock_check_->selection_color(KThemeManager::instance().theme().primary);
        auto* black = KCreateButton(806, 338, 118, 34, "黑屏10秒", KBUTTON_HEAVY);
        black->callback(BlackThunk, this);
        auto* black_perm = KCreateButton(934, 338, 116, 34, "永久黑屏", KBUTTON_LIGHT);
        black_perm->callback(BlackPermThunk, this);
        auto* clear_text = createLabel(692, 382, 492, 24, "黑屏内容独立于聊天；颜色支持 #RRGGBB、0x00BBGGRR 或 黄色/白色/红色/绿色/蓝色。", true);
        action_card->end();

        auto* preview_card = KCreateCard(16, 438, 536, 270, nullptr);
        preview_card->setTitle("屏幕预览");
        preview_card->setSubtitle("收到 LANT 分片后自动重组并显示修正图");
        preview_card->begin();
        preview_view_ = KCreateImageView(36, 490, 496, 194, "Preview");
        preview_view_->set_fit_mode(KImageFitMode::Contain);
        preview_view_->set_empty_text("暂无预览图");
        preview_card->end();

        auto* log_card = KCreateCard(568, 438, 636, 270, nullptr);
        log_card->setTitle("运行日志");
        log_card->setSubtitle("右键复制；错误会保留在这里，不再直接崩溃");
        log_card->begin();
        log_display_ = new CopyableTextDisplay(588, 490, 596, 194, nullptr);
        log_display_->set_text("");
        log_card->end();

        status_bar_ = KCreateStatusBar(16, 720, 1188, 24, "就绪");
        status_bar_->setRightText("MSBuild x64 · Debug/Release");

        end();
    }

    void onStartStop() {
        try {
            if (service_.isRunning()) {
                service_.stop();
                setServiceUiState(false);
                appendLog("INFO", "服务已停止。", {});
                return;
            }
            start_button_->copy_label("启动中");
            status_badge_->setText("启动中");
            status_badge_->setAccentColor(KThemeManager::instance().theme().warning);
            status_bar_->setText("正在打开 UDP 端口并加入组播...");
            redraw();

            TeacherServiceOptions options;
            options.preview_dir = desktopPath();
            std::string error;
            if (!service_.start(options, &error)) {
                setServiceUiState(false);
                KShowMessage("启动失败", error.c_str());
                appendLog("ERROR", error, {});
                return;
            }
            setServiceUiState(true);
            appendLog("INFO", "服务已启动。若 Windows 防火墙弹窗出现，请按你的授权网络策略手动处理。", {});
        } catch (const std::exception& e) {
            service_.stop();
            setServiceUiState(false);
            const std::string error = std::string("启动/停止服务时发生异常：") + e.what();
            KShowMessage("服务异常", error.c_str());
            appendLog("ERROR", error, {});
        } catch (...) {
            service_.stop();
            setServiceUiState(false);
            const std::string error = "启动/停止服务时发生未知异常。";
            KShowMessage("服务异常", error.c_str());
            appendLog("ERROR", error, {});
        }
    }

    void onDebugToggle() {
        const bool enabled = !service_.debugLogging();
        service_.setDebugLogging(enabled);
        debug_button_->copy_label(enabled ? "INFO" : "DEBUG");
        appendLog("INFO", enabled ? "DEBUG 日志已开启。" : "DEBUG 日志已关闭。", {});
    }

    void onTableEvent(KTable* table) {
        if (!table) return;
        const int row = table->callback_row();
        if (row >= 0 && row < static_cast<int>(students_.size())) {
            selected_ip_ = students_[static_cast<std::size_t>(row)].ip;
            updateSelectedLabel();
            const auto& info = students_[static_cast<std::size_t>(row)];
            if (!info.fixed_preview.empty() && std::filesystem::exists(info.fixed_preview)) {
                const std::string preview_path = pathToUtf8(info.fixed_preview);
                preview_view_->setImagePath(preview_path.c_str());
            }
        }
    }

    std::string selectedIpOrWarn() {
        if (selected_ip_.empty()) {
            KShowMessage("未选择学生", "请先在学生表中选择一个学生。\n如果表为空，请先启动服务并等待学生登录。 ");
            return {};
        }
        return selected_ip_;
    }

    void onPreviewSelected() {
        const auto ip = selectedIpOrWarn();
        if (!ip.empty()) service_.requestPreview(ip);
    }

    void onPreviewAll() {
        service_.requestPreviewAll();
        appendLog("INFO", "已向所有已登录学生请求预览。", {});
    }

    void onSendChat() {
        const auto ip = selectedIpOrWarn();
        if (ip.empty()) return;
        const char* text = chat_input_->value();
        if (!text || !*text) {
            KShowMessage("消息为空", "请输入要发送的聊天内容。 ");
            return;
        }
        service_.sendChat(ip, text);
    }

    void onBlack(bool permanent) {
        const auto ip = selectedIpOrWarn();
        if (ip.empty()) return;
        const bool lock_input = lock_check_->value() != 0;
        const char* text = black_text_input_ ? black_text_input_->value() : "";
        const char* color_text = black_color_input_ ? black_color_input_->value() : "";
        const auto color = parseBlackTextColor(color_text ? color_text : "");
        if (!color) {
            KShowMessage("颜色格式无效", "黑屏文字颜色支持：#RRGGBB、0x00BBGGRR，或 黄色/白色/红色/绿色/蓝色。");
            return;
        }
        service_.sendBlackscreen(ip, lock_input, permanent ? 0 : 10, text ? text : "", *color);
    }

    void onUnlockSelected() {
        const auto ip = selectedIpOrWarn();
        if (!ip.empty()) service_.sendUnlock(ip);
    }

    void onUnlockAll() {
        service_.sendUnlockAll();
        appendLog("INFO", "已向所有已登录学生发送解锁。", {});
    }

    void pollService() {
        for (const auto& event : service_.drainEvents()) {
            appendLog(event.level, event.message, event.student_ip);
            if (!event.fixed_preview_path.empty()) {
                selected_ip_ = event.student_ip;
                const std::string preview_path = pathToUtf8(event.fixed_preview_path);
                preview_view_->setImagePath(preview_path.c_str());
            }
        }
        refreshStudents();
        if (service_.isRunning()) {
            setServiceUiState(true);
        }
    }

    void refreshStudents() {
        students_ = service_.studentsSnapshot();
        students_table_->set_size(static_cast<int>(students_.size()), 5);
        for (int col = 0; col < 5; ++col) {
            students_table_->col_width(col, col == 0 ? 160 : (col == 4 ? 184 : 110));
        }
        for (std::size_t row = 0; row < students_.size(); ++row) {
            const auto& s = students_[row];
            students_table_->set_cell(static_cast<int>(row), 0, s.ip.c_str());
            students_table_->set_cell(static_cast<int>(row), 1, s.logged_in ? "是" : "否");
            students_table_->set_cell(static_cast<int>(row), 2, s.last_magic.c_str());
            const auto active = formatTime(s.last_seen);
            students_table_->set_cell(static_cast<int>(row), 3, active.c_str());
            students_table_->set_cell(static_cast<int>(row), 4, s.preview_status.c_str());
            if (s.ip == selected_ip_ && !s.fixed_preview.empty() && std::filesystem::exists(s.fixed_preview)) {
                const std::string preview_path = pathToUtf8(s.fixed_preview);
                preview_view_->setImagePath(preview_path.c_str());
            }
        }
        students_table_->redraw();
        if (!selected_ip_.empty()) {
            const auto still_exists = std::any_of(students_.begin(), students_.end(), [this](const StudentInfo& s) { return s.ip == selected_ip_; });
            if (!still_exists) {
                selected_ip_.clear();
            }
        }
        updateSelectedLabel();
        if (student_count_badge_) {
            std::string count = "学生 " + std::to_string(students_.size());
            student_count_badge_->setText(count.c_str());
        }
    }

    void updateSelectedLabel() {
        std::string text = "选中：" + (selected_ip_.empty() ? std::string("-") : selected_ip_);
        selected_text_->copy_label(text.c_str());
        selected_text_->redraw();
    }

    void setServiceUiState(bool running) {
        if (running) {
            start_button_->copy_label("停止");
            status_badge_->setText("运行中");
            status_badge_->setAccentColor(KThemeManager::instance().theme().success);
            std::string status = "本机 IP：" + service_.localIp();
            status_text_->copy_label(status.c_str());
            status_bar_->setText("服务运行中：正在监听 4705 / 5512 并广播教师端存在性");
        } else {
            start_button_->copy_label("启动");
            status_badge_->setText("未启动");
            status_badge_->setAccentColor(KThemeManager::instance().theme().danger);
            status_text_->copy_label("本机 IP：-");
            status_bar_->setText("就绪");
        }
        start_button_->redraw();
        status_badge_->redraw();
        status_text_->redraw();
        status_bar_->redraw();
    }

    void appendLog(const std::string& level, const std::string& message, const std::string& student_ip) {
        std::ostringstream line;
        line << formatDateTime(std::chrono::system_clock::now()) << " [" << level << "] ";
        if (!student_ip.empty()) {
            line << '[' << student_ip << "] ";
        }
        line << message << '\n';
        log_text_ += line.str();
        constexpr std::size_t kMaxLog = 120000;
        if (log_text_.size() > kMaxLog) {
            log_text_.erase(0, log_text_.size() - kMaxLog);
            const auto pos = log_text_.find('\n');
            if (pos != std::string::npos) {
                log_text_.erase(0, pos + 1);
            }
        }
        log_display_->set_text(log_text_.c_str());
        if (auto* b = log_display_->buffer()) {
            log_display_->insert_position(b->length());
            log_display_->show_insert_position();
        }
    }

private:
    TeacherService service_;
    CleanLabel* status_text_ = nullptr;
    CleanLabel* selected_text_ = nullptr;
    KButton* start_button_ = nullptr;
    KButton* debug_button_ = nullptr;
    KBadge* status_badge_ = nullptr;
    KBadge* student_count_badge_ = nullptr;
    KStatusBar* status_bar_ = nullptr;
    KTable* students_table_ = nullptr;
    KCheckBox* lock_check_ = nullptr;
    CopyableTextBox* chat_input_ = nullptr;
    CopyableTextBox* black_text_input_ = nullptr;
    CopyableTextBox* black_color_input_ = nullptr;
    CopyableTextDisplay* log_display_ = nullptr;
    KImageView* preview_view_ = nullptr;
    std::vector<StudentInfo> students_;
    std::string selected_ip_;
    std::string log_text_;
};

} // namespace

int runTeacherGui(int argc, char** argv) {
    (void)argc;
    (void)argv;
    KEnsureAppUserModelID();
    auto* window = new TeacherWindow();
    window->show();
    return Fl::run();
}

} // namespace jiyu::gui
