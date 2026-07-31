#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace jiyu {

struct CompletedPreview {
    std::string student_ip;
    std::uint32_t frame_sequence = 0;
    std::uint32_t total = 0;
    std::vector<std::uint8_t> jpeg;
};

struct PreviewAcknowledgement {
    bool known = false;
    bool complete = false;
    std::vector<std::uint16_t> missing_parts;
};

class PreviewReassembler {
public:
    std::optional<CompletedPreview> accept(const std::string& student_ip, const std::uint8_t* data, std::size_t size);
    void reset(const std::string& student_ip);
    bool hasActivePreview(const std::string& student_ip) const;
    std::string status(const std::string& student_ip) const;
    PreviewAcknowledgement acknowledgement(const std::string& student_ip, std::uint32_t frame_sequence) const;

private:
    struct State {
        std::uint32_t frame_sequence = 0;
        std::uint32_t total = 0;
        std::vector<std::uint8_t> buffer;
        std::vector<std::uint8_t> seen;
        std::uint32_t got = 0;
    };

    std::unordered_map<std::string, State> states_;
    std::unordered_map<std::string, std::uint32_t> completed_frames_;
};

} // namespace jiyu
