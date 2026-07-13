#include "KImage.h"

#include "Fl_BMP_Image.H"
#include "Fl_Image.H"
#include "Fl_JPEG_Image.H"
#include "Fl_RGB_Image.H"
#include "Fl_Shared_Image.H"
#include "fl_utf8.h"

#include <cstdio>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

// Cache entries own one FLTK shared-image reference and keep failed-load state.
struct KImageResourceCacheEntry {
    // Initializes an empty cache entry; callers fill path, image, dimensions, and error.
    KImageResourceCacheEntry()
        : path(),
          shared_image(nullptr),
          width(0),
          height(0),
          error() {
    }

    // Releases the FLTK shared-image reference held by this cache entry.
    ~KImageResourceCacheEntry() {
        if (shared_image) {
            shared_image->release();
            shared_image = nullptr;
        }
    }

    KImageResourceCacheEntry(const KImageResourceCacheEntry&) = delete;
    KImageResourceCacheEntry& operator=(const KImageResourceCacheEntry&) = delete;

    std::string path;
    Fl_Shared_Image* shared_image;
    int width;
    int height;
    std::string error;
};

namespace {
// ImageCache stores both successful and failed loads so repeated paths avoid I/O.
using ImageCache = std::map<std::string, std::shared_ptr<KImageResourceCacheEntry>>;

// EmptyString returns a stable reference for empty path/error accessors.
const std::string& EmptyString() {
    static const std::string empty;
    return empty;
}

// CacheMutex serializes access to the process-wide image cache and FLTK image loader.
std::mutex& CacheMutex() {
    static std::mutex mutex;
    return mutex;
}

// Cache returns the process-wide path-to-entry map used by KImageResource.
ImageCache& Cache() {
    static ImageCache cache;
    return cache;
}

// RegisterFltkImageHandlers enables FLTK's built-in PNG/JPEG/BMP/GIF handlers once.
void RegisterFltkImageHandlers() {
    static std::once_flag register_once;
    std::call_once(register_once, []() {
        fl_register_images();
    });
}

std::string FltkFailureText(int code);

// CanReadFile checks basic file accessibility before FLTK tries to decode it.
// FLTK file APIs use UTF-8 paths on Windows, unlike std::ifstream(char*), so
// use fl_fopen() here to avoid rejecting valid paths that contain CJK text.
bool CanReadFile(const std::string& path) {
    FILE* file = fl_fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    std::fclose(file);
    return true;
}

bool HasSuffixNoCase(const std::string& value, const char* suffix) {
    if (!suffix) {
        return false;
    }
    const std::string tail(suffix);
    if (value.size() < tail.size()) {
        return false;
    }
    const std::size_t offset = value.size() - tail.size();
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(value[offset + i]);
        const unsigned char b = static_cast<unsigned char>(tail[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::vector<unsigned char> ReadFileBytesUtf8(const std::string& path) {
    std::vector<unsigned char> data;
    FILE* file = fl_fopen(path.c_str(), "rb");
    if (!file) {
        return data;
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return data;
    }
    const long size = std::ftell(file);
    if (size <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return data;
    }

    data.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (read != data.size()) {
        data.clear();
    }
    return data;
}

// LoadSharedImageFromMemory is a Windows/Unicode-path fallback for formats that
// FLTK exposes memory constructors for. The preview pipeline writes JPEG files,
// so this prevents a valid UTF-8 path from failing if a lower FLTK loader still
// takes the narrow CRT fopen() path internally.
Fl_Shared_Image* LoadSharedImageFromMemory(const std::string& path, std::string* error) {
    if (!HasSuffixNoCase(path, ".jpg") && !HasSuffixNoCase(path, ".jpeg") && !HasSuffixNoCase(path, ".bmp")) {
        if (error) {
            *error = "No memory-loader fallback for this image extension.";
        }
        return nullptr;
    }

    const std::vector<unsigned char> data = ReadFileBytesUtf8(path);
    if (data.empty()) {
        if (error) {
            *error = "Could not read image bytes through UTF-8 path.";
        }
        return nullptr;
    }

    std::unique_ptr<Fl_RGB_Image> decoded;
    if (HasSuffixNoCase(path, ".jpg") || HasSuffixNoCase(path, ".jpeg")) {
        decoded.reset(new Fl_JPEG_Image(path.c_str(), data.data(), static_cast<int>(data.size())));
    }
    else {
        decoded.reset(new Fl_BMP_Image(path.c_str(), data.data(), static_cast<long>(data.size())));
    }

    if (!decoded) {
        if (error) {
            *error = "FLTK memory image constructor returned null.";
        }
        return nullptr;
    }
    if (decoded->fail() != 0) {
        if (error) {
            *error = FltkFailureText(decoded->fail());
        }
        return nullptr;
    }
    if (decoded->w() <= 0 || decoded->h() <= 0) {
        if (error) {
            *error = "Image memory decode produced invalid dimensions.";
        }
        return nullptr;
    }

    return Fl_Shared_Image::get(decoded.release(), 1);
}

// FltkFailureText converts FLTK image error codes into stable user-facing text.
std::string FltkFailureText(int code) {
    switch (code) {
    case Fl_Image::ERR_NO_IMAGE:
        return "FLTK did not create image data.";
    case Fl_Image::ERR_FILE_ACCESS:
        return "FLTK could not access the image file.";
    case Fl_Image::ERR_FORMAT:
        return "FLTK does not recognize the image format.";
    case Fl_Image::ERR_MEMORY_ACCESS:
        return "FLTK could not allocate or access image memory.";
    default:
        break;
    }

    std::ostringstream message;
    message << "FLTK image loader failed with error code " << code << ".";
    return message.str();
}

// MakeFailureEntry creates a cached failure entry without throwing exceptions.
std::shared_ptr<KImageResourceCacheEntry> MakeFailureEntry(const std::string& path, const std::string& error) {
    auto entry = std::make_shared<KImageResourceCacheEntry>();
    entry->path = path;
    entry->error = error;
    return entry;
}

// LoadEntry decodes one image path through FLTK and records dimensions or error text.
std::shared_ptr<KImageResourceCacheEntry> LoadEntry(const std::string& path) {
    if (path.empty()) {
        return MakeFailureEntry(path, "Image path is empty.");
    }

    // The wrapper is file-path based, so report an access error before decoding.
    if (!CanReadFile(path)) {
        return MakeFailureEntry(path, "Image file not found or not accessible: " + path);
    }

    RegisterFltkImageHandlers();

    Fl_Shared_Image* image = Fl_Shared_Image::get(path.c_str());
    std::string primary_error;
    if (!image) {
        primary_error = "FLTK could not load image file by path.";
    }
    else {
        // FLTK can return an image object with a failure code; release that reference.
        const int load_error = image->fail();
        if (load_error != 0) {
            primary_error = FltkFailureText(load_error);
            image->release();
            image = nullptr;
        }
    }

    if (!image) {
        std::string fallback_error;
        image = LoadSharedImageFromMemory(path, &fallback_error);
        if (!image) {
            std::string error = primary_error.empty() ? "FLTK could not load image file." : primary_error;
            if (!fallback_error.empty()) {
                error += " Fallback: " + fallback_error;
            }
            return MakeFailureEntry(path, error + " Path: " + path);
        }
    }

    // Zero-sized images are not drawable in this GUI layer and are treated as invalid.
    if (image->w() <= 0 || image->h() <= 0) {
        image->release();
        return MakeFailureEntry(path, "Image loaded with invalid dimensions: " + path);
    }

    auto entry = std::make_shared<KImageResourceCacheEntry>();
    entry->path = path;
    entry->shared_image = image;
    entry->width = image->w();
    entry->height = image->h();
    return entry;
}

// GetOrLoadEntry returns a cached entry or loads and caches a new entry for path.
std::shared_ptr<KImageResourceCacheEntry> GetOrLoadEntry(const std::string& path) {
    std::lock_guard<std::mutex> lock(CacheMutex());

    ImageCache& cache = Cache();
    auto found = cache.find(path);
    if (found != cache.end()) {
        return found->second;
    }

    std::shared_ptr<KImageResourceCacheEntry> entry = LoadEntry(path);
    cache[path] = entry;
    return entry;
}
} // namespace

KImageResource::KImageResource()
    : entry_() {
}

KImageResource::KImageResource(const std::string& path)
    : entry_() {
    loadFromFile(path);
}

void KImageResource::reset() {
    entry_.reset();
}

KImageResource KImageResource::Load(const std::string& path) {
    KImageResource resource;
    resource.entry_ = GetOrLoadEntry(path);
    return resource;
}

KImageResource KImageResource::FromFile(const std::string& path) {
    // Legacy factory spelling shares the same cache path as Load(); no duplicate decode occurs.
    return Load(path);
}

KImageResource KImageResource::from_file(const std::string& path) {
    // Snake_case factory spelling supports newer adapter code while preserving cache semantics.
    return Load(path);
}

bool KImageResource::loadFromFile(const std::string& path) {
    entry_ = GetOrLoadEntry(path);
    return valid();
}

bool KImageResource::load_from_file(const std::string& path) {
    // Snake_case loader delegates to the canonical camelCase API and returns its success flag.
    return loadFromFile(path);
}

const std::string& KImageResource::path() const {
    if (!entry_) {
        return EmptyString();
    }
    return entry_->path;
}

int KImageResource::width() const {
    if (!valid()) {
        return 0;
    }
    return entry_->width;
}

int KImageResource::height() const {
    if (!valid()) {
        return 0;
    }
    return entry_->height;
}

bool KImageResource::valid() const {
    return entry_ && entry_->shared_image && entry_->error.empty();
}

const std::string& KImageResource::error() const {
    if (!entry_) {
        return EmptyString();
    }
    return entry_->error;
}

Fl_Image* KImageResource::image() const {
    return valid() ? entry_->shared_image : nullptr;
}

Fl_Shared_Image* KImageResource::sharedImage() const {
    return valid() ? entry_->shared_image : nullptr;
}

bool KImageResource::cached(const std::string& path) {
    std::lock_guard<std::mutex> lock(CacheMutex());
    return Cache().find(path) != Cache().end();
}

void KImageResource::removeFromCache(const std::string& path) {
    std::lock_guard<std::mutex> lock(CacheMutex());
    Cache().erase(path);
}

void KImageResource::clearCache() {
    std::lock_guard<std::mutex> lock(CacheMutex());
    Cache().clear();
}
