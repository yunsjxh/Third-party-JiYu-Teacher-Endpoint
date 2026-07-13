#pragma once

#include <filesystem>
#include <string>

namespace jiyu::image {

bool saveFixedPreviewJpeg(const std::filesystem::path& raw_path, const std::filesystem::path& fixed_path, std::string* error_message = nullptr);

} // namespace jiyu::image
