#include "protocol.hpp"
#include "preview_reassembler.hpp"

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> makeLant(std::uint32_t total, std::uint32_t offset, const std::vector<std::uint8_t>& frag) {
    std::vector<std::uint8_t> packet(48, 0);
    auto put = [&](std::size_t at, std::uint32_t v) {
        packet[at + 0] = static_cast<std::uint8_t>(v & 0xff);
        packet[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
        packet[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
        packet[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
    };
    put(0, jiyu::protocol::kLant);
    put(36, total);
    put(40, offset);
    put(44, static_cast<std::uint32_t>(frag.size()));
    packet.insert(packet.end(), frag.begin(), frag.end());
    return packet;
}

void testPackets() {
    using namespace jiyu::protocol;
    const std::string ip = "192.168.2.203";

    auto oonc = buildOonc(ip, 7);
    expect(oonc.size() == 44, "OONC total size");
    expect(readLe32(oonc, 0) == kOonc, "OONC magic");
    expect(readLe32(oonc, 8) == 16, "OONC len");
    expect(readLe32(oonc, 40) == 7, "OONC seq offset");

    auto canc = buildCanc(ip);
    expect(canc.size() == 120, "CANC total size");
    expect(readLe32(canc, 8) == 84, "CANC len");
    expect(readLe32(canc, 28) == 0x00020001, "CANC af");
    expect(canc[32] == 192 && canc[35] == 203, "CANC ip offset");

    auto waca = buildWaca(ip);
    expect(waca.size() == 36, "WACA total size");
    expect(readLe32(waca, 0) == kWaca, "WACA magic");

    auto tnrs = buildTnrsRequest();
    expect(tnrs.size() == 44, "TNRS total size");
    expect(readLe32(tnrs, 28) == 0x48, "TNRS tail 0x48");
    expect(readLe32(tnrs, 40) == 0x100, "TNRS tail 0x100");

    auto lpnt3 = buildLpntSubtype3();
    expect(lpnt3.size() == 48, "LPNT3 size");
    expect(readLe32(lpnt3, 28) == 3, "LPNT3 subtype");
    expect(readLe32(lpnt3, 32) == 1, "LPNT3 ready");

    auto dmoc = buildDmoc(ip);
    expect(dmoc.size() == 94, "DMOC total size");
    expect(readLe32(dmoc, 8) == 66, "DMOC len");
    expect(dmoc[32] == 192 && dmoc[35] == 203, "DMOC ip offset");

    auto lock = buildComdLock(true, 9);
    expect(lock.size() == 80, "COMD lock size");
    expect(readLe32(lock, 8) == 52, "COMD lock outer len");
    expect(readLe32(lock, 28) == 0x80000010, "COMD lock cmd code");
    expect(readLe32(lock, 44) == 0x200, "COMD lock subcmd");
    expect(readLe32(lock, 56) == 1, "COMD lock flag");

    auto chat = buildChatMessage("192.168.2.139", "A");
    expect(chat.size() == 36, "chat A total size");
    expect(readLe32(chat, 16) == 20, "chat payload len");
    expect(readLe32(chat, 20) == 0x800, "chat type");
    expect(readLe32(chat, 28) == 2, "chat wchar count includes nul");

    auto infoReq = buildInfoRequestMessage("192.168.2.139", 2);
    expect(infoReq.size() == 32, "info request total size");
    expect(readLe32(infoReq, 0) == kMess, "info request MESS magic");
    expect(readLe32(infoReq, 8) == 1, "info request recipient count");
    expect(readLe32(infoReq, 16) == 16, "info request payload len");
    expect(readLe32(infoReq, 20) == 0x100000, "info request type");
    expect(readLe32(infoReq, 28) == 2, "info request report type");

    auto killPid = buildKillMessage("192.168.2.139", 3280, 0, true);
    expect(killPid.size() == 44, "kill pid MESS size");
    expect(readLe32(killPid, 16) == 24, "kill pid python-compatible len field");
    expect(readLe32(killPid, 20) == 0x100000, "kill pid message type");
    expect(readLe32(killPid, 28) == 4, "kill pid report type 4");
    expect(readLe32(killPid, 36) == 3280, "kill pid field");
    expect(readLe32(killPid, 40) == 1, "kill pid force field");

    auto closeHwnd = buildKillMessage("192.168.2.139", 0, 0x00123456, false);
    expect(readLe32(closeHwnd, 28) == 3, "close app report type 3");
    expect(readLe32(closeHwnd, 32) == 0x00123456, "close app hwnd field");
    expect(readLe32(closeHwnd, 40) == 0, "close app force field");

    auto bs = buildBlackscreenMessage("192.168.2.139", true, 10, "");
    expect(bs.size() == 55, "black no text size");
    expect(readLe32(bs, 16) == 39, "black no text len field");
    expect(bs[52] == 0xa0 && bs[54] == 0x20, "black no text tail");

    auto bsText = buildBlackscreenMessage("192.168.2.139", true, 10, "A");
    expect(readLe32(bsText, 16) == 43, "black text Python-compatible len field");
    expect(bsText.size() == 56, "black text actual total is header + 36 + utf16z");

    auto bsTextColor = buildBlackscreenMessage("192.168.2.139", true, 10, "A", 0x000000FF);
    expect(readLe32(bsTextColor, 44) == 0x000000FF, "black custom COLORREF field");

    auto unlock = buildUnlockMessage("192.168.2.139");
    expect(unlock.size() == 29, "unlock size");
    expect(readLe32(unlock, 16) == 0x0d, "unlock payload len");
    expect(readLe32(unlock, 24) == 0x90000000, "unlock flag");

    auto shutdown = buildShutdownCommand(false, 0, true, "", 11);
    expect(shutdown.size() == 72, "shutdown command size without text");
    expect(readLe32(shutdown, 28) == 0x80000010, "shutdown command code");
    expect(readLe32(shutdown, 32) == 11, "shutdown command id");
    expect(readLe32(shutdown, 36) == 28, "shutdown payload len");
    expect(readLe32(shutdown, 44) == 0x200, "shutdown category");
    expect(readLe32(shutdown, 52) == 0x10000014, "shutdown force cmd id");
    expect(readLe32(shutdown, 56) == 0, "shutdown immediate delay");

    auto reboot = buildShutdownCommand(true, 30, false, "A", 12);
    expect(readLe32(reboot, 32) == 12, "reboot command id");
    expect(readLe32(reboot, 36) == 32, "reboot payload len includes utf16z + padding");
    expect(readLe32(reboot, 52) == 0x13, "reboot non-force cmd id");
    expect(readLe32(reboot, 56) == 30, "reboot delay");
    expect(reboot[68] == 'A' && reboot[69] == 0 && reboot[70] == 0 && reboot[71] == 0, "reboot text utf16z");

    auto openUrl = buildOpenUrlCommand("A", 13);
    expect(openUrl.size() == 68, "open url command size");
    expect(readLe32(openUrl, 32) == 13, "open url command id");
    expect(readLe32(openUrl, 36) == 24, "open url payload len");
    expect(readLe32(openUrl, 52) == 0x18, "open url cmd id");
    expect(openUrl[60] == 'A' && openUrl[61] == 0 && openUrl[62] == 0 && openUrl[63] == 0, "open url utf16z");

    auto run = buildRunProgramCommand("C:\\Windows\\notepad.exe", "C:\\a.txt", 2, true, 14);
    expect(run.size() == 900, "run program command size");
    expect(readLe32(run, 32) == 14, "run program command id");
    expect(readLe32(run, 36) == 856, "run program payload len");
    expect(readLe32(run, 52) == 0x0F, "run program cmd id");
    expect(readLe32(run, 56) == 1, "run program fallback");
    expect(run[60] == 'C' && run[61] == 0, "run program path utf16");
    expect(run[572] == 'C' && run[573] == 0, "run program args utf16");
    expect(readLe32(run, 892) == 2, "run program show mode");

    auto anno2 = buildAnnoLong(ip);
    expect(anno2.size() == 72, "ANNO long size");
}

void testPreviewReassembler() {
    jiyu::PreviewReassembler reassembler;
    const std::vector<std::uint8_t> expected{ 'A', 'B', 'C', 'D', 'E', 'F' };

    auto second = makeLant(6, 3, { 'D', 'E', 'F' });
    auto first = makeLant(6, 0, { 'A', 'B', 'C' });
    auto duplicate = makeLant(6, 3, { 'D', 'E', 'F' });

    auto done = reassembler.accept("10.0.0.2", second.data(), second.size());
    expect(!done.has_value(), "out-of-order second fragment incomplete");
    done = reassembler.accept("10.0.0.2", duplicate.data(), duplicate.size());
    expect(!done.has_value(), "duplicate fragment incomplete and deduped");
    done = reassembler.accept("10.0.0.2", first.data(), first.size());
    expect(done.has_value(), "complete after first fragment");
    expect(done->jpeg == expected, "reassembled bytes match");
}

} // namespace

int main() {
    testPackets();
    testPreviewReassembler();
    std::cout << "protocol self-tests passed\n";
    return 0;
}
