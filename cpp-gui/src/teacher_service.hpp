#pragma once

#include "preview_reassembler.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

namespace jiyu {

struct StudentInfo {
    std::string ip;
    bool logged_in = false;
    std::string last_magic;
    std::chrono::system_clock::time_point last_seen{};
    std::string preview_status = "idle";
    std::filesystem::path raw_preview;
    std::filesystem::path fixed_preview;
};

struct ServiceEvent {
    std::chrono::system_clock::time_point time{};
    std::string level;
    std::string message;
    std::string student_ip;
    std::filesystem::path preview_path;
    std::filesystem::path fixed_preview_path;
};

struct TeacherServiceOptions {
    std::filesystem::path preview_dir;
};

class TeacherService {
public:
    TeacherService();
    ~TeacherService();

    bool start(const TeacherServiceOptions& options, std::string* error_message = nullptr);
    void stop();
    bool isRunning() const;
    std::string localIp() const;

    std::vector<StudentInfo> studentsSnapshot() const;
    std::vector<ServiceEvent> drainEvents();

    void requestPreview(const std::string& student_ip);
    void requestPreviewAll();
    void sendChat(const std::string& student_ip, const std::string& text);
    void sendBlackscreen(const std::string& student_ip, bool lock_input, std::uint32_t timeout_seconds, const std::string& text);
    void sendUnlock(const std::string& student_ip);
    void sendBlackscreenAll(bool lock_input, std::uint32_t timeout_seconds, const std::string& text);
    void sendUnlockAll();
    void setDebugLogging(bool enabled);
    bool debugLogging() const;

private:
    using udp = boost::asio::ip::udp;

    void resetIoObjects();
    bool openSockets(std::string* error_message);
    std::string detectLocalIp();
    static std::filesystem::path defaultPreviewDir();

    void runIo();
    void doMainReceive();
    void doSessionReceive();
    void handleMainPacket(const std::vector<std::uint8_t>& packet, const udp::endpoint& remote);
    void handleSessionPacket(const std::vector<std::uint8_t>& packet, const udp::endpoint& remote);
    void handleMess(const std::vector<std::uint8_t>& packet, const udp::endpoint& remote, const std::string& via);
    void handleCompletedPreview(const CompletedPreview& preview);

    void scheduleBroadcast(bool send_oonc);
    void scheduleSessionAnno(bool short_packet);
    void scheduleKeepAlive();
    void scheduleAutoUnlock(const std::string& student_ip, std::uint32_t timeout_seconds);
    void cancelAutoUnlock(const std::string& student_ip);

    void sendMainTo(const std::string& ip, const std::vector<std::uint8_t>& data);
    void sendSessionTo(const std::string& ip, const std::vector<std::uint8_t>& data);
    void sendSessionMulticast(const std::vector<std::uint8_t>& data);
    void sendMainMulticast(const std::vector<std::uint8_t>& data);
    void doUnlockOnIo(const std::string& student_ip);
    void markStudentPacket(const std::string& ip, const std::string& magic);
    bool hasStudentLocked(const std::string& ip) const;
    std::vector<std::string> studentIpsLocked() const;
    std::filesystem::path nextPreviewPath(const std::string& student_ip, bool fixed) const;

    void pushEvent(std::string level, std::string message, std::string student_ip = {}, std::filesystem::path preview_path = {}, std::filesystem::path fixed_preview_path = {});
    void logDebug(const std::string& message, const std::string& student_ip = {});

    mutable std::mutex mutex_;
    bool running_ = false;
    bool debug_logging_ = false;
    std::string local_ip_;
    TeacherServiceOptions options_;
    std::unordered_map<std::string, StudentInfo> students_;
    std::deque<ServiceEvent> events_;
    PreviewReassembler previews_;

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::unique_ptr<udp::socket> main_socket_;
    std::unique_ptr<udp::socket> session_socket_;
    std::unique_ptr<boost::asio::steady_timer> broadcast_timer_;
    std::unique_ptr<boost::asio::steady_timer> session_timer_;
    std::unique_ptr<boost::asio::steady_timer> keepalive_timer_;
    std::unordered_map<std::string, std::shared_ptr<boost::asio::steady_timer>> blackscreen_timers_;
    std::thread io_thread_;

    std::array<std::uint8_t, 65536> main_recv_buffer_{};
    std::array<std::uint8_t, 65536> session_recv_buffer_{};
    udp::endpoint main_remote_;
    udp::endpoint session_remote_;
    std::uint32_t oonc_sequence_ = 0;
    std::uint32_t command_sequence_ = 0;
};

} // namespace jiyu
