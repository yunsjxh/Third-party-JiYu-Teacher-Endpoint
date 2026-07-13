#include "image_fix.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#pragma comment(lib, "gdiplus.lib")

namespace jiyu::image {
namespace {

class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok) {
            token_ = 0;
        }
    }
    ~GdiplusSession() {
        if (token_ != 0) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }
    bool ok() const { return token_ != 0; }
private:
    ULONG_PTR token_ = 0;
};

GdiplusSession& session() {
    static GdiplusSession s;
    return s;
}

int encoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT num = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) {
        return -1;
    }
    std::vector<std::uint8_t> storage(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(num, size, encoders) != Gdiplus::Ok) {
        return -1;
    }
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(encoders[i].MimeType, format) == 0) {
            *clsid = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

BYTE clampByte(double value) {
    value = std::round(std::clamp(value, 0.0, 255.0));
    return static_cast<BYTE>(value);
}

Gdiplus::Color fixColor(const Gdiplus::Color& input) {
    const double r = input.GetR();
    const double g = input.GetG();
    const double b = input.GetB();

    const double y = 0.299 * r + 0.587 * g + 0.114 * b;
    double cb = 128.0 - 0.168736 * r - 0.331264 * g + 0.5 * b;
    double cr = 128.0 + 0.5 * r - 0.418688 * g - 0.081312 * b;
    cb = 255.0 - cb;
    cr = 255.0 - cr;

    const double rr = y + 1.402 * (cr - 128.0);
    const double gg = y - 0.344136 * (cb - 128.0) - 0.714136 * (cr - 128.0);
    const double bb = y + 1.772 * (cb - 128.0);
    return Gdiplus::Color(input.GetA(), clampByte(rr), clampByte(gg), clampByte(bb));
}

void setError(std::string* out, const std::string& text) {
    if (out) {
        *out = text;
    }
}

} // namespace

bool saveFixedPreviewJpeg(const std::filesystem::path& raw_path, const std::filesystem::path& fixed_path, std::string* error_message) {
    if (!session().ok()) {
        setError(error_message, "GDI+ startup failed");
        return false;
    }

    Gdiplus::Bitmap source(raw_path.wstring().c_str(), FALSE);
    if (source.GetLastStatus() != Gdiplus::Ok) {
        setError(error_message, "failed to load raw JPEG");
        return false;
    }

    const UINT width = source.GetWidth();
    const UINT height = source.GetHeight();
    if (width == 0 || height == 0) {
        setError(error_message, "raw JPEG has empty dimensions");
        return false;
    }

    Gdiplus::Bitmap fixed(width, height, PixelFormat24bppRGB);
    if (fixed.GetLastStatus() != Gdiplus::Ok) {
        setError(error_message, "failed to allocate fixed bitmap");
        return false;
    }

    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            Gdiplus::Color color;
            source.GetPixel(x, height - 1 - y, &color);
            fixed.SetPixel(x, y, fixColor(color));
        }
    }

    CLSID jpg;
    if (encoderClsid(L"image/jpeg", &jpg) < 0) {
        setError(error_message, "JPEG encoder not found");
        return false;
    }

    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    ULONG quality = 95;
    params.Parameter[0].Value = &quality;

    const auto status = fixed.Save(fixed_path.wstring().c_str(), &jpg, &params);
    if (status != Gdiplus::Ok) {
        setError(error_message, "failed to save fixed JPEG");
        return false;
    }
    return true;
}

} // namespace jiyu::image

