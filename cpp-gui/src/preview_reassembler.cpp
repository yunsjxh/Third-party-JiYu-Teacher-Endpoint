#include "preview_reassembler.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <sstream>

namespace jiyu {

std::optional<CompletedPreview> PreviewReassembler::accept(const std::string& student_ip, const std::uint8_t* data, std::size_t size) {
    if (!data || size < 48) {
        return std::nullopt;
    }
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
    if (state.total != total || state.buffer.size() != total) {
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
        done.total = state.total;
        done.jpeg = std::move(state.buffer);
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

} // namespace jiyu

