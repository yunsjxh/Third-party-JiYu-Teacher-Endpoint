#include "protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace jiyu::protocol {
namespace {

const std::uint8_t kTeacherGuidLe[16] = { 0x19, 0x6d, 0x6a, 0xf9, 0x29, 0x5b, 0xb9, 0x46, 0xab, 0x95, 0x8a, 0x14, 0x3e, 0xcd, 0xdc, 0x26 };
const std::uint8_t kTeacherGuidComd[16] = { 0xf9, 0x6a, 0x6d, 0x19, 0x5b, 0x29, 0x46, 0xb9, 0xab, 0x95, 0x8a, 0x14, 0x3e, 0xcd, 0xdc, 0x26 };
const std::uint8_t kPreviewGuid[16] = { 0xaa, 0x3a, 0x8d, 0xbe, 0x2b, 0x90, 0x66, 0x45, 0x90, 0x8e, 0xa2, 0x95, 0x26, 0x21, 0x85, 0x40 };
const std::uint8_t kDmocGuid[16] = { 0xce, 0x90, 0xfd, 0x38, 0x3d, 0xf5, 0x84, 0x4c, 0x85, 0x7f, 0xa3, 0x51, 0x83, 0xc0, 0x51, 0xf3 };

void appendLe32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void appendLe16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void appendRaw(Bytes& out, const std::uint8_t* data, std::size_t size) {
    out.insert(out.end(), data, data + size);
}

void appendBytes(Bytes& out, const Bytes& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

Bytes bytesFromArray(const std::uint8_t* data, std::size_t size) {
    return Bytes(data, data + size);
}

Bytes utf16LeNoNul(const std::string& utf8_text) {
    Bytes out = utf16LeZ(utf8_text);
    if (out.size() >= 2) {
        out.resize(out.size() - 2);
    }
    return out;
}

Bytes teacherNameUtf16(const std::string& teacher_name) {
    std::string name = teacher_name.empty() ? "1" : teacher_name;
    Bytes encoded = utf16LeZ(name);
    constexpr std::size_t kMaxBytes = 64;
    if (encoded.size() > kMaxBytes) {
        encoded.resize(kMaxBytes);
        encoded[kMaxBytes - 2] = 0;
        encoded[kMaxBytes - 1] = 0;
    }
    return encoded;
}

void appendFixedUtf16LeField(Bytes& out, const std::string& text, std::size_t field_bytes, std::size_t max_text_bytes) {
    Bytes raw = utf16LeNoNul(text);
    if (raw.size() > max_text_bytes) {
        raw.resize(max_text_bytes);
    }
    if (raw.size() % 2 != 0) {
        raw.pop_back();
    }
    appendBytes(out, raw);
    if (raw.size() < field_bytes) {
        out.insert(out.end(), field_bytes - raw.size(), 0);
    }
}

Bytes withPacketHeader(std::uint32_t magic, std::uint32_t version, std::uint32_t length, const std::uint8_t* guid, const Bytes& payload) {
    Bytes out;
    appendLe32(out, magic);
    appendLe32(out, version);
    appendLe32(out, length);
    if (guid) {
        appendRaw(out, guid, 16);
    }
    appendBytes(out, payload);
    return out;
}

} // namespace

SessionEndpoint sessionEndpoint(std::uint32_t channel) {
    if (channel < 1 || channel > 32) {
        throw std::invalid_argument("channel must be in 1..32");
    }
    SessionEndpoint endpoint;
    endpoint.multicast = std::string(kSessionMulticastPrefix) + "." + std::to_string(channel + 1) + ".1";
    endpoint.port = static_cast<unsigned short>(kSessionBasePort + channel * kSessionPortStride);
    return endpoint;
}

std::string magicName(std::uint32_t magic) {
    switch (magic) {
    case kOonc: return "OONC";
    case kNanc: return "NANC";
    case kCanc: return "CANC";
    case kWaca: return "WACA";
    case kTnrs: return "TNRS";
    case kComd: return "DMOC/COMD";
    case kLpnt: return "LPNT";
    case kKaca: return "KACA";
    case kTrmc: return "TRMC";
    case kTrnt: return "TRNT";
    case kDent: return "DENT";
    case kLant: return "LANT";
    case kAnno: return "ANNO";
    case kLogi: return "LOGI";
    case kMess: return "MESS";
    default: {
        std::ostringstream oss;
        oss << "UNKNOWN(0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << magic << ")";
        return oss.str();
    }
    }
}

std::uint32_t readLe32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint32_t readLe32(const Bytes& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::out_of_range("readLe32 offset outside buffer");
    }
    return readLe32(data.data() + offset);
}

std::string hexdump(const std::uint8_t* data, std::size_t size, std::size_t width) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < size; i += width) {
        const std::size_t chunk = std::min(width, size - i);
        oss << std::setw(4) << std::setfill('0') << std::hex << i << "  ";
        for (std::size_t j = 0; j < width; ++j) {
            if (j < chunk) {
                oss << std::setw(2) << static_cast<int>(data[i + j]) << ' ';
            } else {
                oss << "   ";
            }
        }
        oss << ' ';
        for (std::size_t j = 0; j < chunk; ++j) {
            const auto ch = data[i + j];
            oss << (ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.');
        }
        if (i + width < size) {
            oss << '\n';
        }
    }
    return oss.str();
}

std::string hexdump(const Bytes& data, std::size_t width) {
    return hexdump(data.data(), data.size(), width);
}

Bytes ipv4Bytes(const std::string& ip) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    char tail = '\0';
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        throw std::invalid_argument("invalid IPv4 address: " + ip);
    }
    return Bytes{ static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d) };
}

Bytes utf16LeZ(const std::string& utf8_text) {
    Bytes out;
    if (!utf8_text.empty()) {
        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(), static_cast<int>(utf8_text.size()), nullptr, 0);
        if (wide_len <= 0) {
            return Bytes{ 0, 0 };
        }
        std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(), static_cast<int>(utf8_text.size()), wide.data(), wide_len);
        for (wchar_t wc : wide) {
            const auto v = static_cast<std::uint32_t>(wc);
            out.push_back(static_cast<std::uint8_t>(v & 0xff));
            out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        }
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

std::string utf16LeToUtf8(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 2) {
        return {};
    }
    while (size >= 2 && data[size - 1] == 0 && data[size - 2] == 0) {
        size -= 2;
    }
    if (size == 0) {
        return {};
    }
    if (size % 2 != 0) {
        --size;
    }
    std::wstring wide;
    wide.reserve(size / 2);
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        wide.push_back(static_cast<wchar_t>(data[i] | (data[i + 1] << 8)));
    }
    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), utf8_len, nullptr, nullptr);
    return out;
}

Bytes buildOonc(const std::string& teacher_ip, std::uint32_t sequence) {
    Bytes payload;
    appendBytes(payload, ipv4Bytes(teacher_ip));
    appendLe32(payload, 1);
    appendLe32(payload, 1);
    appendLe32(payload, sequence);
    return withPacketHeader(kOonc, 0x10000, 16, kTeacherGuidLe, payload);
}

Bytes buildNanc(const std::string& teacher_ip, std::uint32_t channel, const std::string& teacher_name) {
    if (channel < 1 || channel > 32) {
        throw std::invalid_argument("channel must be in 1..32");
    }
    const Bytes name = teacherNameUtf16(teacher_name);
    const auto name_chars = static_cast<std::uint32_t>(name.size() / 2 - 1);
    Bytes payload;
    appendLe32(payload, (name_chars << 17) | channel);
    appendBytes(payload, ipv4Bytes(teacher_ip));
    appendBytes(payload, name);
    const std::size_t declared = 11 + static_cast<std::size_t>(name_chars) * 2;
    if (payload.size() < declared) {
        payload.resize(declared, 0);
    }
    return withPacketHeader(kNanc, 0x10000, static_cast<std::uint32_t>(payload.size()), kTeacherGuidLe, payload);
}

Bytes buildCanc(const std::string& teacher_ip, std::uint32_t channel, const std::string& teacher_name) {
    if (channel < 1 || channel > 32) {
        throw std::invalid_argument("channel must be in 1..32");
    }
    const Bytes name = teacherNameUtf16(teacher_name);
    const auto name_chars = static_cast<std::uint32_t>(name.size() / 2 - 1);
    Bytes payload;
    appendLe32(payload, (name_chars << 17) | channel);
    appendBytes(payload, ipv4Bytes(teacher_ip));
    appendLe32(payload, 1U << (channel - 1));
    appendLe32(payload, 1);
    appendBytes(payload, name);
    if (payload.size() > 84) {
        throw std::invalid_argument("teacher name exceeds CANC payload capacity");
    }
    payload.resize(84, 0);
    return withPacketHeader(kCanc, 0x10000, 84, kTeacherGuidLe, payload);
}

Bytes buildWaca(const std::string& teacher_ip, std::uint32_t channel) {
    if (channel < 1 || channel > 32) {
        throw std::invalid_argument("channel must be in 1..32");
    }
    Bytes payload;
    appendBytes(payload, ipv4Bytes(teacher_ip));
    appendLe32(payload, channel);
    return withPacketHeader(kWaca, 0x10000, 8, kTeacherGuidLe, payload);
}

Bytes buildTnrsRequest() {
    Bytes payload;
    appendLe32(payload, 0x48);
    appendLe32(payload, 1);
    appendLe32(payload, 0);
    appendLe32(payload, 0x100);
    return withPacketHeader(kTnrs, 0x10000, 16, kPreviewGuid, payload);
}

Bytes buildDentAck() {
    Bytes payload;
    const std::uint8_t tail[] = { 0x06,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x09,0x00 };
    appendRaw(payload, tail, sizeof(tail));
    return withPacketHeader(0x544E5253, 0x10000, 14, kPreviewGuid, payload);
}

Bytes buildDmoc(const std::string& teacher_ip) {
    Bytes dd;
    const std::uint8_t prefix[] = { 0x20, 0x4e, 0x00, 0x00 };
    appendRaw(dd, prefix, sizeof(prefix));
    appendBytes(dd, ipv4Bytes(teacher_ip));
    const std::uint8_t middle[] = {
        0x35,0x00,0x00,0x00,0x35,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x80,
        0xe1,0x02,0x02,0x33,0x1e,0x16,0xe1,0x02,0x02,0x34,0x21,0x16,0x00,0x00,0xa0,0x46,
        0x00,0x00,0x20,0x41,0x9a,0x99,0x99,0x3f,0xa0,0x05,0x20,0x00,
        0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3d,0x00
    };
    appendRaw(dd, middle, sizeof(middle));
    return withPacketHeader(kComd, 0x10000, static_cast<std::uint32_t>(dd.size()), kDmocGuid, dd);
}

Bytes buildLpnt(std::uint32_t policy_version, bool enabled, std::uint32_t width, std::uint32_t height, std::uint32_t refresh_seconds) {
    Bytes payload;
    appendLe32(payload, policy_version);
    appendLe32(payload, enabled ? 1U : 0U);
    appendLe32(payload, width);
    appendLe32(payload, height);
    appendLe32(payload, refresh_seconds);
    return withPacketHeader(kLpnt, 0x10000, 20, kPreviewGuid, payload);
}

Bytes buildLpntSubtype2() {
    return buildLpnt(2, false);
}

Bytes buildLpntSubtype3() {
    return buildLpnt(3, true);
}

Bytes buildSrnt(std::uint32_t frame_sequence, bool complete, const std::optional<std::vector<std::uint16_t>>& missing_parts) {
    Bytes payload;
    appendLe32(payload, frame_sequence);
    if (!missing_parts) {
        appendLe32(payload, 0);
        appendLe32(payload, 0xffffffffU);
    } else {
        appendLe32(payload, complete ? 1U : 0U);
        appendLe32(payload, static_cast<std::uint32_t>(missing_parts->size()));
        for (const auto part : *missing_parts) {
            appendLe16(payload, part);
        }
    }
    payload.push_back(0);
    payload.push_back(0);
    return withPacketHeader(kSrnt, 0x10000, static_cast<std::uint32_t>(payload.size()), kPreviewGuid, payload);
}

Bytes buildMessPacket(const std::string& student_ip, const Bytes& payload) {
    Bytes out;
    appendLe32(out, kMess);
    appendLe32(out, 1);
    appendLe32(out, 1);
    appendBytes(out, ipv4Bytes(student_ip));
    appendBytes(out, payload);
    return out;
}

Bytes buildSessionReply(const std::string& student_ip, std::uint32_t message_type, std::uint32_t tcp_mode, std::uint16_t tcp_port) {
    Bytes payload;
    if (message_type == 0x1000) {
        appendLe32(payload, 0x0d);
        appendLe32(payload, 0x1000);
        appendLe32(payload, 0);
        payload.push_back(0);
    } else if (message_type == 0x8000) {
        appendLe32(payload, 0x1b);
        appendLe32(payload, 0x8000);
        appendLe32(payload, 0);
        appendLe32(payload, tcp_mode);
        appendLe16(payload, tcp_port);
        payload.insert(payload.end(), 4, 0);
        appendLe32(payload, 0x270034b0);
        payload.push_back(0);
    } else {
        throw std::invalid_argument("unsupported session reply type");
    }
    return buildMessPacket(student_ip, payload);
}

Bytes buildLoginMessType1000(const std::string& student_ip) {
    return buildSessionReply(student_ip, 0x1000);
}

Bytes buildLoginMessType8000(const std::string& student_ip, std::uint32_t tcp_mode, std::uint16_t tcp_port) {
    return buildSessionReply(student_ip, 0x8000, tcp_mode, tcp_port);
}

Bytes buildChatMessage(const std::string& student_ip, const std::string& text) {
    const Bytes text_utf16 = utf16LeZ(text);
    const std::uint32_t wchar_count = static_cast<std::uint32_t>(text_utf16.size() / 2);
    Bytes payload;
    appendLe32(payload, static_cast<std::uint32_t>(16 + text_utf16.size()));
    appendLe32(payload, 0x800);
    appendLe32(payload, 0);
    appendLe32(payload, wchar_count);
    appendBytes(payload, text_utf16);

    Bytes out;
    appendLe32(out, kMess);
    appendLe32(out, 1);
    appendLe32(out, 1);
    appendBytes(out, ipv4Bytes(student_ip));
    appendBytes(out, payload);
    return out;
}

Bytes buildInfoRequestMessage(const std::string& student_ip, std::uint32_t report_type) {
    Bytes payload;
    appendLe32(payload, 16);
    appendLe32(payload, 0x100000);
    appendLe32(payload, 0);
    appendLe32(payload, report_type);

    Bytes out;
    appendLe32(out, kMess);
    appendLe32(out, 1);
    appendLe32(out, 1);
    appendBytes(out, ipv4Bytes(student_ip));
    appendBytes(out, payload);
    return out;
}

Bytes buildKillMessage(const std::string& student_ip, std::uint32_t pid, std::uint32_t hwnd, bool force) {
    const std::uint32_t report_type = pid != 0 ? 4 : 3;
    Bytes payload;
    // Python-compatible: first length field is 24 even though the wire payload
    // carries seven DWORDs. Student sub_445670 reads type/hwnd/pid/force from it.
    appendLe32(payload, 24);
    appendLe32(payload, 0x100000);
    appendLe32(payload, 0);
    appendLe32(payload, report_type);
    appendLe32(payload, hwnd);
    appendLe32(payload, pid);
    appendLe32(payload, force ? 1 : 0);

    Bytes out;
    appendLe32(out, kMess);
    appendLe32(out, 1);
    appendLe32(out, 1);
    appendBytes(out, ipv4Bytes(student_ip));
    appendBytes(out, payload);
    return out;
}

Bytes buildComdCommandEx(std::uint32_t cmd_code, const Bytes& payload, const Bytes& guid, const Bytes& extra_header, std::uint32_t command_id) {
    Bytes inner;
    appendLe32(inner, cmd_code);
    appendLe32(inner, command_id);
    appendLe32(inner, static_cast<std::uint32_t>(payload.size()));
    appendLe32(inner, 0);
    appendBytes(inner, payload);

    Bytes out;
    appendLe32(out, kComd);
    appendLe32(out, 0x10000);
    appendLe32(out, static_cast<std::uint32_t>(extra_header.size() + inner.size()));
    appendBytes(out, guid);
    appendBytes(out, extra_header);
    appendBytes(out, inner);
    return out;
}

Bytes buildComdLock(std::uint32_t timeout_seconds, std::uint32_t command_id) {
    Bytes payload;
    appendLe32(payload, 0x200);
    appendLe32(payload, 0);
    appendLe32(payload, 6);
    appendLe32(payload, 1);
    appendLe32(payload, 1);
    appendLe32(payload, timeout_seconds);
    appendLe32(payload, 0);
    payload.insert(payload.end(), 8, 0);
    return buildComdCommandEx(0x80000010, payload, bytesFromArray(kTeacherGuidComd, 16), {}, command_id);
}

Bytes buildShutdownCommand(bool reboot, std::uint32_t delay_seconds, bool force, const std::string& text, std::uint32_t command_id) {
    std::uint32_t cmd = reboot ? 0x13 : 0x14;
    if (force) {
        cmd |= 0x10000000;
    }

    Bytes payload;
    appendLe32(payload, 0x200);
    appendLe32(payload, 0);
    appendLe32(payload, cmd);
    appendLe32(payload, delay_seconds);
    payload.insert(payload.end(), 8, 0);
    if (!text.empty()) {
        appendBytes(payload, utf16LeZ(text));
    }
    // The student receiver copies from body+0x0C using the packet length and
    // consumes four bytes past the logical payload; this mirrors the Python
    // implementation and keeps the optional text terminator intact.
    payload.insert(payload.end(), 4, 0);
    return buildComdCommandEx(0x80000010, payload, bytesFromArray(kTeacherGuidComd, 16), {}, command_id);
}

Bytes buildOpenUrlCommand(const std::string& url, std::uint32_t command_id) {
    Bytes payload;
    appendLe32(payload, 0x200);
    appendLe32(payload, 0);
    appendLe32(payload, 0x18);
    appendLe32(payload, 0);
    appendBytes(payload, utf16LeZ(url));
    payload.insert(payload.end(), 4, 0);
    return buildComdCommandEx(0x80000010, payload, bytesFromArray(kTeacherGuidComd, 16), {}, command_id);
}

Bytes buildRunProgramCommand(const std::string& path, const std::string& args, std::uint32_t show_mode, bool fallback, std::uint32_t command_id) {
    Bytes payload;
    appendLe32(payload, 0x200);
    appendLe32(payload, 0);
    appendLe32(payload, 0x0F);
    appendLe32(payload, fallback ? 1 : 0);
    appendFixedUtf16LeField(payload, path, 512, 510);
    appendFixedUtf16LeField(payload, args, 320, 318);
    appendLe32(payload, show_mode);
    payload.insert(payload.end(), 4, 0);
    return buildComdCommandEx(0x80000010, payload, bytesFromArray(kTeacherGuidComd, 16), {}, command_id);
}

Bytes buildBlackscreenMessPayload(bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color) {
    const bool has_text = !text.empty();
    const Bytes text_utf16 = has_text ? utf16LeZ(text) : Bytes{};
    Bytes payload;
    appendLe32(payload, static_cast<std::uint32_t>(39 + text_utf16.size()));
    appendLe32(payload, 0x20);
    appendLe32(payload, 0x80000000);
    appendLe32(payload, lock_input ? 1 : 0);
    appendLe32(payload, 1);
    appendLe32(payload, timeout_seconds);
    appendLe32(payload, has_text ? 1 : 0);
    appendLe32(payload, text_color);
    appendLe32(payload, 0);
    if (has_text) {
        appendBytes(payload, text_utf16);
    } else {
        payload.push_back(0xa0);
        payload.push_back(0x05);
        payload.push_back(0x20);
    }
    return payload;
}

Bytes buildBlackscreenMessage(const std::string& student_ip, bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color) {
    Bytes out;
    appendLe32(out, kMess);
    appendLe32(out, 1);
    appendLe32(out, 1);
    appendBytes(out, ipv4Bytes(student_ip));
    appendBytes(out, buildBlackscreenMessPayload(lock_input, timeout_seconds, text, text_color));
    return out;
}

Bytes buildUnlockMessage(const std::string& student_ip) {
    Bytes payload;
    appendLe32(payload, 0x0d);
    appendLe32(payload, 0x20);
    appendLe32(payload, 0x90000000);
    payload.push_back(0x01);

    Bytes out;
    appendLe32(out, kMess);
    appendLe32(out, 1);
    appendLe32(out, 1);
    appendBytes(out, ipv4Bytes(student_ip));
    appendBytes(out, payload);
    return out;
}

Bytes buildAnnoShort() {
    Bytes out;
    appendLe32(out, kAnno);
    appendLe32(out, 1);
    return out;
}

Bytes buildAnnoLong(const std::string& teacher_ip) {
    Bytes out;
    appendLe32(out, kAnno);
    appendLe32(out, 1);
    appendLe32(out, 1);
    out.insert(out.end(), 8, 0);
    appendBytes(out, ipv4Bytes(teacher_ip));
    appendLe32(out, 0x0D5AD030);
    appendLe32(out, 0);
    appendLe32(out, 0x0D5AD030);
    appendLe32(out, 1);
    out.insert(out.end(), 32, 0);
    return out;
}

Bytes buildRemoteViewPayload(const std::string& teacher_ip, const std::string& student_ip, std::uint16_t local_session_port, std::uint16_t peer_session_port, std::uint32_t tcp_mode) {
    Bytes params(604, 0);
    const auto teacher = ipv4Bytes(teacher_ip);
    const auto student = ipv4Bytes(student_ip);
    std::copy(teacher.begin(), teacher.end(), params.begin() + 4);
    params[8] = static_cast<std::uint8_t>(local_session_port & 0xff);
    params[9] = static_cast<std::uint8_t>((local_session_port >> 8) & 0xff);
    std::copy(teacher.begin(), teacher.end(), params.begin() + 10);
    params[14] = static_cast<std::uint8_t>(peer_session_port & 0xff);
    params[15] = static_cast<std::uint8_t>((peer_session_port >> 8) & 0xff);
    const std::array<std::uint32_t, 8> settings{ 1, 1, 20480, 1, 1440, 25, 75, tcp_mode };
    for (std::size_t i = 0; i < settings.size(); ++i) {
        const auto value = settings[i];
        const auto at = 36 + i * 4;
        params[at + 0] = static_cast<std::uint8_t>(value & 0xff);
        params[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
        params[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
        params[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    }
    std::copy(student.begin(), student.end(), params.begin() + 68);
    const std::array<std::uint32_t, 3> tail{ 5, 12, 16 };
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const auto value = tail[i];
        const auto at = 72 + i * 4;
        params[at + 0] = static_cast<std::uint8_t>(value & 0xff);
        params[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
        params[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
        params[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    }
    Bytes payload;
    appendLe32(payload, 617);
    appendLe32(payload, 8);
    appendLe32(payload, 0x80000000U);
    appendBytes(payload, params);
    payload.push_back(0);
    return payload;
}

Bytes buildRemoteViewStopPayload() {
    Bytes payload;
    appendLe32(payload, 13);
    appendLe32(payload, 0);
    appendLe32(payload, 0);
    payload.push_back(0);
    return payload;
}

Bytes buildCommandTransaction(const Bytes& payload, const std::string& teacher_ip) {
    if (payload.size() > 0x800) {
        throw std::invalid_argument("command transaction payload exceeds 2048 bytes");
    }
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        throw std::runtime_error("CoCreateGuid failed");
    }
    Bytes out;
    appendLe32(out, kComd);
    appendLe32(out, 0x10000);
    appendLe32(out, static_cast<std::uint32_t>(payload.size() + 13));
    appendRaw(out, reinterpret_cast<const std::uint8_t*>(&guid), sizeof(guid));
    appendLe32(out, 20000);
    appendBytes(out, ipv4Bytes(teacher_ip));
    appendLe32(out, static_cast<std::uint32_t>(payload.size()));
    appendBytes(out, payload);
    out.push_back(0);
    return out;
}

Bytes buildRemoteControlPayload(const std::string& student_ip, std::uint32_t mode, std::uint16_t port) {
    Bytes payload(53, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        payload[at + 0] = static_cast<std::uint8_t>(value & 0xff);
        payload[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
        payload[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
        payload[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    };
    put32(0, 53);
    put32(4, 8);
    put32(8, 0);
    payload[12] = 'M'; payload[13] = 'C'; payload[14] = 'M'; payload[15] = 'D';
    put32(16, mode);
    const auto student = ipv4Bytes(student_ip);
    std::copy(student.begin(), student.end(), payload.begin() + 20);
    payload[24] = static_cast<std::uint8_t>(port & 0xff);
    payload[25] = static_cast<std::uint8_t>((port >> 8) & 0xff);
    return payload;
}

Bytes buildRemoteInputPacket(std::uint32_t kind, const Bytes& payload) {
    if (payload.size() != 20) {
        throw std::invalid_argument("remote input payload must be 20 bytes");
    }
    Bytes packet;
    appendLe32(packet, 0x2321347c);
    appendLe32(packet, kind);
    appendBytes(packet, payload);
    return packet;
}

Bytes buildRemoteMousePacket(std::uint32_t message, std::uint32_t x, std::uint32_t y, std::uint32_t data, std::uint32_t swapped) {
    Bytes payload;
    appendLe32(payload, message);
    appendLe32(payload, x);
    appendLe32(payload, y);
    appendLe32(payload, data);
    appendLe32(payload, swapped);
    return buildRemoteInputPacket(1, payload);
}

Bytes buildRemoteKeyPacket(std::uint16_t scan_code, std::uint16_t virtual_key, bool key_up, bool extended) {
    Bytes payload;
    appendLe16(payload, scan_code);
    appendLe16(payload, virtual_key);
    appendLe32(payload, (extended ? 1U : 0U) | (key_up ? 0x80U : 0U));
    payload.resize(20, 0);
    return buildRemoteInputPacket(16, payload);
}

} // namespace jiyu::protocol
