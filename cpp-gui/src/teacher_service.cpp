#include "teacher_service.hpp"

#include "image_fix.hpp"
#include "protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace jiyu {
namespace {

using udp = boost::asio::ip::udp;

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
            ServiceEvent start_event;
            start_event.time = std::chrono::system_clock::now();
            start_event.level = "INFO";
            start_event.message = "Teacher " + local_ip_ + " started on 224.50.50.42:4705 + 225.2.2.1:5512";
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

void TeacherService::requestPreview(const std::string& student_ip) {
    boost::asio::post(io_, [this, student_ip] {
        sendMainTo(student_ip, protocol::buildTnrsRequest());
        pushEvent("INFO", "[Preview] TNRS -> " + student_ip, student_ip);
    });
}

void TeacherService::requestPreviewAll() {
    for (const auto& ip : studentIpsLocked()) {
        requestPreview(ip);
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
            sendMainTo(student_ip, protocol::buildComdLock(true, command_sequence_++));
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
    session_socket_->bind(udp::endpoint(udp::v4(), protocol::kSessionPort), ec);
    if (ec) goto fail;
    session_socket_->set_option(boost::asio::ip::multicast::join_group(boost::asio::ip::make_address_v4(protocol::kSessionMulticast), local_address), ec);
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
    boost::asio::io_context ctx;
    udp::socket socket(ctx);
    boost::system::error_code ec;
    socket.open(udp::v4(), ec);
    if (ec) {
        throw std::runtime_error(ec.message());
    }
    socket.connect(udp::endpoint(boost::asio::ip::make_address("8.8.8.8", ec), 80), ec);
    if (!ec) {
        auto endpoint = socket.local_endpoint(ec);
        if (!ec && endpoint.address().is_v4() && !endpoint.address().is_loopback()) {
            return endpoint.address().to_string();
        }
    }

    char hostname[256] = {};
    if (::gethostname(hostname, static_cast<int>(sizeof(hostname))) == 0) {
        boost::asio::ip::tcp::resolver resolver(ctx);
        auto results = resolver.resolve(hostname, "", ec);
        if (!ec) {
            for (const auto& result : results) {
                const auto address = result.endpoint().address();
                if (address.is_v4() && !address.is_loopback()) {
                    return address.to_string();
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
    if (magic == protocol::kOonc || magic == protocol::kCanc || magic == protocol::kAnno) {
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
        sendMainTo(sip, protocol::buildWaca(localIp()));
        break;
    case protocol::kTrmc:
        sendMainTo(sip, protocol::buildLpntSubtype3());
        sendMainTo(sip, protocol::buildDmoc(localIp()));
        break;
    case protocol::kTrnt:
        break;
    case protocol::kDent:
        sendMainTo(sip, protocol::buildDentAck());
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
        sendSessionTo(sip, protocol::buildLoginMessType8000(sip));
        if (!already_logged) {
            sendMainTo(sip, protocol::buildLpntSubtype2());
            sendMainTo(sip, protocol::buildLpntSubtype3());
            sendMainTo(sip, protocol::buildDmoc(localIp()));
            std::lock_guard<std::mutex> lock(mutex_);
            auto& info = students_[sip];
            info.ip = sip;
            info.logged_in = true;
            info.preview_status = "idle";
        }
        pushEvent("INFO", "[Login] " + sip + " 登录成功", sip);
    } else if (magic == protocol::kMess) {
        markStudentPacket(sip, "MESS");
        handleMess(packet, remote, "SessionRecv");
    } else if (magic != protocol::kOonc && magic != protocol::kCanc && magic != protocol::kAnno) {
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
    if (payload_len >= 16 && protocol::readLe32(payload + 4) == 0x800) {
        decoded = "[聊天] " + protocol::utf16LeToUtf8(payload + 16, payload_len - 16);
    } else if (payload_len >= 24 && protocol::readLe32(payload + 4) == 0) {
        const auto subtype = protocol::readLe32(payload + 12);
        const auto extra = protocol::readLe32(payload + 20);
        if (subtype == 6) {
            decoded = "[窗口标题] " + protocol::utf16LeToUtf8(payload + 24, payload_len - 24);
        } else if (subtype == 7) {
            decoded = "[WiFi可用网络数量] " + std::to_string(extra == 0xffffffff ? -1 : static_cast<int>(extra));
        } else if (subtype == 1) {
            decoded = "[IE/浏览器URL信息] extra=0x";
            std::ostringstream oss;
            oss << decoded << std::hex << std::uppercase << extra;
            decoded = oss.str();
        } else if (subtype == 0) {
            decoded = "[窗口标题清空/PID]";
        } else if (subtype == 3) {
            decoded = "[系统性能/进程信息]";
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
        sendMainMulticast(protocol::buildOonc(localIp(), oonc_sequence_++));
    } else {
        sendMainMulticast(protocol::buildCanc(localIp()));
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
        sendSessionMulticast(protocol::buildAnnoShort());
        session_timer_->expires_after(std::chrono::milliseconds(300));
    } else {
        sendSessionMulticast(protocol::buildAnnoLong(localIp()));
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
            sendMainTo(ip, protocol::buildLpntSubtype3());
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
    const udp::endpoint endpoint(boost::asio::ip::make_address(ip, ec), protocol::kSessionPort);
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
    const auto address = boost::asio::ip::make_address(protocol::kSessionMulticast, ec);
    if (!ec) {
        const udp::endpoint endpoint(address, protocol::kSessionPort);
        session_socket_->send_to(boost::asio::buffer(data), endpoint, 0, ec);
    }
    if (ec) {
        pushEvent("ERROR", "send session multicast failed: " + ec.message());
    }
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

void TeacherService::doUnlockOnIo(const std::string& student_ip) {
    sendSessionMulticast(protocol::buildUnlockMessage(student_ip));
    sendMainTo(student_ip, protocol::buildComdLock(false, command_sequence_++));
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

