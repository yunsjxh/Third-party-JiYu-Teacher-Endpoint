#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jiyu::protocol {

using Bytes = std::vector<std::uint8_t>;

inline constexpr const char* kMainMulticast = "224.50.50.42";
inline constexpr unsigned short kMainPort = 4705;
inline constexpr const char* kSessionMulticast = "225.2.2.1";
inline constexpr unsigned short kSessionPort = 5512;

inline constexpr std::uint32_t kOonc = 0x434E4F4F;
inline constexpr std::uint32_t kCanc = 0x434E4143;
inline constexpr std::uint32_t kWaca = 0x41434157;
inline constexpr std::uint32_t kTnrs = 0x53524E54;
inline constexpr std::uint32_t kComd = 0x434F4D44;
inline constexpr std::uint32_t kLpnt = 0x544E504C;
inline constexpr std::uint32_t kKaca = 0x4143414B;
inline constexpr std::uint32_t kTrmc = 0x434D5254;
inline constexpr std::uint32_t kTrnt = 0x544E5254;
inline constexpr std::uint32_t kDent = 0x544E4544;
inline constexpr std::uint32_t kLant = 0x544E414C;
inline constexpr std::uint32_t kAnno = 0x4F4E4E41;
inline constexpr std::uint32_t kLogi = 0x49474F4C;
inline constexpr std::uint32_t kMess = 0x5353454D;

std::string magicName(std::uint32_t magic);
std::uint32_t readLe32(const std::uint8_t* data);
std::uint32_t readLe32(const Bytes& data, std::size_t offset);
std::string hexdump(const std::uint8_t* data, std::size_t size, std::size_t width = 16);
std::string hexdump(const Bytes& data, std::size_t width = 16);

Bytes ipv4Bytes(const std::string& ip);
Bytes utf16LeZ(const std::string& utf8_text);
std::string utf16LeToUtf8(const std::uint8_t* data, std::size_t size);

Bytes buildOonc(const std::string& teacher_ip, std::uint32_t sequence);
Bytes buildCanc(const std::string& teacher_ip);
Bytes buildWaca(const std::string& teacher_ip);
Bytes buildTnrsRequest();
Bytes buildDentAck();
Bytes buildDmoc(const std::string& teacher_ip);
Bytes buildLpntSubtype2();
Bytes buildLpntSubtype3();
Bytes buildLoginMessType1000(const std::string& student_ip);
Bytes buildLoginMessType8000(const std::string& student_ip);
Bytes buildChatMessage(const std::string& student_ip, const std::string& text);
Bytes buildInfoRequestMessage(const std::string& student_ip, std::uint32_t report_type);
Bytes buildKillMessage(const std::string& student_ip, std::uint32_t pid, std::uint32_t hwnd, bool force);
Bytes buildComdCommandEx(std::uint32_t cmd_code, const Bytes& payload, const Bytes& guid, const Bytes& extra_header, std::uint32_t command_id);
Bytes buildComdLock(bool lock, std::uint32_t command_id);
Bytes buildShutdownCommand(bool reboot, std::uint32_t delay_seconds, bool force, const std::string& text, std::uint32_t command_id);
Bytes buildOpenUrlCommand(const std::string& url, std::uint32_t command_id);
Bytes buildRunProgramCommand(const std::string& path, const std::string& args, std::uint32_t show_mode, bool fallback, std::uint32_t command_id);
Bytes buildBlackscreenMessPayload(bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color = 0x0000FFFF);
Bytes buildBlackscreenMessage(const std::string& student_ip, bool lock_input, std::uint32_t timeout_seconds, const std::string& text, std::uint32_t text_color = 0x0000FFFF);
Bytes buildUnlockMessage(const std::string& student_ip);
Bytes buildAnnoShort();
Bytes buildAnnoLong(const std::string& teacher_ip);

} // namespace jiyu::protocol
