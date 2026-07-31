#include "remote_view.hpp"

#include "protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objidl.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace jiyu {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kUmspVersion = 0x10000;
constexpr std::uint32_t kUmspSelectMagic = 0x4f434853;
constexpr std::uint32_t kUmspDataMagic = 0x43504b54;
constexpr std::uint32_t kDeskChannel = 10;
constexpr std::uint32_t kMaxFrameSize = 0x241800;

std::string socketError(const char* action) {
    std::ostringstream out;
    out << action << " failed, WSA=" << WSAGetLastError();
    return out.str();
}

std::string timestampTag() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &value);
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S");
    return out.str();
}

std::string filenameIp(std::string ip) {
    std::replace(ip.begin(), ip.end(), '.', '_');
    return ip;
}

bool waitForConnect(SOCKET socket, const sockaddr_in& address, std::chrono::milliseconds timeout, std::string& error) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        error = socketError("ioctlsocket");
        return false;
    }
    const int result = ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result != 0) {
        const int code = WSAGetLastError();
        if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) {
            error = "connect failed, WSA=" + std::to_string(code);
            return false;
        }
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket, &write_set);
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        const int selected = select(0, nullptr, &write_set, nullptr, &tv);
        if (selected <= 0) {
            error = selected == 0 ? "TCP connect timeout" : socketError("select");
            return false;
        }
        int socket_error = 0;
        int socket_error_size = sizeof(socket_error);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &socket_error_size) != 0 || socket_error != 0) {
            error = "TCP connect failed, WSA=" + std::to_string(socket_error ? socket_error : WSAGetLastError());
            return false;
        }
    }
    nonblocking = 0;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        error = socketError("ioctlsocket blocking");
        return false;
    }
    return true;
}

SOCKET connectTcp(const std::string& local_ip, const std::string& remote_ip, std::uint16_t remote_port, std::chrono::milliseconds timeout, std::string& error) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        error = socketError("socket");
        return INVALID_SOCKET;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    if (inet_pton(AF_INET, local_ip.c_str(), &local.sin_addr) != 1 ||
        ::bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        error = socketError("bind");
        closesocket(socket);
        return INVALID_SOCKET;
    }
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, remote_ip.c_str(), &remote.sin_addr) != 1) {
        error = "invalid remote IPv4 address";
        closesocket(socket);
        return INVALID_SOCKET;
    }
    if (!waitForConnect(socket, remote, timeout, error)) {
        closesocket(socket);
        return INVALID_SOCKET;
    }
    return socket;
}

bool sendAll(SOCKET socket, const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int count = ::send(socket, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

class H264Decoder {
public:
    bool decode(const std::uint8_t* data, std::size_t size, int encoded_width, int encoded_height,
        int visible_width, int visible_height, std::uint64_t frame_index, RemoteFrame& frame, std::string& error) {
        if (!data || size == 0 || encoded_width <= 0 || encoded_height <= 0) {
            error = "invalid H.264 input";
            return false;
        }
        if (!transform_ || input_width_ != encoded_width || input_height_ != encoded_height) {
            if (!configure(encoded_width, encoded_height, error)) {
                return false;
            }
        }

        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
        if (FAILED(hr)) {
            error = "MFCreateMemoryBuffer failed";
            return false;
        }
        BYTE* target = nullptr;
        DWORD maximum = 0;
        hr = buffer->Lock(&target, &maximum, nullptr);
        if (FAILED(hr)) {
            error = "H.264 input buffer lock failed";
            return false;
        }
        std::memcpy(target, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(size));
        hr = MFCreateSample(&sample);
        if (FAILED(hr) || FAILED(sample->AddBuffer(buffer.Get()))) {
            error = "H.264 input sample creation failed";
            return false;
        }
        sample->SetSampleTime(static_cast<LONGLONG>(frame_index * 400000));
        sample->SetSampleDuration(400000);

        hr = transform_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            RemoteFrame ignored;
            std::string ignored_error;
            drain(encoded_width, encoded_height, visible_width, visible_height, ignored, ignored_error);
            hr = transform_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            std::ostringstream text;
            text << "H.264 ProcessInput failed: 0x" << std::hex << static_cast<unsigned long>(hr);
            error = text.str();
            return false;
        }
        return drain(encoded_width, encoded_height, visible_width, visible_height, frame, error);
    }

private:
    bool configure(int width, int height, std::string& error) {
        transform_.Reset();
        output_type_.Reset();
        HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform_));
        if (FAILED(hr)) {
            error = "Windows H.264 decoder is unavailable";
            return false;
        }
        ComPtr<IMFMediaType> input;
        if (FAILED(MFCreateMediaType(&input)) ||
            FAILED(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
            FAILED(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height)) ||
            FAILED(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 25, 1)) ||
            FAILED(MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(transform_->SetInputType(0, input.Get(), 0))) {
            error = "failed to configure H.264 input type";
            transform_.Reset();
            return false;
        }
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        input_width_ = width;
        input_height_ = height;
        return true;
    }

    bool selectOutputType(std::string& error) {
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            const HRESULT hr = transform_->GetOutputAvailableType(0, index, &type);
            if (hr == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (FAILED(hr)) {
                continue;
            }
            GUID subtype{};
            if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12 &&
                SUCCEEDED(transform_->SetOutputType(0, type.Get(), 0))) {
                output_type_ = type;
                return true;
            }
        }
        error = "H.264 decoder did not expose NV12 output";
        return false;
    }

    bool drain(int encoded_width, int encoded_height, int visible_width, int visible_height, RemoteFrame& frame, std::string& error) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            MFT_OUTPUT_STREAM_INFO info{};
            HRESULT hr = transform_->GetOutputStreamInfo(0, &info);
            if (FAILED(hr)) {
                error = "GetOutputStreamInfo failed";
                return false;
            }
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> allocation;
            if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
                const DWORD bytes = std::max<DWORD>(info.cbSize,
                    static_cast<DWORD>(static_cast<std::uint64_t>(encoded_width) * encoded_height * 3 / 2 + 4096));
                if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(bytes, &allocation)) ||
                    FAILED(sample->AddBuffer(allocation.Get()))) {
                    error = "failed to allocate H.264 output sample";
                    return false;
                }
                output.pSample = sample.Get();
            }
            DWORD status = 0;
            hr = transform_->ProcessOutput(0, 1, &output, &status);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
                if (!selectOutputType(error)) {
                    return false;
                }
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return false;
            }
            if (FAILED(hr)) {
                std::ostringstream text;
                text << "H.264 ProcessOutput failed: 0x" << std::hex << static_cast<unsigned long>(hr);
                error = text.str();
                return false;
            }
            ComPtr<IMFSample> produced = output.pSample ? output.pSample : sample.Get();
            if (!produced) {
                return false;
            }
            ComPtr<IMFMediaBuffer> contiguous;
            if (FAILED(produced->ConvertToContiguousBuffer(&contiguous))) {
                error = "failed to read H.264 output buffer";
                return false;
            }
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (FAILED(contiguous->Lock(&bytes, nullptr, &length))) {
                error = "failed to lock H.264 output buffer";
                return false;
            }
            UINT32 output_width = static_cast<UINT32>(encoded_width);
            UINT32 output_height = static_cast<UINT32>(encoded_height);
            if (output_type_) {
                MFGetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, &output_width, &output_height);
            }
            LONG stride = static_cast<LONG>(output_width);
            if (output_type_) {
                UINT32 raw_stride = 0;
                if (SUCCEEDED(output_type_->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
                    stride = static_cast<LONG>(raw_stride);
                }
            }
            const int absolute_stride = std::abs(stride);
            const int crop_width = std::clamp(visible_width, 1, static_cast<int>(output_width));
            const int crop_height = std::clamp(visible_height, 1, static_cast<int>(output_height));
            const std::size_t required = static_cast<std::size_t>(absolute_stride) * output_height * 3 / 2;
            if (absolute_stride < static_cast<int>(output_width) || length < required) {
                contiguous->Unlock();
                error = "unexpected NV12 buffer layout";
                return false;
            }
            frame.width = crop_width;
            frame.height = crop_height;
            frame.rgb.resize(static_cast<std::size_t>(crop_width) * crop_height * 3);
            const BYTE* y_plane = bytes;
            const BYTE* uv_plane = bytes + static_cast<std::size_t>(absolute_stride) * output_height;
            for (int y = 0; y < crop_height; ++y) {
                for (int x = 0; x < crop_width; ++x) {
                    const int yy = y_plane[static_cast<std::size_t>(y) * absolute_stride + x];
                    const std::size_t uv_at = static_cast<std::size_t>(y / 2) * absolute_stride + (x & ~1);
                    // JiYu's H.264 stream has U/V chroma planes reversed. NV12 is UV,
                    // so read the second byte as U and the first byte as V.
                    const int u = uv_plane[uv_at + 1];
                    const int v = uv_plane[uv_at + 0];
                    const int c = std::max(0, yy - 16);
                    const int d = u - 128;
                    const int e = v - 128;
                    const auto at = (static_cast<std::size_t>(y) * crop_width + x) * 3;
                    frame.rgb[at + 0] = clampByte((298 * c + 409 * e + 128) >> 8);
                    frame.rgb[at + 1] = clampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
                    frame.rgb[at + 2] = clampByte((298 * c + 516 * d + 128) >> 8);
                }
            }
            contiguous->Unlock();
            if (output.pEvents) {
                output.pEvents->Release();
            }
            return true;
        }
        error = "H.264 decoder output did not stabilize";
        return false;
    }

    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaType> output_type_;
    int input_width_ = 0;
    int input_height_ = 0;
};

bool decodeJpeg(const std::uint8_t* data, std::size_t size, RemoteFrame& frame, std::string& error) {
    if (!data || size == 0) {
        error = "empty JPEG";
        return false;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) {
        error = "GlobalAlloc failed";
        return false;
    }
    void* target = GlobalLock(memory);
    std::memcpy(target, data, size);
    GlobalUnlock(memory);
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        error = "CreateStreamOnHGlobal failed";
        return false;
    }
    Gdiplus::Bitmap bitmap(stream.Get());
    if (bitmap.GetLastStatus() != Gdiplus::Ok || bitmap.GetWidth() == 0 || bitmap.GetHeight() == 0) {
        error = "GDI+ could not decode JPEG";
        return false;
    }
    Gdiplus::Rect rect(0, 0, static_cast<INT>(bitmap.GetWidth()), static_cast<INT>(bitmap.GetHeight()));
    Gdiplus::BitmapData locked{};
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &locked) != Gdiplus::Ok) {
        error = "GDI+ JPEG LockBits failed";
        return false;
    }
    frame.width = rect.Width;
    frame.height = rect.Height;
    frame.rgb.resize(static_cast<std::size_t>(frame.width) * frame.height * 3);
    const auto* base = static_cast<const std::uint8_t*>(locked.Scan0);
    for (int y = 0; y < frame.height; ++y) {
        const int source_y = locked.Stride >= 0 ? y : frame.height - 1 - y;
        const auto* row = base + static_cast<std::ptrdiff_t>(source_y) * std::abs(locked.Stride);
        for (int x = 0; x < frame.width; ++x) {
            const auto source = static_cast<std::size_t>(x) * 3;
            const auto dest = (static_cast<std::size_t>(y) * frame.width + x) * 3;
            frame.rgb[dest + 0] = row[source + 2];
            frame.rgb[dest + 1] = row[source + 1];
            frame.rgb[dest + 2] = row[source + 0];
        }
    }
    bitmap.UnlockBits(&locked);
    return true;
}

} // namespace

struct RemoteViewSession::Impl {
    struct Fragment {
        std::uint32_t total = 0;
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> seen;
        std::uint32_t received = 0;
    };

    RemoteViewOptions options;
    RemoteViewCallbacks callbacks;
    std::atomic<bool> stopping{ false };
    std::atomic<bool> connected{ false };
    std::atomic<bool> finished{ false };
    std::atomic<std::uint64_t> frames{ 0 };
    std::atomic<SOCKET> socket{ INVALID_SOCKET };
    std::map<std::uint32_t, Fragment> fragments;
    std::unique_ptr<H264Decoder> decoder;
    std::string session_tag = timestampTag();
    std::thread thread;
};

RemoteViewSession::RemoteViewSession(RemoteViewOptions options, RemoteViewCallbacks callbacks)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->callbacks = std::move(callbacks);
}

RemoteViewSession::~RemoteViewSession() {
    stop();
    join();
}

void RemoteViewSession::start() {
    impl_->thread = std::thread([this] { run(); });
}

void RemoteViewSession::stop() {
    if (!impl_) {
        return;
    }
    impl_->stopping.store(true);
    closeSocket();
}

void RemoteViewSession::join() {
    if (impl_ && impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id()) {
        impl_->thread.join();
    }
}

bool RemoteViewSession::connected() const {
    return impl_ && impl_->connected.load();
}

bool RemoteViewSession::finished() const {
    return !impl_ || impl_->finished.load();
}

std::uint64_t RemoteViewSession::frameCount() const {
    return impl_ ? impl_->frames.load() : 0;
}

void RemoteViewSession::closeSocket() {
    const SOCKET socket = impl_->socket.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
    impl_->connected.store(false);
}

void RemoteViewSession::log(const std::string& level, const std::string& message) const {
    if (impl_->callbacks.log) {
        impl_->callbacks.log(level, message);
    }
}

void RemoteViewSession::run() {
    std::string failure;
    WSADATA winsock{};
    bool winsock_ready = WSAStartup(MAKEWORD(2, 2), &winsock) == 0;
    const HRESULT co_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_ready = SUCCEEDED(co_result) || co_result == RPC_E_CHANGED_MODE;
    const bool uninitialize_com = SUCCEEDED(co_result);
    const bool mf_ready = com_ready && SUCCEEDED(MFStartup(MF_VERSION));
    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartupInput gdiplus_input;
    const bool gdiplus_ready = Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) == Gdiplus::Ok;

    try {
        if (!winsock_ready) {
            throw std::runtime_error("Winsock initialization failed");
        }
        std::string probe_error;
        SOCKET probe = connectTcp(impl_->options.local_ip, impl_->options.student_ip,
            impl_->options.tcp_port, std::chrono::milliseconds(1500), probe_error);
        if (probe == INVALID_SOCKET) {
            throw std::runtime_error("TCP probe failed: " + probe_error);
        }
        closesocket(probe);

        if (!impl_->callbacks.request_feature_start || !impl_->callbacks.request_feature_start(failure)) {
            throw std::runtime_error(failure.empty() ? "remote-view feature request failed" : failure);
        }
        if (impl_->options.start_delay_ms != 0) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(impl_->options.start_delay_ms);
            while (!impl_->stopping.load() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (impl_->stopping.load()) {
            throw std::runtime_error("stopped");
        }
        std::string connect_error;
        SOCKET socket = connectTcp(impl_->options.local_ip, impl_->options.student_ip,
            impl_->options.tcp_port, std::chrono::milliseconds(2500), connect_error);
        if (socket == INVALID_SOCKET) {
            throw std::runtime_error(connect_error);
        }
        impl_->socket.store(socket);
        const std::array<std::uint32_t, 5> hello{ kUmspVersion, kUmspSelectMagic, 8, 1, kDeskChannel };
        if (!sendAll(socket, reinterpret_cast<const std::uint8_t*>(hello.data()), sizeof(hello))) {
            throw std::runtime_error(socketError("send SHCO"));
        }
        DWORD timeout = 1000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        impl_->connected.store(true);
        if (impl_->callbacks.connected) {
            impl_->callbacks.connected();
        }
        log("INFO", "TCP/UMSP connected; SHCO channel=10");

        std::vector<std::uint8_t> stream;
        std::array<std::uint8_t, 65536> receive{};
        while (!impl_->stopping.load()) {
            const int count = recv(socket, reinterpret_cast<char*>(receive.data()), static_cast<int>(receive.size()), 0);
            if (count == 0) {
                throw std::runtime_error("student closed the TCP connection");
            }
            if (count < 0) {
                const int code = WSAGetLastError();
                if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) {
                    continue;
                }
                if (impl_->stopping.load()) {
                    break;
                }
                throw std::runtime_error("TCP receive failed, WSA=" + std::to_string(code));
            }
            stream.insert(stream.end(), receive.begin(), receive.begin() + count);
            while (stream.size() >= 12) {
                const auto version = protocol::readLe32(stream.data());
                const auto magic = protocol::readLe32(stream.data() + 4);
                const auto payload_size = protocol::readLe32(stream.data() + 8);
                if (payload_size > kMaxFrameSize + 0x10000) {
                    throw std::runtime_error("UMSP payload exceeds safety limit");
                }
                const std::size_t packet_size = 12 + payload_size;
                if (stream.size() < packet_size) {
                    break;
                }
                if (version != kUmspVersion) {
                    log("WARN", "unexpected UMSP version");
                }
                if (magic == kUmspDataMagic && payload_size >= 4 && protocol::readLe32(stream.data() + 12) == kDeskChannel) {
                    handleFragment(stream.data() + 16, payload_size - 4);
                }
                stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(packet_size));
            }
        }
    } catch (const std::exception& error) {
        if (!impl_->stopping.load() && std::string(error.what()) != "stopped") {
            failure = error.what();
            log("ERROR", failure);
        }
    }

    closeSocket();
    impl_->decoder.reset();
    if (gdiplus_ready) {
        Gdiplus::GdiplusShutdown(gdiplus_token);
    }
    if (mf_ready) {
        MFShutdown();
    }
    if (uninitialize_com) {
        CoUninitialize();
    }
    if (winsock_ready) {
        WSACleanup();
    }
    if (impl_->callbacks.closed) {
        impl_->callbacks.closed(failure);
    }
    impl_->finished.store(true);
}

void RemoteViewSession::handleFragment(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 12) {
        log("WARN", "desktop fragment is shorter than 12 bytes");
        return;
    }
    const auto sequence = protocol::readLe32(data);
    const auto offset = protocol::readLe32(data + 4);
    const auto total = protocol::readLe32(data + 8);
    const auto fragment_size = size - 12;
    if (total == 0 || total > kMaxFrameSize || offset > total || fragment_size > total - offset) {
        log("WARN", "invalid desktop fragment bounds");
        return;
    }
    auto& state = impl_->fragments[sequence];
    if (state.total != total) {
        state.total = total;
        state.data.assign(total, 0);
        state.seen.assign(total, 0);
        state.received = 0;
    }
    for (std::size_t i = 0; i < fragment_size; ++i) {
        const std::size_t at = static_cast<std::size_t>(offset) + i;
        state.data[at] = data[12 + i];
        if (!state.seen[at]) {
            state.seen[at] = 1;
            ++state.received;
        }
    }
    while (impl_->fragments.size() > 4) {
        impl_->fragments.erase(impl_->fragments.begin());
    }
    if (state.received == state.total) {
        auto record = std::move(state.data);
        impl_->fragments.erase(sequence);
        handleRecord(sequence, record);
    }
}

void RemoteViewSession::handleRecord(std::uint32_t sequence, const std::vector<std::uint8_t>& record) {
    if (record.size() < 64) {
        if (record.size() >= 16 && std::memcmp(record.data(), "SPUC", 4) == 0 && protocol::readLe32(record.data() + 4) == 16) {
            const auto packed = protocol::readLe32(record.data() + 12);
            if (impl_->callbacks.cursor) {
                impl_->callbacks.cursor(static_cast<std::uint16_t>(packed & 0xffff), static_cast<std::uint16_t>((packed >> 16) & 0xffff));
            }
        }
        return;
    }
    const auto magic = protocol::readLe32(record.data());
    const auto readSigned = [&](std::size_t at) { return static_cast<std::int32_t>(protocol::readLe32(record.data() + at)); };
    const int encoded_width = std::abs(readSigned(20) - readSigned(12));
    const int encoded_height = std::abs(readSigned(24) - readSigned(16));
    int visible_width = std::abs(readSigned(36) - readSigned(28));
    int visible_height = std::abs(readSigned(40) - readSigned(32));
    const int fallback_width = encoded_width > 0 ? encoded_width : visible_width;
    const int fallback_height = encoded_height > 0 ? encoded_height : visible_height;
    if (fallback_width <= 0 || fallback_height <= 0) {
        log("WARN", "desktop record has no usable dimensions");
        return;
    }
    const int actual_encoded_width = fallback_width;
    const int actual_encoded_height = fallback_height;
    if (visible_width <= 0 || visible_width > actual_encoded_width) visible_width = actual_encoded_width;
    if (visible_height <= 0 || visible_height > actual_encoded_height) visible_height = actual_encoded_height;
    const auto* payload = record.data() + 64;
    const std::size_t payload_size = record.size() - 64;
    RemoteFrame frame;
    frame.student_ip = impl_->options.student_ip;
    frame.sequence = sequence;
    std::string error;
    bool decoded = false;

    if (magic == 0x46524848) {
        if (!impl_->decoder) {
            impl_->decoder = std::make_unique<H264Decoder>();
        }
        decoded = impl_->decoder->decode(payload, payload_size, actual_encoded_width, actual_encoded_height,
            visible_width, visible_height, impl_->frames.load(), frame, error);
        if (impl_->options.dump_h264 || !decoded) {
            try {
                std::filesystem::create_directories(impl_->options.output_dir);
                const auto path = impl_->options.output_dir /
                    ("remote_" + filenameIp(impl_->options.student_ip) + "_" + impl_->session_tag + ".h264");
                std::ofstream output(path, std::ios::binary | std::ios::app);
                output.write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(payload_size));
            } catch (...) {
            }
        }
    } else if (magic == 0x46524a48) {
        const std::array<std::uint8_t, 2> soi{ 0xff, 0xd8 };
        const auto begin = std::search(payload, payload + payload_size, soi.begin(), soi.end());
        const auto* jpeg = begin == payload + payload_size ? payload : begin;
        const auto jpeg_size = payload_size - static_cast<std::size_t>(jpeg - payload);
        decoded = decodeJpeg(jpeg, jpeg_size, frame, error);
        try {
            std::filesystem::create_directories(impl_->options.output_dir);
            const auto path = impl_->options.output_dir /
                ("remote_" + filenameIp(impl_->options.student_ip) + "_latest.jpg");
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(jpeg), static_cast<std::streamsize>(jpeg_size));
        } catch (...) {
        }
    } else if (magic != 0x46524d48) {
        log("WARN", "unknown desktop record magic");
        return;
    }

    const auto count = impl_->frames.fetch_add(1) + 1;
    if (decoded && impl_->callbacks.frame) {
        impl_->callbacks.frame(std::move(frame));
    } else if (!decoded && !error.empty() && (count == 1 || count % 100 == 0)) {
        log("WARN", "video frame decode pending/failed: " + error);
    }
}

} // namespace jiyu
