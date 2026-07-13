#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace jiyu {

struct CompletedPreview {
    std::string student_ip;
    std::uint32_t total = 0;
    std::vector<std::uint8_t> jpeg;
};

class PreviewReassembler {
public:
    std::optional<CompletedPreview> accept(const std::string& student_ip, const std::uint8_t* data, std::size_t size);
    void reset(const std::string& student_ip);
    bool hasActivePreview(const std::string& student_ip) const;
    std::string status(const std::string& student_ip) const;

private:
    struct State {
        std::uint32_t total = 0;
        std::vector<std::uint8_t> buffer;
        std::vector<std::uint8_t> seen;
        std::uint32_t got = 0;
    };

    std::unordered_map<std::string, State> states_;
};

} // namespace jiyu
