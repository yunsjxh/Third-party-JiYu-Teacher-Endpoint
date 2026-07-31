#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jiyu {

struct RemoteFrame {
    std::string student_ip;
    std::uint32_t sequence = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

struct RemoteViewOptions {
    std::string local_ip;
    std::string student_ip;
    std::uint16_t tcp_port = 4806;
    std::uint32_t start_delay_ms = 0;
    std::filesystem::path output_dir;
    bool dump_h264 = false;
};

struct RemoteViewCallbacks {
    std::function<bool(std::string&)> request_feature_start;
    std::function<void()> connected;
    std::function<void(RemoteFrame)> frame;
    std::function<void(std::uint16_t, std::uint16_t)> cursor;
    std::function<void(const std::string&, const std::string&)> log;
    std::function<void(const std::string&)> closed;
};

class RemoteViewSession {
public:
    RemoteViewSession(RemoteViewOptions options, RemoteViewCallbacks callbacks);
    ~RemoteViewSession();

    void start();
    void stop();
    void join();
    bool connected() const;
    bool finished() const;
    std::uint64_t frameCount() const;

private:
    void run();
    void handleFragment(const std::uint8_t* data, std::size_t size);
    void handleRecord(std::uint32_t sequence, const std::vector<std::uint8_t>& record);
    void closeSocket();
    void log(const std::string& level, const std::string& message) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jiyu
