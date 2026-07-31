#pragma once

#include "preview_reassembler.hpp"
#include "remote_view.hpp"

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

struct StudentSystemInfo {
    bool valid = false;
    std::string computer_name;
    std::uint32_t student_id = 0;
    std::string mac;
    std::string login_user;
    std::string os_name;
    std::string os_version;
    std::string cpu_vendor;
    std::string cpu_model;
    std::string memory;
};

struct StudentListEntry {
    std::uint32_t id = 0;
    std::string name;
};

struct StudentInfo {
    std::string ip;
    bool logged_in = false;
    std::string last_magic;
    std::chrono::system_clock::time_point last_seen{};
    std::string preview_status = "idle";
    std::filesystem::path raw_preview;
    std::filesystem::path fixed_preview;
    StudentSystemInfo system_info;
    std::vector<StudentListEntry> processes;
    std::vector<StudentListEntry> windows;
    std::chrono::system_clock::time_point last_info_seen{};
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
    std::string local_ip;
    std::string peer_ip;
    std::string network_cidr;
    std::string teacher_name = "1";
    std::uint32_t channel = 1;
    std::uint32_t tcp_mode = 1;
    std::uint16_t tcp_port = 4806;
    std::uint16_t view_local_port = 0;
    std::uint16_t view_peer_port = 0;
    std::uint16_t control_port = 0;
    std::uint32_t control_mode = 2;
    std::uint32_t view_start_delay_ms = 0;
    std::uint32_t view_failure_cooldown_seconds = 60;
    bool remote_control_enabled = true;
    bool dump_remote_h264 = false;
};

struct RemoteViewState {
    bool active = false;
    bool connected = false;
    bool controlling = false;
    std::uint16_t tcp_port = 0;
    std::uint16_t control_port = 0;
    bool has_cursor = false;
    std::uint16_t cursor_x = 0;
    std::uint16_t cursor_y = 0;
    std::uint64_t frame_count = 0;
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
    std::vector<RemoteFrame> drainRemoteFrames();

    void requestPreview(const std::string& student_ip);
    void requestPreviewAll();
    void requestInfo(const std::string& student_ip, std::uint32_t report_type = 0);
    void requestInfoAll(std::uint32_t report_type = 0);
    void sendChat(const std::string& student_ip, const std::string& text);
    void sendBlackscreen(const std::string& student_ip, bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color = 0x0000FFFF);
    void sendUnlock(const std::string& student_ip);
    void sendBlackscreenAll(bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color = 0x0000FFFF);
    void sendUnlockAll();
    void sendShutdown(const std::string& student_ip, bool reboot, std::uint32_t delay_seconds, bool force, const std::string& text);
    void sendKillProcess(const std::string& student_ip, std::uint32_t pid, bool force);
    void sendCloseApplication(const std::string& student_ip, std::uint32_t hwnd, bool force);
    void sendOpenUrl(const std::string& student_ip, const std::string& url);
    void sendRunProgram(const std::string& student_ip, const std::string& path, const std::string& args, std::uint32_t show_mode, bool fallback = true);
    bool startRemoteView(const std::string& student_ip, std::uint16_t tcp_port, std::string* error_message = nullptr);
    void stopRemoteView(const std::string& student_ip, bool notify = true);
    bool startRemoteControl(const std::string& student_ip, std::uint16_t port, std::string* error_message = nullptr);
    void stopRemoteControl(const std::string& student_ip, bool notify = true);
    bool sendRemoteMouse(const std::string& student_ip, std::uint32_t message, std::uint32_t x, std::uint32_t y, std::int32_t data = 0);
    bool sendRemoteKey(const std::string& student_ip, std::uint16_t virtual_key, bool key_up, std::uint16_t scan_code = 0, bool extended = false);
    RemoteViewState remoteViewState(const std::string& student_ip) const;
    std::string sessionDescription() const;
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
    void sendMainAnnouncement(const std::vector<std::uint8_t>& data);
    void sendSessionAnnouncement(const std::vector<std::uint8_t>& data);
    bool requestRemoteViewFeatureSync(const std::string& student_ip, bool enabled, std::string& error_message);
    void queueRemoteInput(const std::string& student_ip, const std::vector<std::uint8_t>& packet);
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
    std::string selected_broadcast_ip_;
    std::string selected_host_candidate_ip_;
    std::string session_multicast_;
    unsigned short session_port_ = 0;
    TeacherServiceOptions options_;
    std::unordered_map<std::string, StudentInfo> students_;
    std::deque<ServiceEvent> events_;
    PreviewReassembler previews_;
    std::unordered_map<std::string, std::uint32_t> preview_policy_versions_;
    std::unordered_map<std::string, std::shared_ptr<RemoteViewSession>> remote_views_;
    std::unordered_map<std::string, RemoteViewState> remote_view_states_;
    std::unordered_map<std::string, RemoteFrame> remote_frames_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> remote_view_failures_;

    struct RemoteControlState {
        std::shared_ptr<udp::socket> socket;
        std::uint16_t port = 0;
        std::chrono::steady_clock::time_point ready_at{};
        std::uint64_t input_count = 0;
    };
    std::unordered_map<std::string, std::shared_ptr<RemoteControlState>> remote_controls_;
    std::vector<udp::endpoint> main_announce_targets_;
    std::vector<udp::endpoint> session_announce_targets_;

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
