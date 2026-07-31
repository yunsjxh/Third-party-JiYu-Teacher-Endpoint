#include "preview_reassembler.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <sstream>

namespace jiyu {

std::optional<CompletedPreview> PreviewReassembler::accept(const std::string& student_ip, const std::uint8_t* data, std::size_t size) {
    if (!data || size < 48) {
        return std::nullopt;
    }
    const auto frame_sequence = protocol::readLe32(data + 32);
    const auto total = protocol::readLe32(data + 36);
    const auto offset = protocol::readLe32(data + 40);
    const auto frag_len = protocol::readLe32(data + 44);
    if (total == 0 || frag_len == 0 || offset >= total || size < 48) {
        return std::nullopt;
    }

    const std::size_t available = size > 48 ? size - 48 : 0;
    const std::size_t copy_len = std::min<std::size_t>({ frag_len, available, static_cast<std::size_t>(total - offset) });
    if (copy_len == 0) {
        return std::nullopt;
    }

    auto& state = states_[student_ip];
    if (state.frame_sequence != frame_sequence || state.total != total || state.buffer.size() != total) {
        state.frame_sequence = frame_sequence;
        state.total = total;
        state.buffer.assign(total, 0);
        state.seen.assign(total, 0);
        state.got = 0;
    }

    const auto* frag = data + 48;
    for (std::size_t i = 0; i < copy_len; ++i) {
        const std::size_t pos = static_cast<std::size_t>(offset) + i;
        state.buffer[pos] = frag[i];
        if (!state.seen[pos]) {
            state.seen[pos] = 1;
            ++state.got;
        }
    }

    if (state.got >= state.total) {
        CompletedPreview done;
        done.student_ip = student_ip;
        done.frame_sequence = state.frame_sequence;
        done.total = state.total;
        done.jpeg = std::move(state.buffer);
        completed_frames_[student_ip] = state.frame_sequence;
        states_.erase(student_ip);
        return done;
    }
    return std::nullopt;
}

void PreviewReassembler::reset(const std::string& student_ip) {
    states_.erase(student_ip);
}

bool PreviewReassembler::hasActivePreview(const std::string& student_ip) const {
    return states_.find(student_ip) != states_.end();
}

std::string PreviewReassembler::status(const std::string& student_ip) const {
    const auto it = states_.find(student_ip);
    if (it == states_.end()) {
        return "idle";
    }
    std::ostringstream oss;
    oss << it->second.got << '/' << it->second.total;
    return oss.str();
}

PreviewAcknowledgement PreviewReassembler::acknowledgement(const std::string& student_ip, std::uint32_t frame_sequence) const {
    PreviewAcknowledgement result;
    const auto completed = completed_frames_.find(student_ip);
    if (completed != completed_frames_.end() && completed->second == frame_sequence) {
        result.known = true;
        result.complete = true;
        return result;
    }

    const auto it = states_.find(student_ip);
    if (it == states_.end() || it->second.frame_sequence != frame_sequence) {
        return result;
    }
    result.known = true;
    const auto& state = it->second;
    const std::size_t part_count = (static_cast<std::size_t>(state.total) + 1023) / 1024;
    result.missing_parts.reserve(part_count);
    for (std::size_t part = 0; part < part_count; ++part) {
        const std::size_t begin = part * 1024;
        const std::size_t end = std::min<std::size_t>(begin + 1024, state.seen.size());
        if (std::find(state.seen.begin() + static_cast<std::ptrdiff_t>(begin), state.seen.begin() + static_cast<std::ptrdiff_t>(end), 0) != state.seen.begin() + static_cast<std::ptrdiff_t>(end)) {
            if (part <= 0xffff) {
                result.missing_parts.push_back(static_cast<std::uint16_t>(part));
            }
        }
    }
    result.complete = result.missing_parts.empty() && state.got >= state.total;
    return result;
}

} // namespace jiyu

