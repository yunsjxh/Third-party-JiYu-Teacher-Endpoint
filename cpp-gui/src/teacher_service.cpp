#include "teacher_service.hpp"

#include "image_fix.hpp"
#include "protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace jiyu {
namespace {

using udp = boost::asio::ip::udp;

struct Ipv4Network {
    std::uint32_t address = 0;
    std::uint32_t network = 0;
    std::uint32_t broadcast = 0;
    std::uint8_t prefix = 0;

    bool contains(std::uint32_t candidate) const {
        const std::uint32_t mask = prefix == 0 ? 0 : (0xffffffffU << (32 - prefix));
        return (candidate & mask) == network;
    }
};

std::optional<std::uint32_t> parseIpv4Host(const std::string& text) {
    IN_ADDR address{};
    if (InetPtonA(AF_INET, text.c_str(), &address) != 1) {
        return std::nullopt;
    }
    return ntohl(address.S_un.S_addr);
}

std::string formatIpv4Host(std::uint32_t value) {
    IN_ADDR address{};
    address.S_un.S_addr = htonl(value);
    char buffer[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &address, buffer, sizeof(buffer)) ? std::string(buffer) : std::string{};
}

bool usableIpv4(std::uint32_t address) {
    const auto first = static_cast<unsigned>((address >> 24) & 0xff);
    const auto second = static_cast<unsigned>((address >> 16) & 0xff);
    if (address == 0 || first == 127 || first >= 224 || (first == 169 && second == 254)) {
        return false;
    }
    return !(first == 198 && (second == 18 || second == 19));
}

bool privateIpv4(std::uint32_t address) {
    const auto first = static_cast<unsigned>((address >> 24) & 0xff);
    const auto second = static_cast<unsigned>((address >> 16) & 0xff);
    return first == 10 || (first == 172 && second >= 16 && second <= 31) || (first == 192 && second == 168);
}

std::optional<Ipv4Network> parseNetwork(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto slash = text.find('/');
    const std::string address_text = text.substr(0, slash);
    const auto address = parseIpv4Host(address_text);
    if (!address) {
        return std::nullopt;
    }
    unsigned long prefix = 32;
    try {
        if (slash != std::string::npos) {
            std::size_t parsed = 0;
            prefix = std::stoul(text.substr(slash + 1), &parsed, 10);
            if (parsed != text.size() - slash - 1 || prefix > 32) {
                return std::nullopt;
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    const std::uint32_t mask = prefix == 0 ? 0 : (0xffffffffU << (32 - prefix));
    Ipv4Network result;
    result.address = *address;
    result.prefix = static_cast<std::uint8_t>(prefix);
    result.network = *address & mask;
    result.broadcast = result.network | ~mask;
    return result;
}

std::string getEnvironmentText(const char* name) {
    const DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::string value(size, '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), size);
    if (written == 0 || written >= size) {
        return {};
    }
    value.resize(written);
    return value;
}

std::string nowPrefix() {
    return {};
}

std::string endpointAddress(const udp::endpoint& endpoint) {
    try {
        return endpoint.address().to_string();
    } catch (...) {
        return {};
    }
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

std::string ipForFilename(std::string ip) {
    std::replace(ip.begin(), ip.end(), '.', '_');
    return ip;
}

std::string joinWords(const std::vector<std::string>& values, const char* sep) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) {
            oss << sep;
        }
        oss << values[i];
    }
    return oss.str();
}

std::string fixedUtf16LeField(const std::uint8_t* payload, std::size_t payload_len, std::size_t offset, std::size_t max_chars) {
    if (!payload || offset >= payload_len || max_chars == 0) {
        return {};
    }
    std::size_t bytes = std::min(max_chars * 2, payload_len - offset);
    if (bytes % 2 != 0) {
        --bytes;
    }
    std::size_t used = 0;
    while (used + 1 < bytes) {
        if (payload[offset + used] == 0 && payload[offset + used + 1] == 0) {
            break;
        }
        used += 2;
    }
    return protocol::utf16LeToUtf8(payload + offset, used);
}

std::string formatMac(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 6) {
        return {};
    }
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < 6; ++i) {
        if (i) {
            oss << '-';
        }
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

StudentSystemInfo parseStudentSystemInfoPayload(const std::uint8_t* payload, std::size_t payload_len) {
    StudentSystemInfo info;
    if (!payload || payload_len < 0x5A) {
        return info;
    }
    info.valid = true;
    info.computer_name = fixedUtf16LeField(payload, payload_len, 0x10, 32);
    info.student_id = protocol::readLe32(payload + 0x50);
    info.mac = formatMac(payload + 0x54, payload_len - 0x54);
    info.login_user = fixedUtf16LeField(payload, payload_len, 0x5A, 32);
    info.os_name = fixedUtf16LeField(payload, payload_len, 0x9A, 32);
    info.os_version = fixedUtf16LeField(payload, payload_len, 0xDA, 64);
    info.cpu_vendor = fixedUtf16LeField(payload, payload_len, 0x21A, 32);
    info.cpu_model = fixedUtf16LeField(payload, payload_len, 0x25A, 64);
    info.memory = fixedUtf16LeField(payload, payload_len, 0x2DA, 16);
    return info;
}

std::vector<StudentListEntry> parseIdNamePairs(const std::uint8_t* data, std::size_t size) {
    std::vector<StudentListEntry> entries;
    if (!data) {
        return entries;
    }
    std::size_t pos = 0;
    while (pos + 6 <= size) {
        StudentListEntry entry;
        entry.id = protocol::readLe32(data + pos);
        std::size_t p = pos + 4;
        while (p + 1 < size && !(data[p] == 0 && data[p + 1] == 0)) {
            p += 2;
        }
        if (p > pos + 4) {
            entry.name = protocol::utf16LeToUtf8(data + pos + 4, p - (pos + 4));
        }
        entries.push_back(std::move(entry));
        if (p + 1 >= size) {
            break;
        }
        pos = p + 2;
    }
    return entries;
}

std::string sampleEntryNames(const std::vector<StudentListEntry>& entries, std::size_t max_count) {
    std::vector<std::string> names;
    names.reserve(std::min(max_count, entries.size()));
    for (const auto& entry : entries) {
        if (!entry.name.empty()) {
            names.push_back(entry.name);
            if (names.size() >= max_count) {
                break;
            }
        }
    }
    return joinWords(names, "，");
}

void appendUniqueEntries(std::vector<StudentListEntry>& target, const std::vector<StudentListEntry>& entries) {
    for (const auto& entry : entries) {
        const auto exists = std::any_of(target.begin(), target.end(), [&](const StudentListEntry& old) {
            return old.id == entry.id && old.name == entry.name;
        });
        if (!exists) {
            target.push_back(entry);
        }
    }
}

} // namespace

TeacherService::TeacherService() = default;

TeacherService::~TeacherService() {
    stop();
}

bool TeacherService::start(const TeacherServiceOptions& options, std::string* error_message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return true;
        }
        options_ = options;
    }
    if (options_.teacher_name.empty()) {
        options_.teacher_name = "1";
    }
    if (options_.local_ip.empty()) {
        options_.local_ip = getEnvironmentText("TEACHER_IP");
    }
    if (options_.peer_ip.empty()) {
        options_.peer_ip = getEnvironmentText("TEACHER_PEER_IP");
    }
    if (options_.network_cidr.empty()) {
        options_.network_cidr = getEnvironmentText("TEACHER_NETWORK");
    }
    if (options_.channel < 1 || options_.channel > 32 || options_.tcp_mode > 1 ||
        options_.tcp_port == 0 || options_.control_mode > 2 || options_.control_mode == 1 ||
        options_.view_failure_cooldown_seconds < 5) {
        if (error_message) {
            *error_message = "连接设置无效：频道应为 1..32，TCP 模式应为 0/1，远控模式应为 0/2。";
        }
        return false;
    }
    try {
        const auto session = protocol::sessionEndpoint(options_.channel);
        session_multicast_ = session.multicast;
        session_port_ = session.port;
    } catch (const std::exception& e) {
        if (error_message) {
            *error_message = e.what();
        }
        return false;
    }
    if (options_.view_local_port == 0) options_.view_local_port = session_port_;
    if (options_.view_peer_port == 0) options_.view_peer_port = session_port_;
    if (options_.preview_dir.empty()) {
        options_.preview_dir = defaultPreviewDir();
    }
    try {
        std::filesystem::create_directories(options_.preview_dir);
    } catch (const std::exception& e) {
        if (error_message) {
            *error_message = std::string("创建预览目录失败：") + e.what();
        }
        return false;
    }

    try {
        local_ip_ = detectLocalIp();
    } catch (const std::exception& e) {
        if (error_message) {
            *error_message = std::string("获取本机 IP 失败：") + e.what();
        }
        return false;
    }

    main_announce_targets_.clear();
    session_announce_targets_.clear();
    const auto add_target = [&](std::vector<udp::endpoint>& targets, const std::string& host, unsigned short port) {
        if (host.empty() || host == local_ip_) return;
        boost::system::error_code ec;
        const auto address = boost::asio::ip::make_address_v4(host, ec);
        if (ec) return;
        const udp::endpoint endpoint(address, port);
        if (std::find(targets.begin(), targets.end(), endpoint) == targets.end()) {
            targets.push_back(endpoint);
        }
    };
    add_target(main_announce_targets_, protocol::kMainMulticast, protocol::kMainPort);
    add_target(session_announce_targets_, session_multicast_, session_port_);
    for (const auto& target : { selected_broadcast_ip_, selected_host_candidate_ip_, options_.peer_ip }) {
        add_target(main_announce_targets_, target, protocol::kMainPort);
        add_target(session_announce_targets_, target, session_port_);
    }

    try {
        io_.restart();
        work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(io_));
        if (!openSockets(error_message)) {
            resetIoObjects();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = true;
            oonc_sequence_ = 0;
            command_sequence_ = 0;
            students_.clear();
            previews_ = PreviewReassembler{};
            preview_policy_versions_.clear();
            remote_frames_.clear();
            remote_view_states_.clear();
            remote_view_failures_.clear();
            ServiceEvent start_event;
            start_event.time = std::chrono::system_clock::now();
            start_event.level = "INFO";
            start_event.message = "Teacher " + local_ip_ + " channel=" + std::to_string(options_.channel)
                + " started on 224.50.50.42:4705 + " + session_multicast_ + ":" + std::to_string(session_port_);
            events_.push_back(std::move(start_event));
        }

        doMainReceive();
        doSessionReceive();
        scheduleBroadcast(true);
        scheduleSessionAnno(true);
        scheduleKeepAlive();

        io_thread_ = std::thread([this] { runIo(); });
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        boost::system::error_code ignored;
        if (main_socket_) {
            main_socket_->close(ignored);
        }
        if (session_socket_) {
            session_socket_->close(ignored);
        }
        resetIoObjects();
        if (error_message) {
            *error_message = std::string("启动网络服务失败：") + e.what();
        }
        return false;
    }
    return true;
}

void TeacherService::stop() {
    std::vector<std::string> remote_ips;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            remote_ips.reserve(remote_views_.size());
            for (const auto& [ip, _] : remote_views_) remote_ips.push_back(ip);
        }
    }
    for (const auto& ip : remote_ips) {
        stopRemoteView(ip, true);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !io_thread_.joinable()) {
            return;
        }
        running_ = false;
    }

    boost::asio::post(io_, [this] {
        boost::system::error_code ignored;
        if (main_socket_) {
            main_socket_->close(ignored);
        }
        if (session_socket_) {
            session_socket_->close(ignored);
        }
        if (broadcast_timer_) {
            broadcast_timer_->cancel();
        }
        if (session_timer_) {
            session_timer_->cancel();
        }
        if (keepalive_timer_) {
            keepalive_timer_->cancel();
        }
        for (auto& [_, timer] : blackscreen_timers_) {
            if (timer) {
                timer->cancel();
            }
        }
        blackscreen_timers_.clear();
        for (auto& [_, state] : remote_controls_) {
            if (state && state->socket) state->socket->close(ignored);
        }
        remote_controls_.clear();
        if (work_guard_) {
            work_guard_->reset();
        }
    });
    io_.stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    resetIoObjects();
    pushEvent("INFO", "Teacher service stopped");
}

bool TeacherService::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::string TeacherService::localIp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_ip_;
}

std::vector<StudentInfo> TeacherService::studentsSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StudentInfo> out;
    out.reserve(students_.size());
    for (const auto& [_, info] : students_) {
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const StudentInfo& a, const StudentInfo& b) { return a.ip < b.ip; });
    return out;
}

std::vector<ServiceEvent> TeacherService::drainEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceEvent> out;
    out.reserve(events_.size());
    while (!events_.empty()) {
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return out;
}

std::vector<RemoteFrame> TeacherService::drainRemoteFrames() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RemoteFrame> frames;
    frames.reserve(remote_frames_.size());
    for (auto& [_, frame] : remote_frames_) {
        frames.push_back(std::move(frame));
    }
    remote_frames_.clear();
    return frames;
}

std::string TeacherService::sessionDescription() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_multicast_ + ":" + std::to_string(session_port_);
}

void TeacherService::requestPreview(const std::string& student_ip) {
    boost::asio::post(io_, [this, student_ip] {
        const std::uint32_t current = preview_policy_versions_.contains(student_ip) ? preview_policy_versions_[student_ip] : 3U;
        const std::uint32_t stop_version = current + 1;
        const std::uint32_t start_version = current + 2;
        preview_policy_versions_[student_ip] = start_version;
        sendMainTo(student_ip, protocol::buildLpnt(stop_version, false));
        auto timer = std::make_shared<boost::asio::steady_timer>(io_);
        timer->expires_after(std::chrono::milliseconds(50));
        timer->async_wait([this, student_ip, start_version, timer](boost::system::error_code ec) {
            if (!ec && isRunning()) {
                sendMainTo(student_ip, protocol::buildLpnt(start_version, true));
                pushEvent("INFO", "[Preview] LPNT restart -> " + student_ip + ", version=" + std::to_string(start_version), student_ip);
            }
        });
    });
}

void TeacherService::requestPreviewAll() {
    for (const auto& ip : studentIpsLocked()) {
        requestPreview(ip);
    }
}

void TeacherService::requestInfo(const std::string& student_ip, std::uint32_t report_type) {
    if (report_type > 2) {
        report_type = 0;
    }
    boost::asio::post(io_, [this, student_ip, report_type] {
        sendSessionTo(student_ip, protocol::buildInfoRequestMessage(student_ip, report_type));
        std::ostringstream oss;
        oss << "[Info] 请求学生信息 rtype=" << report_type << " -> " << student_ip;
        pushEvent("INFO", oss.str(), student_ip);
    });
}

void TeacherService::requestInfoAll(std::uint32_t report_type) {
    for (const auto& ip : studentIpsLocked()) {
        requestInfo(ip, report_type);
    }
}

void TeacherService::sendChat(const std::string& student_ip, const std::string& text) {
    boost::asio::post(io_, [this, student_ip, text] {
        sendSessionTo(student_ip, protocol::buildChatMessage(student_ip, text));
        pushEvent("INFO", "[命令] 聊天消息 -> " + student_ip + ": " + text, student_ip);
    });
}

void TeacherService::sendBlackscreen(const std::string& student_ip, bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color) {
    boost::asio::post(io_, [this, student_ip, lock_input, timeout_seconds, text, text_color] {
        sendSessionMulticast(protocol::buildBlackscreenMessage(student_ip, lock_input, timeout_seconds, text, text_color));
        if (lock_input) {
            sendMainTo(student_ip, protocol::buildComdLock(timeout_seconds, command_sequence_++));
        }
        cancelAutoUnlock(student_ip);
        if (timeout_seconds > 0) {
            scheduleAutoUnlock(student_ip, timeout_seconds);
        }
        std::ostringstream oss;
        oss << "[命令] 黑屏安静 -> " << student_ip << ", lock=" << (lock_input ? 1 : 0)
            << ", timeout=" << timeout_seconds << ", text=" << text
            << ", color=0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << text_color;
        pushEvent("INFO", oss.str(), student_ip);
    });
}

void TeacherService::sendUnlock(const std::string& student_ip) {
    boost::asio::post(io_, [this, student_ip] {
        cancelAutoUnlock(student_ip);
        doUnlockOnIo(student_ip);
        pushEvent("INFO", "[命令] 解锁 -> " + student_ip, student_ip);
    });
}

void TeacherService::sendBlackscreenAll(bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color) {
    for (const auto& ip : studentIpsLocked()) {
        sendBlackscreen(ip, lock_input, timeout_seconds, text, text_color);
    }
}

void TeacherService::sendUnlockAll() {
    for (const auto& ip : studentIpsLocked()) {
        sendUnlock(ip);
    }
}

void TeacherService::sendShutdown(const std::string& student_ip, bool reboot, std::uint32_t delay_seconds, bool force, const std::string& text) {
    boost::asio::post(io_, [this, student_ip, reboot, delay_seconds, force, text] {
        sendMainTo(student_ip, protocol::buildShutdownCommand(reboot, delay_seconds, force, text, command_sequence_++));
        std::ostringstream oss;
        oss << "[命令] " << (reboot ? "重启" : "关机") << " -> " << student_ip
            << ", mode=" << (force ? "强制" : "倒计时")
            << ", delay=" << delay_seconds
            << ", text=" << text;
        pushEvent("INFO", oss.str(), student_ip);
    });
}

void TeacherService::sendKillProcess(const std::string& student_ip, std::uint32_t pid, bool force) {
    boost::asio::post(io_, [this, student_ip, pid, force] {
        sendSessionTo(student_ip, protocol::buildKillMessage(student_ip, pid, 0, force));
        std::ostringstream oss;
        oss << "[命令] 结束进程 -> " << student_ip << ", pid=" << pid << ", force=" << (force ? 1 : 0);
        pushEvent("INFO", oss.str(), student_ip);
    });
}

void TeacherService::sendCloseApplication(const std::string& student_ip, std::uint32_t hwnd, bool force) {
    boost::asio::post(io_, [this, student_ip, hwnd, force] {
        sendSessionTo(student_ip, protocol::buildKillMessage(student_ip, 0, hwnd, force));
        std::ostringstream oss;
        oss << "[命令] 结束应用程序 -> " << student_ip << ", hwnd=0x"
            << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << hwnd
            << ", force=" << (force ? 1 : 0);
        pushEvent("INFO", oss.str(), student_ip);
    });
}

void TeacherService::sendOpenUrl(const std::string& student_ip, const std::string& url) {
    boost::asio::post(io_, [this, student_ip, url] {
        sendMainTo(student_ip, protocol::buildOpenUrlCommand(url, command_sequence_++));
        pushEvent("INFO", "[命令] 打开网页/文件 -> " + student_ip + ": " + url, student_ip);
    });
}

void TeacherService::sendRunProgram(const std::string& student_ip, const std::string& path, const std::string& args, std::uint32_t show_mode, bool fallback) {
    boost::asio::post(io_, [this, student_ip, path, args, show_mode, fallback] {
        sendMainTo(student_ip, protocol::buildRunProgramCommand(path, args, show_mode, fallback, command_sequence_++));
        std::ostringstream oss;
        oss << "[命令] 远程运行 -> " << student_ip << ": \"" << path << "\" " << args
            << ", show=" << show_mode << ", fallback=" << (fallback ? 1 : 0);
        pushEvent("INFO", oss.str(), student_ip);
    });
}

bool TeacherService::startRemoteView(const std::string& student_ip, std::uint16_t tcp_port, std::string* error_message) {
    if (!isRunning() || !hasStudentLocked(student_ip)) {
        if (error_message) *error_message = "学生未登录或服务尚未启动。";
        return false;
    }
    if (tcp_port == 0) tcp_port = options_.tcp_port;

    std::shared_ptr<RemoteViewSession> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto failure = remote_view_failures_.find(student_ip);
        if (failure != remote_view_failures_.end()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - failure->second).count();
            if (elapsed < options_.view_failure_cooldown_seconds) {
                if (error_message) {
                    *error_message = "观看连接仍在失败冷却中，请等待 "
                        + std::to_string(options_.view_failure_cooldown_seconds - elapsed) + " 秒。";
                }
                return false;
            }
            remote_view_failures_.erase(failure);
        }
        const auto existing = remote_views_.find(student_ip);
        if (existing != remote_views_.end()) {
            if (!existing->second->finished()) {
                if (error_message) *error_message = "该学生已有连续观看会话。";
                return false;
            }
            previous = existing->second;
            remote_views_.erase(existing);
        }
    }
    if (previous) previous->join();

    RemoteViewOptions view_options;
    view_options.local_ip = localIp();
    view_options.student_ip = student_ip;
    view_options.tcp_port = tcp_port;
    view_options.start_delay_ms = options_.view_start_delay_ms;
    view_options.output_dir = options_.preview_dir / "teacher_remote_view";
    view_options.dump_h264 = options_.dump_remote_h264;

    RemoteViewCallbacks callbacks;
    callbacks.request_feature_start = [this, student_ip](std::string& error) {
        return requestRemoteViewFeatureSync(student_ip, true, error);
    };
    callbacks.connected = [this, student_ip] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = remote_view_states_[student_ip];
            state.active = true;
            state.connected = true;
            remote_view_failures_.erase(student_ip);
        }
        pushEvent("INFO", "[RemoteView] TCP/UMSP channel 10 已连接", student_ip);
    };
    callbacks.frame = [this, student_ip](RemoteFrame frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        remote_frames_[student_ip] = std::move(frame);
        auto& state = remote_view_states_[student_ip];
        ++state.frame_count;
    };
    callbacks.cursor = [this, student_ip](std::uint16_t x, std::uint16_t y) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = remote_view_states_[student_ip];
        state.has_cursor = true;
        state.cursor_x = x;
        state.cursor_y = y;
    };
    callbacks.log = [this, student_ip](const std::string& level, const std::string& message) {
        pushEvent(level, "[RemoteView] " + message, student_ip);
    };
    callbacks.closed = [this, student_ip](const std::string& failure) {
        bool unexpected = !failure.empty();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& state = remote_view_states_[student_ip];
            state.active = false;
            state.connected = false;
            state.controlling = false;
            if (unexpected) remote_view_failures_[student_ip] = std::chrono::steady_clock::now();
        }
        stopRemoteControl(student_ip, unexpected);
        if (unexpected) {
            std::string ignored;
            requestRemoteViewFeatureSync(student_ip, false, ignored);
        }
        pushEvent(unexpected ? "ERROR" : "INFO", unexpected
            ? "[RemoteView] 会话失败并停止：" + failure
            : "[RemoteView] 会话已停止", student_ip);
    };

    auto session = std::make_shared<RemoteViewSession>(std::move(view_options), std::move(callbacks));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remote_views_[student_ip] = session;
        auto& state = remote_view_states_[student_ip];
        state = RemoteViewState{};
        state.active = true;
        state.tcp_port = tcp_port;
    }
    session->start();
    pushEvent("INFO", "[RemoteView] 正在探测并连接 TCP " + student_ip + ":" + std::to_string(tcp_port), student_ip);
    return true;
}

void TeacherService::stopRemoteView(const std::string& student_ip, bool notify) {
    stopRemoteControl(student_ip, notify);
    if (notify && isRunning()) {
        std::string ignored;
        requestRemoteViewFeatureSync(student_ip, false, ignored);
    }
    std::shared_ptr<RemoteViewSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = remote_views_.find(student_ip);
        if (it != remote_views_.end()) {
            session = it->second;
            remote_views_.erase(it);
        }
        remote_frames_.erase(student_ip);
        auto& state = remote_view_states_[student_ip];
        state.active = false;
        state.connected = false;
        state.controlling = false;
    }
    if (session) {
        session->stop();
        session->join();
    }
}

bool TeacherService::startRemoteControl(const std::string& student_ip, std::uint16_t port, std::string* error_message) {
    if (!options_.remote_control_enabled) {
        if (error_message) *error_message = "远程控制已在连接设置中禁用。";
        return false;
    }
    std::shared_ptr<RemoteViewSession> view;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = remote_views_.find(student_ip);
        if (it != remote_views_.end()) view = it->second;
    }
    if (!view || !view->connected()) {
        if (error_message) *error_message = "屏幕 TCP 通道尚未连接，请先启动连续观看。";
        return false;
    }
    if (port == 0) port = options_.control_port;
    if (port == 0) {
        std::random_device random;
        std::uniform_int_distribution<unsigned> ports(49152, 65535);
        port = static_cast<std::uint16_t>(ports(random));
    }

    auto state = std::make_shared<RemoteControlState>();
    state->port = port;
    state->ready_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    state->socket = std::make_shared<udp::socket>(io_);
    boost::system::error_code ec;
    state->socket->open(udp::v4(), ec);
    if (!ec) {
        state->socket->bind(udp::endpoint(boost::asio::ip::make_address_v4(localIp()), 0), ec);
    }
    if (ec) {
        if (error_message) *error_message = "创建远控输入 socket 失败：" + ec.message();
        return false;
    }

    std::shared_ptr<RemoteControlState> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = remote_controls_.find(student_ip);
        if (existing != remote_controls_.end()) old = existing->second;
        remote_controls_[student_ip] = state;
        auto& view_state = remote_view_states_[student_ip];
        view_state.controlling = true;
        view_state.control_port = port;
    }
    if (old && old->socket) old->socket->close(ec);
    const auto payload = protocol::buildRemoteControlPayload(student_ip, options_.control_mode, port);
    const auto packet = protocol::buildCommandTransaction(payload, localIp());
    boost::asio::post(io_, [this, student_ip, packet] { sendMainTo(student_ip, packet); });
    pushEvent("INFO", "[RemoteControl] MCMD mode=" + std::to_string(options_.control_mode)
        + "，输入端口=" + std::to_string(port), student_ip);
    return true;
}

void TeacherService::stopRemoteControl(const std::string& student_ip, bool notify) {
    std::shared_ptr<RemoteControlState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = remote_controls_.find(student_ip);
        if (it != remote_controls_.end()) {
            state = it->second;
            remote_controls_.erase(it);
        }
        auto& view_state = remote_view_states_[student_ip];
        view_state.controlling = false;
        view_state.control_port = 0;
    }
    boost::system::error_code ignored;
    if (state && state->socket) state->socket->close(ignored);
    if (notify && isRunning()) {
        const std::uint16_t port = state ? state->port : options_.control_port;
        const auto payload = protocol::buildRemoteControlPayload(student_ip, 1, port);
        const auto packet = protocol::buildCommandTransaction(payload, localIp());
        boost::asio::post(io_, [this, student_ip, packet] { sendMainTo(student_ip, packet); });
    }
}

bool TeacherService::sendRemoteMouse(const std::string& student_ip, std::uint32_t message, std::uint32_t x, std::uint32_t y, std::int32_t data) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!remote_controls_.contains(student_ip)) return false;
    }
    queueRemoteInput(student_ip, protocol::buildRemoteMousePacket(message, x, y, static_cast<std::uint32_t>(data)));
    return true;
}

bool TeacherService::sendRemoteKey(const std::string& student_ip, std::uint16_t virtual_key, bool key_up, std::uint16_t scan_code, bool extended) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!remote_controls_.contains(student_ip)) return false;
    }
    if (scan_code == 0) scan_code = static_cast<std::uint16_t>(MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC));
    queueRemoteInput(student_ip, protocol::buildRemoteKeyPacket(scan_code, virtual_key, key_up, extended));
    return true;
}

RemoteViewState TeacherService::remoteViewState(const std::string& student_ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = remote_view_states_.find(student_ip);
    if (it == remote_view_states_.end()) return {};
    RemoteViewState state = it->second;
    const auto view = remote_views_.find(student_ip);
    if (view != remote_views_.end()) {
        state.connected = view->second->connected();
        state.active = !view->second->finished();
        state.frame_count = view->second->frameCount();
    }
    return state;
}

void TeacherService::setDebugLogging(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    debug_logging_ = enabled;
}

bool TeacherService::debugLogging() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return debug_logging_;
}

void TeacherService::resetIoObjects() {
    main_socket_.reset();
    session_socket_.reset();
    broadcast_timer_.reset();
    session_timer_.reset();
    keepalive_timer_.reset();
    work_guard_.reset();
    blackscreen_timers_.clear();
    remote_controls_.clear();
    remote_views_.clear();
}

bool TeacherService::openSockets(std::string* error_message) {
    boost::system::error_code ec;
    const auto local_address = boost::asio::ip::make_address_v4(local_ip_, ec);
    if (ec) {
        if (error_message) {
            *error_message = "本机 IP 无效：" + local_ip_;
        }
        return false;
    }

    main_socket_ = std::make_unique<udp::socket>(io_);
    main_socket_->open(udp::v4(), ec);
    if (ec) goto fail;
    main_socket_->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) goto fail;
    main_socket_->set_option(boost::asio::socket_base::broadcast(true), ec);
    if (ec) goto fail;
    main_socket_->bind(udp::endpoint(udp::v4(), protocol::kMainPort), ec);
    if (ec) goto fail;
    main_socket_->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::make_address_v4(protocol::kMainMulticast), local_address), ec);
    if (ec) goto fail;
    main_socket_->set_option(boost::asio::ip::multicast::hops(32), ec);
    if (ec) goto fail;
    main_socket_->set_option(boost::asio::ip::multicast::outbound_interface(local_address), ec);

    session_socket_ = std::make_unique<udp::socket>(io_);
    session_socket_->open(udp::v4(), ec);
    if (ec) goto fail;
    session_socket_->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) goto fail;
    session_socket_->set_option(boost::asio::socket_base::broadcast(true), ec);
    if (ec) goto fail;
    session_socket_->bind(udp::endpoint(udp::v4(), session_port_), ec);
    if (ec) goto fail;
    session_socket_->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::make_address_v4(session_multicast_), local_address), ec);
    if (ec) goto fail;
    session_socket_->set_option(boost::asio::ip::multicast::hops(32), ec);
    if (ec) goto fail;
    session_socket_->set_option(boost::asio::ip::multicast::outbound_interface(local_address), ec);
    return true;

fail:
    if (error_message) {
        *error_message = "打开 UDP socket 失败：" + ec.message();
    }
    return false;
}

std::string TeacherService::detectLocalIp() {
    selected_broadcast_ip_.clear();
    selected_host_candidate_ip_.clear();
    const auto wanted_network = parseNetwork(options_.network_cidr);
    if (!options_.network_cidr.empty() && !wanted_network) {
        throw std::runtime_error("TEACHER_NETWORK/网段不是有效 IPv4 CIDR：" + options_.network_cidr);
    }
    const auto peer = options_.peer_ip.empty() ? std::optional<std::uint32_t>{} : parseIpv4Host(options_.peer_ip);
    if (!options_.peer_ip.empty() && !peer) {
        throw std::runtime_error("定向学生 IP 不是有效 IPv4 地址：" + options_.peer_ip);
    }
    if (!options_.local_ip.empty()) {
        const auto address = parseIpv4Host(options_.local_ip);
        if (!address || !usableIpv4(*address)) {
            throw std::runtime_error("指定的本机 IP 不可用：" + options_.local_ip);
        }
        if (wanted_network && !wanted_network->contains(*address)) {
            throw std::runtime_error("指定的本机 IP 不属于设置的网段");
        }
        if (wanted_network && wanted_network->prefix <= 30 && privateIpv4(*address)) {
            selected_broadcast_ip_ = formatIpv4Host(wanted_network->broadcast);
            selected_host_candidate_ip_ = formatIpv4Host(wanted_network->network + 1);
        }
        return options_.local_ip;
    }

    ULONG buffer_size = 16 * 1024;
    std::vector<std::uint8_t> storage(buffer_size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(buffer_size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &buffer_size);
    }

    struct Candidate {
        int score = 0;
        std::string text;
        Ipv4Network network;
    };
    std::vector<Candidate> candidates;
    if (result == NO_ERROR) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            const bool has_gateway = adapter->FirstGatewayAddress != nullptr;
            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                const std::uint32_t host = ntohl(address->sin_addr.S_un.S_addr);
                if (!usableIpv4(host)) continue;
                Ipv4Network network;
                network.address = host;
                network.prefix = std::min<std::uint8_t>(unicast->OnLinkPrefixLength, 32);
                const std::uint32_t mask = network.prefix == 0 ? 0 : (0xffffffffU << (32 - network.prefix));
                network.network = host & mask;
                network.broadcast = network.network | ~mask;
                if (wanted_network && !wanted_network->contains(host)) continue;
                Candidate candidate;
                candidate.text = formatIpv4Host(host);
                candidate.network = network;
                candidate.score = (has_gateway ? 100 : 0) + (privateIpv4(host) ? 20 : 0);
                if (peer && network.contains(*peer)) candidate.score += 1000;
                candidates.push_back(std::move(candidate));
            }
        }
    }
    if (!candidates.empty()) {
        const auto selected = std::max_element(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) { return left.score < right.score; });
        if (selected->network.prefix <= 30 && privateIpv4(selected->network.address)) {
            selected_broadcast_ip_ = formatIpv4Host(selected->network.broadcast);
            selected_host_candidate_ip_ = formatIpv4Host(selected->network.network + 1);
        }
        return selected->text;
    }
    if (wanted_network) {
        throw std::runtime_error("没有找到属于指定网段的本机 IPv4 地址");
    }

    boost::asio::io_context ctx;
    udp::socket socket(ctx);
    boost::system::error_code ec;
    socket.open(udp::v4(), ec);
    if (ec) {
        throw std::runtime_error(ec.message());
    }
    const std::array<std::pair<std::string, unsigned short>, 3> route_targets{
        std::pair<std::string, unsigned short>{ options_.peer_ip, protocol::kMainPort },
        { protocol::kMainMulticast, protocol::kMainPort },
        { "8.8.8.8", 80 }
    };
    for (const auto& [target, port] : route_targets) {
        if (target.empty()) continue;
        socket.connect(udp::endpoint(boost::asio::ip::make_address(target, ec), port), ec);
        if (!ec) {
            auto endpoint = socket.local_endpoint(ec);
            if (!ec && endpoint.address().is_v4()) {
                const auto value = parseIpv4Host(endpoint.address().to_string());
                if (value && usableIpv4(*value)) {
                    return endpoint.address().to_string();
                }
            }
        }
        ec.clear();
    }

    char hostname[256] = {};
    if (::gethostname(hostname, static_cast<int>(sizeof(hostname))) == 0) {
        boost::asio::ip::tcp::resolver resolver(ctx);
        auto results = resolver.resolve(hostname, "", ec);
        if (!ec) {
            for (const auto& result : results) {
                const auto address = result.endpoint().address();
                if (address.is_v4()) {
                    const auto value = parseIpv4Host(address.to_string());
                    if (value && usableIpv4(*value)) return address.to_string();
                }
            }
        }
    }
    throw std::runtime_error("没有找到可用的非回环 IPv4 地址");
}

std::filesystem::path TeacherService::defaultPreviewDir() {
    return desktopPath();
}

void TeacherService::runIo() {
    try {
        io_.run();
    } catch (const std::exception& e) {
        pushEvent("ERROR", std::string("io_context 异常：") + e.what());
    }
}

void TeacherService::doMainReceive() {
    if (!main_socket_) return;
    main_socket_->async_receive_from(boost::asio::buffer(main_recv_buffer_), main_remote_, [this](boost::system::error_code ec, std::size_t size) {
        if (!ec && size > 0) {
            std::vector<std::uint8_t> packet(main_recv_buffer_.begin(), main_recv_buffer_.begin() + static_cast<std::ptrdiff_t>(size));
            handleMainPacket(packet, main_remote_);
        }
        if (isRunning()) {
            doMainReceive();
        }
    });
}

void TeacherService::doSessionReceive() {
    if (!session_socket_) return;
    session_socket_->async_receive_from(boost::asio::buffer(session_recv_buffer_), session_remote_, [this](boost::system::error_code ec, std::size_t size) {
        if (!ec && size > 0) {
            std::vector<std::uint8_t> packet(session_recv_buffer_.begin(), session_recv_buffer_.begin() + static_cast<std::ptrdiff_t>(size));
            handleSessionPacket(packet, session_remote_);
        }
        if (isRunning()) {
            doSessionReceive();
        }
    });
}

void TeacherService::handleMainPacket(const std::vector<std::uint8_t>& packet, const udp::endpoint& remote) {
    const std::string sip = endpointAddress(remote);
    if (sip.empty() || sip == localIp() || packet.size() < 4) {
        return;
    }
    const auto magic = protocol::readLe32(packet.data());
    if (magic == protocol::kOonc || magic == protocol::kNanc || magic == protocol::kCanc || magic == protocol::kAnno) {
        return;
    }
    const std::string name = protocol::magicName(magic);
    markStudentPacket(sip, name);
    if (magic == protocol::kTrmc || magic == protocol::kTrnt) {
        logDebug("[MainRecv] " + name + " from " + sip, sip);
    } else {
        pushEvent("INFO", "[MainRecv] " + name + " from " + sip, sip);
    }

    switch (magic) {
    case protocol::kKaca:
        sendMainTo(sip, protocol::buildWaca(localIp(), options_.channel));
        break;
    case protocol::kTrmc:
        sendMainTo(sip, protocol::buildLpnt(3, true));
        sendMainTo(sip, protocol::buildDmoc(localIp()));
        break;
    case protocol::kTrnt:
        break;
    case protocol::kDent:
        if (packet.size() >= 40) {
            const auto frame_sequence = protocol::readLe32(packet.data() + 36);
            const auto ack = previews_.acknowledgement(sip, frame_sequence);
            if (!ack.known) {
                sendMainTo(sip, protocol::buildSrnt(frame_sequence, false, std::nullopt));
            } else {
                sendMainTo(sip, protocol::buildSrnt(frame_sequence, ack.complete, ack.missing_parts));
            }
        }
        break;
    case protocol::kLant: {
        auto done = previews_.accept(sip, packet.data(), packet.size());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& info = students_[sip];
            info.ip = sip;
            info.preview_status = previews_.status(sip);
        }
        if (done) {
            handleCompletedPreview(*done);
        }
        break;
    }
    case protocol::kMess:
        handleMess(packet, remote, "MainRecv");
        break;
    default:
        pushEvent("WARN", "[MainRecv] 未知包 " + name + " from " + sip + "\n" + protocol::hexdump(packet.data(), std::min<std::size_t>(packet.size(), 256)), sip);
        break;
    }
}

void TeacherService::handleSessionPacket(const std::vector<std::uint8_t>& packet, const udp::endpoint& remote) {
    const std::string sip = endpointAddress(remote);
    if (sip.empty() || sip == localIp() || packet.size() < 4) {
        return;
    }
    const auto magic = protocol::readLe32(packet.data());
    if (magic == protocol::kLogi) {
        markStudentPacket(sip, "LOGI");
        pushEvent("INFO", "[SessionRecv] LOGI from " + sip, sip);
        const bool already_logged = hasStudentLocked(sip);
        sendSessionTo(sip, protocol::buildLoginMessType1000(sip));
        sendSessionTo(sip, protocol::buildLoginMessType8000(sip, options_.tcp_mode, options_.tcp_port));
        if (!already_logged) {
            sendMainTo(sip, protocol::buildLpntSubtype2());
            sendMainTo(sip, protocol::buildLpntSubtype3());
            sendMainTo(sip, protocol::buildDmoc(localIp()));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& info = students_[sip];
                info.ip = sip;
                info.logged_in = true;
                info.preview_status = "idle";
            }

            auto timer = std::make_shared<boost::asio::steady_timer>(io_);
            timer->expires_after(std::chrono::milliseconds(200));
            timer->async_wait([this, sip, timer](boost::system::error_code ec) {
                if (!ec && isRunning()) {
                    sendSessionTo(sip, protocol::buildInfoRequestMessage(sip, 0));
                    pushEvent("INFO", "[Info] 登录后自动请求学生信息 -> " + sip, sip);
                }
            });
            pushEvent("INFO", "[Login] " + sip + " 登录成功", sip);
        } else {
            logDebug("[Login] " + sip + " 重复 LOGI 心跳", sip);
        }
    } else if (magic == protocol::kMess) {
        markStudentPacket(sip, "MESS");
        handleMess(packet, remote, "SessionRecv");
    } else if (magic != protocol::kOonc && magic != protocol::kNanc && magic != protocol::kCanc && magic != protocol::kAnno) {
        markStudentPacket(sip, protocol::magicName(magic));
        pushEvent("WARN", "[SessionRecv] 未认证/非登录包 " + protocol::magicName(magic) + " from " + sip, sip);
    }
}

void TeacherService::handleMess(const std::vector<std::uint8_t>& packet, const udp::endpoint& remote, const std::string& via) {
    const std::string sip = endpointAddress(remote);
    if (packet.size() < 12) {
        return;
    }
    const auto rcpt_count = protocol::readLe32(packet, 8);
    const std::size_t header_len = 12 + static_cast<std::size_t>(rcpt_count) * 4;
    if (packet.size() < header_len) {
        return;
    }
    const std::uint8_t* payload = packet.data() + header_len;
    const std::size_t payload_len = packet.size() - header_len;
    std::string decoded;
    if (payload_len >= 16) {
        const auto msg_type = protocol::readLe32(payload + 4);
        const auto category = protocol::readLe32(payload + 8);
        const auto subtype = protocol::readLe32(payload + 12);
        if (msg_type == 0x800) {
            decoded = "[聊天] " + protocol::utf16LeToUtf8(payload + 16, payload_len - 16);
        } else if (msg_type == 0 && category == 0x800000) {
            if (subtype == 5) {
                auto parsed = parseStudentSystemInfoPayload(payload, payload_len);
                if (parsed.valid) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto& info = students_[sip];
                        info.ip = sip;
                        info.system_info = parsed;
                        info.last_info_seen = std::chrono::system_clock::now();
                    }
                    decoded = "[学生信息] 计算机=" + parsed.computer_name
                        + " 用户=" + parsed.login_user
                        + " MAC=" + parsed.mac
                        + " OS=" + parsed.os_name + " " + parsed.os_version
                        + " CPU=" + parsed.cpu_vendor + "/" + parsed.cpu_model
                        + " 内存=" + parsed.memory;
                } else {
                    decoded = "[学生信息] type=5 长度不足 payload_len=" + std::to_string(payload_len);
                }
            } else if (subtype == 6 || subtype == 7) {
                if (payload_len >= 20) {
                    const auto chunk_flag = protocol::readLe32(payload + 16);
                    auto entries = parseIdNamePairs(payload + 20, payload_len - 20);
                    std::size_t total = entries.size();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto& info = students_[sip];
                        info.ip = sip;
                        auto& target = subtype == 6 ? info.windows : info.processes;
                        if (chunk_flag == 1) {
                            target.clear();
                        }
                        appendUniqueEntries(target, entries);
                        total = target.size();
                        info.last_info_seen = std::chrono::system_clock::now();
                    }
                    const std::string kind = subtype == 6 ? "窗口" : "进程";
                    decoded = "[" + kind + "列表" + (chunk_flag == 1 ? "首片" : "续片") + "] 本片 "
                        + std::to_string(entries.size()) + " 个，累计 " + std::to_string(total) + " 个";
                    const auto sample = sampleEntryNames(entries, 6);
                    if (!sample.empty()) {
                        decoded += "：" + sample;
                        if (entries.size() > 6) {
                            decoded += "…";
                        }
                    }
                } else {
                    decoded = "[信息上报] type=" + std::to_string(subtype) + " 长度不足 payload_len=" + std::to_string(payload_len);
                }
            } else {
                decoded = "[信息上报] 未知 type=" + std::to_string(subtype);
            }
        } else if (msg_type == 0 && payload_len >= 24) {
            const auto extra = protocol::readLe32(payload + 20);
            if (subtype == 6) {
                decoded = "[窗口标题] " + protocol::utf16LeToUtf8(payload + 24, payload_len - 24);
            } else if (subtype == 7) {
                decoded = "[WiFi可用网络数量] " + std::to_string(extra == 0xffffffff ? -1 : static_cast<int>(extra));
            } else if (subtype == 1) {
                std::ostringstream oss;
                oss << "[IE/浏览器URL信息] extra=0x" << std::hex << std::uppercase << extra;
                decoded = oss.str();
            } else if (subtype == 0) {
                std::ostringstream oss;
                oss << "[窗口标题清空/PID=0x" << std::hex << std::uppercase << extra << "]";
                decoded = oss.str();
            } else if (subtype == 3) {
                decoded = "[系统性能/进程信息]";
            }
        }
    }
    if (decoded.empty()) {
        decoded = "[MESS] payload_len=" + std::to_string(payload_len);
    }
    pushEvent("INFO", "[MESS] " + via + " from " + sip + ": " + decoded, sip);
}

void TeacherService::handleCompletedPreview(const CompletedPreview& preview) {
    const auto raw_path = nextPreviewPath(preview.student_ip, false);
    const auto fixed_path = nextPreviewPath(preview.student_ip, true);
    try {
        std::ofstream out(raw_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(preview.jpeg.data()), static_cast<std::streamsize>(preview.jpeg.size()));
        out.close();
    } catch (const std::exception& e) {
        pushEvent("ERROR", std::string("[Preview] 保存原图失败：") + e.what(), preview.student_ip);
        return;
    }

    std::string fix_error;
    const bool fixed_ok = image::saveFixedPreviewJpeg(raw_path, fixed_path, &fix_error);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& info = students_[preview.student_ip];
        info.ip = preview.student_ip;
        info.preview_status = fixed_ok ? "done" : "raw only";
        info.raw_preview = raw_path;
        if (fixed_ok) {
            info.fixed_preview = fixed_path;
        }
    }
    if (fixed_ok) {
        pushEvent("INFO", "[Preview] saved fixed " + pathToUtf8(fixed_path), preview.student_ip, raw_path, fixed_path);
    } else {
        pushEvent("WARN", "[Preview] raw saved, fixed failed: " + fix_error, preview.student_ip, raw_path, {});
    }
}

void TeacherService::scheduleBroadcast(bool send_oonc) {
    if (!isRunning()) return;
    if (!broadcast_timer_) {
        broadcast_timer_ = std::make_unique<boost::asio::steady_timer>(io_);
    }
    if (send_oonc) {
        sendMainAnnouncement(protocol::buildOonc(localIp(), oonc_sequence_++));
    } else {
        sendMainAnnouncement(protocol::buildNanc(localIp(), options_.channel, options_.teacher_name));
        sendMainAnnouncement(protocol::buildCanc(localIp(), options_.channel, options_.teacher_name));
    }
    broadcast_timer_->expires_after(std::chrono::milliseconds(500));
    broadcast_timer_->async_wait([this, send_oonc](boost::system::error_code ec) {
        if (!ec && isRunning()) {
            scheduleBroadcast(!send_oonc);
        }
    });
}

void TeacherService::scheduleSessionAnno(bool short_packet) {
    if (!isRunning()) return;
    if (!session_timer_) {
        session_timer_ = std::make_unique<boost::asio::steady_timer>(io_);
    }
    if (short_packet) {
        sendSessionAnnouncement(protocol::buildAnnoShort());
        session_timer_->expires_after(std::chrono::milliseconds(300));
    } else {
        sendSessionAnnouncement(protocol::buildAnnoLong(localIp()));
        session_timer_->expires_after(std::chrono::milliseconds(700));
    }
    session_timer_->async_wait([this, short_packet](boost::system::error_code ec) {
        if (!ec && isRunning()) {
            scheduleSessionAnno(!short_packet);
        }
    });
}

void TeacherService::scheduleKeepAlive() {
    if (!isRunning()) return;
    if (!keepalive_timer_) {
        keepalive_timer_ = std::make_unique<boost::asio::steady_timer>(io_);
    }
    const auto ips = studentIpsLocked();
    for (const auto& ip : ips) {
        if (!previews_.hasActivePreview(ip)) {
            sendMainTo(ip, protocol::buildLpnt(3, true));
            sendMainTo(ip, protocol::buildDmoc(localIp()));
            logDebug("[KeepAlive] LPNT subtype3 + DMOC -> " + ip, ip);
        }
    }
    keepalive_timer_->expires_after(std::chrono::milliseconds(600));
    keepalive_timer_->async_wait([this](boost::system::error_code ec) {
        if (!ec && isRunning()) {
            scheduleKeepAlive();
        }
    });
}

void TeacherService::scheduleAutoUnlock(const std::string& student_ip, std::uint32_t timeout_seconds) {
    auto timer = std::make_shared<boost::asio::steady_timer>(io_);
    timer->expires_after(std::chrono::seconds(timeout_seconds));
    blackscreen_timers_[student_ip] = timer;
    timer->async_wait([this, student_ip](boost::system::error_code ec) {
        if (!ec && isRunning()) {
            blackscreen_timers_.erase(student_ip);
            doUnlockOnIo(student_ip);
            pushEvent("INFO", "[AutoUnlock] 超时自动解锁 " + student_ip, student_ip);
        }
    });
}

void TeacherService::cancelAutoUnlock(const std::string& student_ip) {
    auto it = blackscreen_timers_.find(student_ip);
    if (it != blackscreen_timers_.end()) {
        if (it->second) {
            it->second->cancel();
        }
        blackscreen_timers_.erase(it);
    }
}

void TeacherService::sendMainTo(const std::string& ip, const std::vector<std::uint8_t>& data) {
    if (!main_socket_) return;
    boost::system::error_code ec;
    const udp::endpoint endpoint(boost::asio::ip::make_address(ip, ec), protocol::kMainPort);
    if (!ec) {
        main_socket_->send_to(boost::asio::buffer(data), endpoint, 0, ec);
    }
    if (ec) {
        pushEvent("ERROR", "send main -> " + ip + " failed: " + ec.message(), ip);
    }
}

void TeacherService::sendSessionTo(const std::string& ip, const std::vector<std::uint8_t>& data) {
    if (!session_socket_) return;
    boost::system::error_code ec;
    const udp::endpoint endpoint(boost::asio::ip::make_address(ip, ec), session_port_);
    if (!ec) {
        session_socket_->send_to(boost::asio::buffer(data), endpoint, 0, ec);
    }
    if (ec) {
        pushEvent("ERROR", "send session -> " + ip + " failed: " + ec.message(), ip);
    }
}

void TeacherService::sendSessionMulticast(const std::vector<std::uint8_t>& data) {
    if (!session_socket_) return;
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(session_multicast_, ec);
    if (!ec) {
        const udp::endpoint endpoint(address, session_port_);
        session_socket_->send_to(boost::asio::buffer(data), endpoint, 0, ec);
    }
    if (ec) {
        pushEvent("ERROR", "send session multicast failed: " + ec.message());
    }
}

void TeacherService::sendMainAnnouncement(const std::vector<std::uint8_t>& data) {
    if (!main_socket_) return;
    std::vector<std::string> failures;
    std::size_t sent = 0;
    for (const auto& target : main_announce_targets_) {
        boost::system::error_code ec;
        main_socket_->send_to(boost::asio::buffer(data), target, 0, ec);
        if (ec) failures.push_back(target.address().to_string() + ":" + std::to_string(target.port()) + " " + ec.message());
        else ++sent;
    }
    if (sent == 0 && !failures.empty()) pushEvent("ERROR", "main announcement failed: " + failures.front());
    else if (!failures.empty()) pushEvent("WARN", "main announcement partially failed: " + failures.front());
}

void TeacherService::sendSessionAnnouncement(const std::vector<std::uint8_t>& data) {
    if (!session_socket_) return;
    std::vector<std::string> failures;
    std::size_t sent = 0;
    for (const auto& target : session_announce_targets_) {
        boost::system::error_code ec;
        session_socket_->send_to(boost::asio::buffer(data), target, 0, ec);
        if (ec) failures.push_back(target.address().to_string() + ":" + std::to_string(target.port()) + " " + ec.message());
        else ++sent;
    }
    if (sent == 0 && !failures.empty()) pushEvent("ERROR", "session announcement failed: " + failures.front());
    else if (!failures.empty()) pushEvent("WARN", "session announcement partially failed: " + failures.front());
}

void TeacherService::sendMainMulticast(const std::vector<std::uint8_t>& data) {
    if (!main_socket_) return;
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(protocol::kMainMulticast, ec);
    if (!ec) {
        const udp::endpoint endpoint(address, protocol::kMainPort);
        main_socket_->send_to(boost::asio::buffer(data), endpoint, 0, ec);
    }
    if (ec) {
        pushEvent("ERROR", "send main multicast failed: " + ec.message());
    }
}

bool TeacherService::requestRemoteViewFeatureSync(const std::string& student_ip, bool enabled, std::string& error_message) {
    if (!isRunning()) {
        error_message = "teacher service is not running";
        return false;
    }
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();
    boost::asio::post(io_, [this, student_ip, enabled, promise] {
        try {
            const auto payload = enabled
                ? protocol::buildRemoteViewPayload(localIp(), student_ip, options_.view_local_port, options_.view_peer_port, options_.tcp_mode)
                : protocol::buildRemoteViewStopPayload();
            const auto packet = protocol::buildMessPacket(student_ip, payload);
            if (!session_socket_) {
                promise->set_value("session socket is closed");
                return;
            }
            boost::system::error_code ec;
            const auto address = boost::asio::ip::make_address(student_ip, ec);
            if (!ec) session_socket_->send_to(boost::asio::buffer(packet), udp::endpoint(address, session_port_), 0, ec);
            promise->set_value(ec ? ec.message() : std::string{});
        } catch (const std::exception& e) {
            promise->set_value(e.what());
        }
    });
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        error_message = "sending remote-view feature timed out";
        return false;
    }
    error_message = future.get();
    return error_message.empty();
}

void TeacherService::queueRemoteInput(const std::string& student_ip, const std::vector<std::uint8_t>& packet) {
    std::shared_ptr<RemoteControlState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = remote_controls_.find(student_ip);
        if (it == remote_controls_.end()) return;
        state = it->second;
    }
    boost::asio::post(io_, [this, student_ip, state, packet] {
        const auto send_packet = [this, student_ip, state, packet] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = remote_controls_.find(student_ip);
                if (it == remote_controls_.end() || it->second != state) return;
            }
            boost::system::error_code ec;
            const auto address = boost::asio::ip::make_address(student_ip, ec);
            if (!ec && state->socket && state->socket->is_open()) {
                state->socket->send_to(boost::asio::buffer(packet), udp::endpoint(address, state->port), 0, ec);
            }
            if (ec) {
                pushEvent("ERROR", "[RemoteControl] input send failed: " + ec.message(), student_ip);
            } else {
                ++state->input_count;
                if (packet.size() >= 8 && (protocol::readLe32(packet, 4) == 16 || state->input_count <= 8 || state->input_count % 100 == 0)) {
                    logDebug("[RemoteControl] input #" + std::to_string(state->input_count)
                        + " kind=" + std::to_string(protocol::readLe32(packet, 4)), student_ip);
                }
            }
        };
        const auto now = std::chrono::steady_clock::now();
        if (now >= state->ready_at) {
            send_packet();
            return;
        }
        auto timer = std::make_shared<boost::asio::steady_timer>(io_);
        timer->expires_at(state->ready_at);
        timer->async_wait([send_packet, timer](boost::system::error_code ec) {
            if (!ec) send_packet();
        });
    });
}

void TeacherService::doUnlockOnIo(const std::string& student_ip) {
    sendSessionMulticast(protocol::buildUnlockMessage(student_ip));
}

void TeacherService::markStudentPacket(const std::string& ip, const std::string& magic) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& info = students_[ip];
    info.ip = ip;
    info.last_magic = magic;
    info.last_seen = std::chrono::system_clock::now();
    if (info.preview_status.empty()) {
        info.preview_status = "idle";
    }
}

bool TeacherService::hasStudentLocked(const std::string& ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = students_.find(ip);
    return it != students_.end() && it->second.logged_in;
}

std::vector<std::string> TeacherService::studentIpsLocked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ips;
    for (const auto& [ip, info] : students_) {
        if (info.logged_in) {
            ips.push_back(ip);
        }
    }
    std::sort(ips.begin(), ips.end());
    return ips;
}

std::filesystem::path TeacherService::nextPreviewPath(const std::string& student_ip, bool fixed) const {
    const auto base = ipForFilename(student_ip);
    for (int i = 0; i < 10000; ++i) {
        const auto name = "preview_" + base + "_" + std::to_string(i) + (fixed ? "_fixed.jpg" : ".jpg");
        auto path = options_.preview_dir / name;
        if (!std::filesystem::exists(path)) {
            return path;
        }
    }
    return options_.preview_dir / ("preview_" + base + (fixed ? "_fixed.jpg" : ".jpg"));
}

void TeacherService::pushEvent(std::string level, std::string message, std::string student_ip, std::filesystem::path preview_path, std::filesystem::path fixed_preview_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    ServiceEvent event;
    event.time = std::chrono::system_clock::now();
    event.level = std::move(level);
    event.message = std::move(message);
    event.student_ip = std::move(student_ip);
    event.preview_path = std::move(preview_path);
    event.fixed_preview_path = std::move(fixed_preview_path);
    events_.push_back(std::move(event));
    while (events_.size() > 2000) {
        events_.pop_front();
    }
}

void TeacherService::logDebug(const std::string& message, const std::string& student_ip) {
    if (debugLogging()) {
        pushEvent("DEBUG", message, student_ip);
    }
}

} // namespace jiyu
